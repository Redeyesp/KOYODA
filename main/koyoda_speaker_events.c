#include "koyoda_speaker_events.h"

#include <stdint.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "nvs.h"

#include "bsp/esp-bsp.h"

static const char *TAG = "KOYODA_SPK_EVT";

#define SAMPLE_RATE_HZ              22050
#define CHUNK_SAMPLES                 128
#define DEFAULT_VOLUME_PERCENT         25
#define TONE_HZ                       660
#define TONE_MS                       120
#define START_DELAY_MS               3000

#define SPEAKER_EVT_CHARGE_BEEP   (1UL << 0)
#define SPEAKER_EVT_TEST_BEEP     (1UL << 1)
#define SPEAKER_EVT_VOLUME_CHANGE (1UL << 2)

#define NVS_NAMESPACE "koyoda_audio"
#define NVS_KEY_VOLUME "volume"

static TaskHandle_t s_task = NULL;
static esp_codec_dev_handle_t s_speaker = NULL;

static volatile int s_volume_percent = DEFAULT_VOLUME_PERCENT;

static int clamp_volume(int percent)
{
    if (percent < 0)
    {
        return 0;
    }

    if (percent > 100)
    {
        return 100;
    }

    return percent;
}

static void load_volume_from_nvs(void)
{
    nvs_handle_t handle = 0;

    esp_err_t err = nvs_open(
        NVS_NAMESPACE,
        NVS_READONLY,
        &handle);

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGI(
            TAG,
            "No saved volume yet; using default %d%%",
            DEFAULT_VOLUME_PERCENT);
        s_volume_percent = DEFAULT_VOLUME_PERCENT;
        return;
    }

    if (err != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Could not open volume NVS: %s; using default %d%%",
            esp_err_to_name(err),
            DEFAULT_VOLUME_PERCENT);
        s_volume_percent = DEFAULT_VOLUME_PERCENT;
        return;
    }

    uint8_t saved = DEFAULT_VOLUME_PERCENT;

    err = nvs_get_u8(
        handle,
        NVS_KEY_VOLUME,
        &saved);

    nvs_close(handle);

    if (err == ESP_OK)
    {
        s_volume_percent = clamp_volume((int)saved);
        ESP_LOGI(
            TAG,
            "Loaded saved volume: %d%%",
            (int)s_volume_percent);
    }
    else
    {
        s_volume_percent = DEFAULT_VOLUME_PERCENT;
        ESP_LOGI(
            TAG,
            "Volume key not set; using default %d%%",
            DEFAULT_VOLUME_PERCENT);
    }
}

static void save_volume_to_nvs(int percent)
{
    nvs_handle_t handle = 0;

    esp_err_t err = nvs_open(
        NVS_NAMESPACE,
        NVS_READWRITE,
        &handle);

    if (err != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Could not open NVS to save volume: %s",
            esp_err_to_name(err));
        return;
    }

    err = nvs_set_u8(
        handle,
        NVS_KEY_VOLUME,
        (uint8_t)clamp_volume(percent));

    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Could not save volume: %s",
            esp_err_to_name(err));
    }
    else
    {
        ESP_LOGI(TAG, "Saved volume: %d%%", percent);
    }
}

static void fill_square_tone(
    int16_t *samples,
    size_t count,
    uint32_t *phase)
{
    uint32_t period = SAMPLE_RATE_HZ / TONE_HZ;

    if (period < 2)
    {
        period = 2;
    }

    const int16_t amplitude = 3500;

    for (size_t i = 0; i < count; ++i)
    {
        uint32_t p = (*phase) % period;

        samples[i] =
            (p < (period / 2U))
                ? amplitude
                : (int16_t)-amplitude;

        (*phase)++;
    }
}

static bool play_one_beep(void)
{
    if (s_speaker == NULL)
    {
        return false;
    }

    int16_t samples[CHUNK_SAMPLES];
    uint32_t phase = 0;

    const uint32_t total_samples =
        (SAMPLE_RATE_HZ * TONE_MS) / 1000U;

    uint32_t sent = 0;

    while (sent < total_samples)
    {
        size_t chunk = CHUNK_SAMPLES;
        uint32_t remain = total_samples - sent;

        if (remain < chunk)
        {
            chunk = remain;
        }

        fill_square_tone(
            samples,
            chunk,
            &phase);

        int ret = esp_codec_dev_write(
            s_speaker,
            samples,
            chunk * sizeof(int16_t));

        if (ret != ESP_CODEC_DEV_OK)
        {
            ESP_LOGE(
                TAG,
                "esp_codec_dev_write failed: %d",
                ret);
            return false;
        }

        sent += chunk;
        taskYIELD();
    }

    return true;
}

static void apply_current_volume(void)
{
    if (s_speaker == NULL)
    {
        return;
    }

    const int volume =
        clamp_volume((int)s_volume_percent);

    int ret = esp_codec_dev_set_out_vol(
        s_speaker,
        volume);

    if (ret != ESP_CODEC_DEV_OK)
    {
        ESP_LOGW(
            TAG,
            "set volume %d%% returned: %d",
            volume,
            ret);
    }
    else
    {
        ESP_LOGI(TAG, "Speaker volume applied: %d%%", volume);
    }
}

static void speaker_event_task(void *arg)
{
    (void)arg;

    ESP_LOGI(
        TAG,
        "VOLUME STEP1 START: boot/charge/test beeps only; no repeating timer");

    /* Let display/Wi-Fi settle before codec init. */
    vTaskDelay(pdMS_TO_TICKS(START_DELAY_MS));

    s_speaker =
        bsp_audio_codec_speaker_init();

    if (s_speaker == NULL)
    {
        ESP_LOGE(
            TAG,
            "bsp_audio_codec_speaker_init returned NULL");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    esp_codec_dev_sample_info_t format = {
        .sample_rate = SAMPLE_RATE_HZ,
        .channel = 1,
        .bits_per_sample = 16,
    };

    int ret =
        esp_codec_dev_open(
            s_speaker,
            &format);

    if (ret != ESP_CODEC_DEV_OK)
    {
        ESP_LOGE(
            TAG,
            "esp_codec_dev_open failed: %d",
            ret);
        s_speaker = NULL;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    apply_current_volume();

    ESP_LOGI(
        TAG,
        "BOOT BEEP ONCE at %d%%",
        (int)s_volume_percent);

    play_one_beep();

    /*
     * Completely event-driven after the one boot beep.
     * No periodic beep loop exists.
     */
    while (1)
    {
        uint32_t events = 0;

        xTaskNotifyWait(
            0,
            UINT32_MAX,
            &events,
            portMAX_DELAY);

        /*
         * Apply volume first so a TEST requested together with a new
         * volume value uses the new level.
         */
        if (events & SPEAKER_EVT_VOLUME_CHANGE)
        {
            const int volume =
                clamp_volume((int)s_volume_percent);

            apply_current_volume();
            save_volume_to_nvs(volume);
        }

        if (events & SPEAKER_EVT_CHARGE_BEEP)
        {
            ESP_LOGI(
                TAG,
                "CHARGE INSERTION BEEP at %d%%",
                (int)s_volume_percent);
            play_one_beep();
        }

        if (events & SPEAKER_EVT_TEST_BEEP)
        {
            ESP_LOGI(
                TAG,
                "TEST BEEP at %d%%",
                (int)s_volume_percent);
            play_one_beep();
        }
    }
}

esp_err_t koyoda_speaker_events_start(void)
{
    if (s_task != NULL)
    {
        return ESP_OK;
    }

    /*
     * Wi-Fi is started before this module in app_main and initializes NVS.
     * If the namespace/key is missing, load_volume_from_nvs simply falls
     * back to the safe 25% default.
     */
    load_volume_from_nvs();

    BaseType_t result = xTaskCreate(
        speaker_event_task,
        "speaker_events",
        4096,
        NULL,
        2,
        &s_task);

    if (result != pdPASS)
    {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void koyoda_speaker_events_beep_charge(void)
{
    TaskHandle_t task = s_task;

    if (task != NULL)
    {
        xTaskNotify(
            task,
            SPEAKER_EVT_CHARGE_BEEP,
            eSetBits);
    }
}

void koyoda_speaker_events_beep_test(void)
{
    TaskHandle_t task = s_task;

    if (task != NULL)
    {
        xTaskNotify(
            task,
            SPEAKER_EVT_TEST_BEEP,
            eSetBits);
    }
}

int koyoda_speaker_events_get_volume(void)
{
    return clamp_volume(
        (int)s_volume_percent);
}

void koyoda_speaker_events_set_volume(int percent)
{
    const int clamped =
        clamp_volume(percent);

    s_volume_percent = clamped;

    TaskHandle_t task = s_task;

    if (task != NULL)
    {
        xTaskNotify(
            task,
            SPEAKER_EVT_VOLUME_CHANGE,
            eSetBits);
    }
}
