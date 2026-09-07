#include "koyoda_speaker_events.h"

#include <stdint.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "bsp/esp-bsp.h"

static const char *TAG = "KOYODA_SPK_EVT";

#define SAMPLE_RATE_HZ          22050
#define CHUNK_SAMPLES             128
#define VOLUME_PERCENT             25
#define TONE_HZ                   660
#define TONE_MS                   120
#define START_DELAY_MS           3000

static TaskHandle_t s_task = NULL;
static esp_codec_dev_handle_t s_speaker = NULL;
static volatile bool s_pending_charge_beep = false;

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

        fill_square_tone(samples, chunk, &phase);

        int ret = esp_codec_dev_write(
            s_speaker,
            samples,
            chunk * sizeof(int16_t));

        if (ret != ESP_CODEC_DEV_OK)
        {
            ESP_LOGE(TAG, "esp_codec_dev_write failed: %d", ret);
            return false;
        }

        sent += chunk;
        taskYIELD();
    }

    return true;
}

static void speaker_event_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "QUIET V4 START: no repeating beep timer exists");

    /* Let display/Wi-Fi settle before codec init. */
    vTaskDelay(pdMS_TO_TICKS(START_DELAY_MS));

    s_speaker = bsp_audio_codec_speaker_init();

    if (s_speaker == NULL)
    {
        ESP_LOGE(TAG, "bsp_audio_codec_speaker_init returned NULL");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    esp_codec_dev_sample_info_t format = {
        .sample_rate = SAMPLE_RATE_HZ,
        .channel = 1,
        .bits_per_sample = 16,
    };

    int ret = esp_codec_dev_open(s_speaker, &format);

    if (ret != ESP_CODEC_DEV_OK)
    {
        ESP_LOGE(TAG, "esp_codec_dev_open failed: %d", ret);
        s_speaker = NULL;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ret = esp_codec_dev_set_out_vol(
        s_speaker,
        VOLUME_PERCENT);

    if (ret != ESP_CODEC_DEV_OK)
    {
        ESP_LOGW(TAG, "set volume returned: %d", ret);
    }

    ESP_LOGI(TAG, "BOOT BEEP ONCE");
    play_one_beep();

    /*
     * IMPORTANT:
     * No periodic delay loop, no 5-second timer, no automatic replay.
     *
     * This task sleeps indefinitely until main.c explicitly notifies it
     * after a confirmed stable VBUS insertion.
     */
    while (1)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (s_pending_charge_beep)
        {
            s_pending_charge_beep = false;
            ESP_LOGI(TAG, "CHARGE INSERTION BEEP");
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
    s_pending_charge_beep = true;

    TaskHandle_t task = s_task;
    if (task != NULL)
    {
        xTaskNotifyGive(task);
    }
}
