#include "koyoda_speaker_probe.h"

#include <stdint.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "bsp/esp-bsp.h"

static const char *TAG = "KOYODA_SPK";

/*
 * SPEAKER SOLO DIAGNOSTIC
 *
 * IMPORTANT:
 *   The microphone is intentionally NOT running in this build.
 *
 * Goal:
 *   Test only:
 *       Display + Wi-Fi + ES8311 speaker
 *
 * If this is stable, the previous freeze is specifically caused by
 * running/initializing Mic + Speaker together, not by the speaker hardware
 * itself.
 */

#define KOYODA_SPK_SAMPLE_RATE_HZ    22050
#define KOYODA_SPK_CHUNK_SAMPLES       128
#define KOYODA_SPK_VOLUME_PERCENT        18
#define KOYODA_SPK_TONE_HZ              660
#define KOYODA_SPK_TONE_MS              120
#define KOYODA_SPK_GAP_MS               250
#define KOYODA_SPK_BEEP_COUNT             3
#define KOYODA_SPK_START_DELAY_MS       5000

static volatile bool s_started = false;
static volatile bool s_finished = false;

static void log_memory(const char *where)
{
    ESP_LOGI(
        TAG,
        "%s: DMA free=%u largest=%u | internal free=%u largest=%u",
        where,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static void fill_square_tone(
    int16_t *samples,
    size_t count,
    uint32_t *phase)
{
    uint32_t period =
        KOYODA_SPK_SAMPLE_RATE_HZ / KOYODA_SPK_TONE_HZ;

    if (period < 2)
    {
        period = 2;
    }

    /* Quieter than the first probe on purpose. */
    const int16_t amplitude = 3000;

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

static bool play_tone(
    esp_codec_dev_handle_t speaker,
    uint32_t duration_ms)
{
    int16_t samples[KOYODA_SPK_CHUNK_SAMPLES];
    uint32_t phase = 0;

    const uint32_t total_samples =
        (KOYODA_SPK_SAMPLE_RATE_HZ * duration_ms) / 1000U;

    uint32_t sent = 0;

    while (sent < total_samples)
    {
        size_t chunk = KOYODA_SPK_CHUNK_SAMPLES;
        uint32_t remain = total_samples - sent;

        if (remain < chunk)
        {
            chunk = remain;
        }

        fill_square_tone(samples, chunk, &phase);

        int ret = esp_codec_dev_write(
            speaker,
            samples,
            chunk * sizeof(int16_t));

        if (ret != ESP_CODEC_DEV_OK)
        {
            ESP_LOGE(TAG, "esp_codec_dev_write failed: %d", ret);
            return false;
        }

        sent += chunk;

        /*
         * Yield between tiny writes so the display task is never starved.
         */
        taskYIELD();
    }

    return true;
}

static void speaker_probe_task(void *arg)
{
    (void)arg;

    ESP_LOGW(
        TAG,
        "SOLO TEST: mic is OFF; waiting %u ms before speaker init",
        (unsigned)KOYODA_SPK_START_DELAY_MS);

    /*
     * Let LVGL, Wi-Fi, battery and animation settle first.
     */
    vTaskDelay(pdMS_TO_TICKS(KOYODA_SPK_START_DELAY_MS));

    log_memory("before speaker init");

    ESP_LOGI(TAG, "Calling bsp_audio_codec_speaker_init()");

    esp_codec_dev_handle_t speaker =
        bsp_audio_codec_speaker_init();

    if (speaker == NULL)
    {
        ESP_LOGE(TAG, "bsp_audio_codec_speaker_init returned NULL");
        log_memory("speaker init failed");
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    log_memory("after speaker init");

    esp_codec_dev_sample_info_t format = {
        .sample_rate = KOYODA_SPK_SAMPLE_RATE_HZ,
        .channel = 1,
        .bits_per_sample = 16,
    };

    ESP_LOGI(TAG, "Opening ES8311 codec");

    int ret = esp_codec_dev_open(speaker, &format);

    if (ret != ESP_CODEC_DEV_OK)
    {
        ESP_LOGE(TAG, "esp_codec_dev_open failed: %d", ret);
        log_memory("speaker open failed");
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    log_memory("after speaker open");

    ret = esp_codec_dev_set_out_vol(
        speaker,
        KOYODA_SPK_VOLUME_PERCENT);

    if (ret != ESP_CODEC_DEV_OK)
    {
        ESP_LOGW(TAG, "set volume returned: %d", ret);
    }

    ESP_LOGI(
        TAG,
        "SPEAKER READY: 22050 Hz / 16-bit / mono / volume=%d%%",
        KOYODA_SPK_VOLUME_PERCENT);

    for (int i = 0; i < KOYODA_SPK_BEEP_COUNT; ++i)
    {
        ESP_LOGI(
            TAG,
            "BEEP %d/%d",
            i + 1,
            KOYODA_SPK_BEEP_COUNT);

        if (!play_tone(speaker, KOYODA_SPK_TONE_MS))
        {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(KOYODA_SPK_GAP_MS));
    }

    ret = esp_codec_dev_close(speaker);

    if (ret != ESP_CODEC_DEV_OK)
    {
        ESP_LOGW(TAG, "esp_codec_dev_close returned: %d", ret);
    }

    log_memory("after speaker close");

    ESP_LOGI(TAG, "SPEAKER SOLO TEST COMPLETE");
    s_finished = true;

    vTaskDelete(NULL);
}

esp_err_t koyoda_speaker_probe_start(void)
{
    if (s_started)
    {
        return ESP_OK;
    }

    s_started = true;

    BaseType_t result = xTaskCreate(
        speaker_probe_task,
        "speaker_probe",
        4096,
        NULL,
        2,
        NULL);

    if (result != pdPASS)
    {
        s_started = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Speaker solo probe scheduled");
    return ESP_OK;
}

bool koyoda_speaker_probe_is_finished(void)
{
    return s_finished;
}
