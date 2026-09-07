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
 * KOYODA Speaker Solo Diagnostic v2
 *
 * Mic is intentionally OFF in this build.
 *
 * This version repeats the speaker test forever so Serial Monitor can be
 * opened at any time without missing the startup messages.
 */

#define KOYODA_SPK_SAMPLE_RATE_HZ     22050
#define KOYODA_SPK_CHUNK_SAMPLES        128
#define KOYODA_SPK_VOLUME_PERCENT         35
#define KOYODA_SPK_TONE_HZ               660
#define KOYODA_SPK_TONE_MS               180
#define KOYODA_SPK_GAP_MS                220
#define KOYODA_SPK_BEEP_COUNT              3
#define KOYODA_SPK_START_DELAY_MS        5000
#define KOYODA_SPK_REPEAT_DELAY_MS       5000

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
    uint32_t period = KOYODA_SPK_SAMPLE_RATE_HZ / KOYODA_SPK_TONE_HZ;
    if (period < 2)
    {
        period = 2;
    }

    const int16_t amplitude = 5000;

    for (size_t i = 0; i < count; ++i)
    {
        uint32_t p = (*phase) % period;
        samples[i] = (p < (period / 2U))
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
        taskYIELD();
    }

    return true;
}

static void speaker_probe_task(void *arg)
{
    (void)arg;

    ESP_LOGW(
        TAG,
        "SOLO v2: mic is OFF; waiting %u ms before speaker init",
        (unsigned)KOYODA_SPK_START_DELAY_MS);

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

    unsigned cycle = 1;

    while (1)
    {
        ESP_LOGI(TAG, "=== SPEAKER TEST CYCLE %u ===", cycle);

        bool ok = true;

        for (int i = 0; i < KOYODA_SPK_BEEP_COUNT; ++i)
        {
            ESP_LOGI(
                TAG,
                "BEEP %d/%d",
                i + 1,
                KOYODA_SPK_BEEP_COUNT);

            if (!play_tone(speaker, KOYODA_SPK_TONE_MS))
            {
                ok = false;
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(KOYODA_SPK_GAP_MS));
        }

        log_memory("after beep cycle");

        if (ok)
        {
            ESP_LOGI(
                TAG,
                "Cycle %u complete; repeating in %u ms",
                cycle,
                (unsigned)KOYODA_SPK_REPEAT_DELAY_MS);
        }
        else
        {
            ESP_LOGE(
                TAG,
                "Cycle %u failed; repeating in %u ms",
                cycle,
                (unsigned)KOYODA_SPK_REPEAT_DELAY_MS);
        }

        s_finished = true;
        cycle++;

        vTaskDelay(pdMS_TO_TICKS(KOYODA_SPK_REPEAT_DELAY_MS));
    }
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

    ESP_LOGI(TAG, "Speaker solo v2 probe scheduled");
    return ESP_OK;
}

bool koyoda_speaker_probe_is_finished(void)
{
    return s_finished;
}
