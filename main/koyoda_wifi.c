#include "koyoda_wifi.h"

#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#if __has_include("koyoda_wifi_secrets.h")
#include "koyoda_wifi_secrets.h"
#else
#define KOYODA_WIFI_SSID ""
#define KOYODA_WIFI_PASSWORD ""
#endif

static const char *TAG = "KOYODA_WIFI";

static volatile bool s_started = false;
static volatile bool s_connected = false;
static volatile int s_rssi = -127;

static esp_event_handler_instance_t s_wifi_handler = NULL;
static esp_event_handler_instance_t s_ip_handler = NULL;

#define WIFI_START_DELAY_MS 1200

static esp_err_t init_nvs_safe(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ret = nvs_flash_erase();
        if (ret != ESP_OK) return ret;
        ret = nvs_flash_init();
    }

    return ret;
}

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG, "Station started; connecting...");
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        s_connected = false;
        s_rssi = -127;

        ESP_LOGW(TAG, "Disconnected; reconnecting...");
        esp_wifi_connect();
        return;
    }

    if (event_base == IP_EVENT &&
        event_id == IP_EVENT_STA_GOT_IP)
    {
        const ip_event_got_ip_t *event =
            (const ip_event_got_ip_t *)event_data;

        s_connected = true;

        ESP_LOGI(
            TAG,
            "Connected; IP=" IPSTR,
            IP2STR(&event->ip_info.ip));

        wifi_ap_record_t ap = {0};

        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        {
            s_rssi = ap.rssi;

            ESP_LOGI(
                TAG,
                "SSID=%s RSSI=%d dBm",
                (const char *)ap.ssid,
                ap.rssi);
        }
    }
}

static void wifi_start_task(void *arg)
{
    (void)arg;

    /*
     * Let display/PMU/touch finish first.
     * KOYODA should be alive before networking starts.
     */
    vTaskDelay(pdMS_TO_TICKS(WIFI_START_DELAY_MS));

    if (KOYODA_WIFI_SSID[0] == '\0')
    {
        ESP_LOGW(
            TAG,
            "No Wi-Fi credentials supplied; KOYODA remains offline");
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    esp_err_t ret = init_nvs_safe();

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    ret = esp_netif_init();

    if (ret != ESP_OK &&
        ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    ret = esp_event_loop_create_default();

    if (ret != ESP_OK &&
        ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "event loop init failed: %s", esp_err_to_name(ret));
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    if (sta_netif == NULL)
    {
        ESP_LOGE(TAG, "Could not create default Wi-Fi STA netif");
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();

    ret = esp_wifi_init(&init_cfg);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    ret = esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_event_handler,
        NULL,
        &s_wifi_handler);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Wi-Fi event handler failed: %s", esp_err_to_name(ret));
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    ret = esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &wifi_event_handler,
        NULL,
        &s_ip_handler);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "IP event handler failed: %s", esp_err_to_name(ret));
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    wifi_config_t config = {0};

    strlcpy(
        (char *)config.sta.ssid,
        KOYODA_WIFI_SSID,
        sizeof(config.sta.ssid));

    strlcpy(
        (char *)config.sta.password,
        KOYODA_WIFI_PASSWORD,
        sizeof(config.sta.password));

    ret = esp_wifi_set_mode(WIFI_MODE_STA);

    if (ret == ESP_OK)
    {
        ret = esp_wifi_set_config(
            WIFI_IF_STA,
            &config);
    }

    if (ret == ESP_OK)
    {
        ret = esp_wifi_start();
    }

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Wi-Fi setup failed: %s", esp_err_to_name(ret));
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Wi-Fi background connection enabled");

    vTaskDelete(NULL);
}

esp_err_t koyoda_wifi_start(void)
{
    if (s_started)
    {
        return ESP_OK;
    }

    s_started = true;

    BaseType_t result = xTaskCreate(
        wifi_start_task,
        "wifi_start",
        4096,
        NULL,
        1,
        NULL);

    if (result != pdPASS)
    {
        s_started = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Wi-Fi startup scheduled; KOYODA UI remains independent");

    return ESP_OK;
}

bool koyoda_wifi_is_connected(void)
{
    return s_connected;
}

int koyoda_wifi_get_rssi(void)
{
    if (!s_connected)
    {
        return -127;
    }

    wifi_ap_record_t ap = {0};

    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
    {
        s_rssi = ap.rssi;
    }

    return s_rssi;
}
