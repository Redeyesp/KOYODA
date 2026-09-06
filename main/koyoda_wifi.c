#include "koyoda_wifi.h"
#include "koyoda_wifi_retry.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs_flash.h"

/* Generated inside CI; never commit this header. Other workflows continue
 * to build an offline pet when the header is absent. */
#if __has_include("koyoda_wifi_secrets.h")
#include "koyoda_wifi_secrets.h"
#else
#define KOYODA_WIFI_SSID ""
#define KOYODA_WIFI_PASSWORD ""
#endif

#define WIFI_START_DELAY_MS 2000U
#define WIFI_POLL_MS 250U
#define WIFI_RSSI_PERIOD_MS 10000U

static const char *TAG = "KOYODA_WIFI";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_start_requested;
static koyoda_wifi_status_t s_status = {
    .state = KOYODA_WIFI_DISABLED, .rssi = -127,
};

typedef enum { LINK_NONE, LINK_STARTED, LINK_DOWN, LINK_IP, LINK_LOST_IP, LINK_STOPPED } link_kind_t;
typedef struct {
    uint32_t sequence;
    link_kind_t kind;
    uint8_t reason;
    char ip[16];
} link_event_t;
static link_event_t s_link;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void publish_state(koyoda_wifi_state_t state, esp_err_t error, uint8_t reason)
{
    portENTER_CRITICAL(&s_lock);
    s_status.state = state;
    s_status.last_error = error;
    s_status.disconnect_reason = reason;
    if (state != KOYODA_WIFI_CONNECTED) {
        s_status.ip[0] = '\0';
        s_status.rssi = -127;
    }
    portEXIT_CRITICAL(&s_lock);
}

/* Callbacks only copy a bounded mailbox. No reconnect, LVGL, allocation,
 * or BSP display lock in the system event-loop task. */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    link_event_t event = {0};
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        event.kind = LINK_STARTED;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        event.kind = LINK_DOWN;
        if (data != NULL) event.reason = ((wifi_event_sta_disconnected_t *)data)->reason;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) {
        event.kind = LINK_STOPPED;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP && data != NULL) {
        const ip_event_got_ip_t *ip = data;
        if (ip->esp_netif != arg) return;
        event.kind = LINK_IP;
        snprintf(event.ip, sizeof(event.ip), IPSTR, IP2STR(&ip->ip_info.ip));
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        event.kind = LINK_LOST_IP;
    } else {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    event.sequence = s_link.sequence + 1;
    s_link = event;
    portEXIT_CRITICAL(&s_lock);
}

static void schedule_retry(koyoda_wifi_retry_t *retry, uint32_t now, esp_err_t error, uint8_t reason)
{
    /* A disconnect caused by our timeout must not postpone the retry twice. */
    uint32_t delay = wifi_retry_schedule(retry, now);
    if (delay == 0) return;
    publish_state(KOYODA_WIFI_RETRYING, error, reason);
    ESP_LOGW(TAG, "Offline; retry in %lu s (reason=%u, error=%s)",
             (unsigned long)(delay / 1000), (unsigned)reason, esp_err_to_name(error));
}

static void begin_connection(koyoda_wifi_retry_t *retry, uint32_t now)
{
    publish_state(KOYODA_WIFI_CONNECTING, ESP_OK, 0);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) schedule_retry(retry, now, err, 0);
}

static void wifi_worker(void *arg)
{
    (void)arg;
    esp_netif_t *netif = NULL;
    esp_event_handler_instance_t wifi_handler = NULL, ip_handler = NULL;
    bool driver_initialized = false, driver_started = false;
    esp_err_t err;
    vTaskDelay(pdMS_TO_TICKS(WIFI_START_DELAY_MS));

    /* Never erase the whole NVS partition to recover a Wi-Fi failure. */
    err = nvs_flash_init();
    if (err != ESP_OK) goto failed;
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto failed;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto failed;
    /* The convenience create_default_wifi_sta() helper aborts on failure.
     * Use its individual steps so low memory does not abort the UI. */
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_WIFI_STA();
    netif = esp_netif_new(&netif_config);
    if (netif == NULL) { err = ESP_ERR_NO_MEM; goto failed; }
    err = esp_netif_attach_wifi_station(netif);
    if (err != ESP_OK) goto failed;
    err = esp_wifi_set_default_wifi_sta_handlers();
    if (err != ESP_OK) goto failed;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) goto failed;
    driver_initialized = true;
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, netif, &wifi_handler);
    if (err != ESP_OK) goto failed;
    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, netif, &ip_handler);
    if (err != ESP_OK) goto failed;
    wifi_config_t config = {0};
    /* Copy all 32 SSID bytes / 64 PSK bytes when valid, without truncation. */
    memcpy(config.sta.ssid, KOYODA_WIFI_SSID, sizeof(KOYODA_WIFI_SSID) - 1);
    memcpy(config.sta.password, KOYODA_WIFI_PASSWORD, sizeof(KOYODA_WIFI_PASSWORD) - 1);
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) goto failed;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) goto failed;
    err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) goto failed;
    err = esp_wifi_start();
    if (err != ESP_OK) goto failed;
    driver_started = true;

    koyoda_wifi_retry_t retry = wifi_retry_init(now_ms());
    uint32_t seen_sequence = 0, rssi_checked = 0;
    ESP_LOGI(TAG, "Station ready; waiting for connection in background");
    for (;;) {
        uint32_t now = now_ms();
        link_event_t event;
        portENTER_CRITICAL(&s_lock);
        event = s_link;
        portEXIT_CRITICAL(&s_lock);
        if (event.sequence != seen_sequence) {
            seen_sequence = event.sequence;
            switch (event.kind) {
                case LINK_STARTED:
                    if (retry.phase == WIFI_RETRY_WAIT_START) {
                        wifi_retry_begin(&retry, now);
                        begin_connection(&retry, now);
                    }
                    break;
                case LINK_DOWN:
                    schedule_retry(&retry, now, ESP_OK, event.reason);
                    break;
                case LINK_LOST_IP:
                    if (retry.phase == WIFI_RETRY_CONNECTED) {
                        schedule_retry(&retry, now, ESP_ERR_TIMEOUT, 0);
                        (void)esp_wifi_disconnect();
                    }
                    break;
                case LINK_IP:
                    /* Ignore late DHCP after a timed-out attempt. */
                    if (wifi_retry_connected(&retry)) {
                        portENTER_CRITICAL(&s_lock);
                        s_status.state = KOYODA_WIFI_CONNECTED;
                        s_status.last_error = ESP_OK;
                        s_status.disconnect_reason = 0;
                        memcpy(s_status.ip, event.ip, sizeof(s_status.ip));
                        portEXIT_CRITICAL(&s_lock);
                        rssi_checked = now - WIFI_RSSI_PERIOD_MS;
                        ESP_LOGI(TAG, "Connected; IP=%s", event.ip);
                    }
                    break;
                case LINK_STOPPED:
                    err = ESP_ERR_INVALID_STATE;
                    goto failed;
                default:
                    break;
            }
        }
        wifi_retry_action_t action = wifi_retry_poll(&retry, now);
        if (action == WIFI_RETRY_CONNECT) {
            begin_connection(&retry, now);
        } else if (action == WIFI_RETRY_TIMEOUT) {
            schedule_retry(&retry, now, ESP_ERR_TIMEOUT, 0);
            (void)esp_wifi_disconnect();
        } else if (action == WIFI_RETRY_START_TIMEOUT) {
            err = ESP_ERR_TIMEOUT;
            goto failed;
        }
        if (retry.phase == WIFI_RETRY_CONNECTED &&
            (uint32_t)(now - rssi_checked) >= WIFI_RSSI_PERIOD_MS) {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                portENTER_CRITICAL(&s_lock);
                s_status.rssi = ap.rssi;
                portEXIT_CRITICAL(&s_lock);
            }
            rssi_checked = now;
        }
        vTaskDelay(pdMS_TO_TICKS(WIFI_POLL_MS));
    }
failed:
    publish_state(KOYODA_WIFI_ERROR, err, 0);
    ESP_LOGE(TAG, "Wi-Fi setup stopped: %s; pet remains running", esp_err_to_name(err));
    /* Undo only our resources. Global NVS/netif/event loop may be shared. */
    if (wifi_handler != NULL)
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler);
    if (ip_handler != NULL)
        (void)esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, ip_handler);
    if (driver_started) (void)esp_wifi_stop();
    if (driver_initialized) (void)esp_wifi_deinit();
    if (netif != NULL) esp_netif_destroy_default_wifi(netif);
    vTaskDelete(NULL);
}

esp_err_t koyoda_wifi_start(void)
{
    const size_t ssid_len = sizeof(KOYODA_WIFI_SSID) - 1;
    const size_t pass_len = sizeof(KOYODA_WIFI_PASSWORD) - 1;
    if (ssid_len == 0) {
        ESP_LOGI(TAG, "Wi-Fi disabled in this build");
        return ESP_OK;
    }
    if (ssid_len > 32 || pass_len < 8 || pass_len > 64) {
        publish_state(KOYODA_WIFI_ERROR, ESP_ERR_INVALID_ARG, 0);
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    if (s_start_requested) {
        esp_err_t result = s_status.state == KOYODA_WIFI_ERROR ? s_status.last_error : ESP_OK;
        portEXIT_CRITICAL(&s_lock);
        return result;
    }
    s_start_requested = true;
    s_status.state = KOYODA_WIFI_STARTING;
    portEXIT_CRITICAL(&s_lock);
    if (xTaskCreate(wifi_worker, "koyoda_wifi", 4096, NULL, 1, NULL) != pdPASS) {
        publish_state(KOYODA_WIFI_ERROR, ESP_ERR_NO_MEM, 0);
        portENTER_CRITICAL(&s_lock);
        s_start_requested = false;
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void koyoda_wifi_get_status(koyoda_wifi_status_t *out)
{
    if (out == NULL) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
}

bool koyoda_wifi_is_connected(void)
{
    koyoda_wifi_status_t status;
    koyoda_wifi_get_status(&status);
    return status.state == KOYODA_WIFI_CONNECTED;
}

int koyoda_wifi_get_rssi(void)
{
    koyoda_wifi_status_t status;
    koyoda_wifi_get_status(&status);
    return status.rssi;
}
