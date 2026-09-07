#include "koyoda_speaker_probe.h"

#include <stdint.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "bsp/esp-bsp.h"

#include "koyoda_mic_probe.h"

/*
 * KOYODA Speaker Probe Step 1
 *
 * Purpose:
 *   Prove that ES8311 playback can coexist with:
 *   - LVGL/display animation
 *   - Wi-Fi
 *   - ES7210 microphone capture
 *
 * This module does not touch LVGL or page state.
 *
 * The current microphone probe uses the Waveshare BSP default audio profile:
 *   22050 Hz / 16-bit / mono.
 *
 * The speaker MUST use the same format because ES7210 RX and ES8311 TX share
 * the BSP's duplex I2S clock domain.
 */

static const char *TAG = "KOYODA_SPK";

#define KOYODA_SPK_SAMPLE_RATE_HZ       22050
#define KOYODA_SPK_SAMPLES_PER_CHUNK    256
#define KOYODA_SPK_VOLUME_PERCENT       25
#define KOYODA_SPK_TONE_HZ              880
#define KOYODA_SPK_TONE_MS              180
#define KOYODA_SPK_GAP_MS               220
#define KOYODA_SPK_BEEP_COUNT            3
#define KOYODA_SPK_WAIT_MIC_MS         10000

static volatile bool s_started = false;
static volatile bool s_finished = false;

static void log_dma_heap(const char *where)
{
    size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t largest_dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);

    ESP_LOGI(
        TAG,
        "%s: DMA free=%u largest=%u",
        where,
        (unsigned)free_dma,
        (unsigned)largest_dma);
}

static void fill_square_tone(
    int16_t *samples,
    size_t count,
    uint32_t *phase)
{
    /*
     * Lightweight square-wave generator.
     * No float/libm and no large waveform asset.
     */
    const uint32_t period =
        KOYODA_SPK_SAMPLE_RATE_HZ / KOYODA_SPK_TONE_HZ;

    const int16_t amplitude = 5000;

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

static bool write_tone_ms(
    esp_codec_dev_handle_t speaker,
    uint32_t duration_ms)
{
    int16_t samples[KOYODA_SPK_SAMPLES_PER_CHUNK];
    uint32_t phase = 0;

    uint32_t total_samples =
        (KOYODA_SPK_SAMPLE_RATE_HZ * duration_ms) / 1000U;

    uint32_t written_samples = 0;

    while (written_samples < total_samples)
    {
        size_t chunk = KOYODA_SPK_SAMPLES_PER_CHUNK;

        uint32_t remaining =
            total_samples - written_samples;

        if (remaining < chunk)
        {
            chunk = remaining;
        }

        fill_square_tone(samples, chunk, &phase);

        int codec_ret = esp_codec_dev_write(
            speaker,
            samples,
            chunk * sizeof(int16_t));

        if (codec_ret != ESP_CODEC_DEV_OK)
        {
            ESP_LOGE(
                TAG,
                "esp_codec_dev_write failed: %d",
                codec_ret);
            return false;
        }

        written_samples += chunk;
    }

    return true;
}

static void speaker_probe_task(void *arg)
{
    (void)arg;

    ESP_LOGI(
        TAG,
        "Speaker probe waiting for microphone to become ready");

    uint32_t waited_ms = 0;

    while (!koyoda_mic_probe_is_running() &&
           waited_ms < KOYODA_SPK_WAIT_MIC_MS)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited_ms += 100;
    }

    if (koyoda_mic_probe_is_running())
    {
        ESP_LOGI(
            TAG,
            "Microphone is running; testing full-duplex audio");
    }
    else
    {
        ESP_LOGW(
            TAG,
            "Microphone not ready after %u ms; testing speaker anyway",
            (unsigned)KOYODA_SPK_WAIT_MIC_MS);
    }

    /*
     * Give UI/Wi-Fi/mic a moment to settle so the beep is a clean
     * coexistence test rather than another boot-time initialization burst.
     */
    vTaskDelay(pdMS_TO_TICKS(1000));

    log_dma_heap("before speaker init");

    /*
     * If the mic already initialized BSP audio, this reuses the same I2S
     * data interface and adds the ES8311 playback codec/PA path.
     * The BSP owns GPIO46 PA control.
     */
    esp_codec_dev_handle_t speaker =
        bsp_audio_codec_speaker_init();

    if (speaker == NULL)
    {
        ESP_LOGE(
            TAG,
            "bsp_audio_codec_speaker_init returned NULL");
        log_dma_heap("speaker init failed");
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    esp_codec_dev_sample_info_t format = {
        .sample_rate = KOYODA_SPK_SAMPLE_RATE_HZ,
        .channel = 1,
        .bits_per_sample = 16,
    };

    int codec_ret = esp_codec_dev_open(
        speaker,
        &format);

    if (codec_ret != ESP_CODEC_DEV_OK)
    {
        ESP_LOGE(
            TAG,
            "esp_codec_dev_open failed: %d",
            codec_ret);
        log_dma_heap("speaker open failed");
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    codec_ret = esp_codec_dev_set_out_vol(
        speaker,
        KOYODA_SPK_VOLUME_PERCENT);

    if (codec_ret != ESP_CODEC_DEV_OK)
    {
        ESP_LOGW(
            TAG,
            "Could not set speaker volume: %d",
            codec_ret);
    }

    log_dma_heap("speaker ready");

    ESP_LOGI(
        TAG,
        "SPEAKER READY: ES8311 22050 Hz / 16-bit / mono / volume=%d%%",
        KOYODA_SPK_VOLUME_PERCENT);

    for (int beep = 0;
         beep < KOYODA_SPK_BEEP_COUNT;
         ++beep)
    {
        ESP_LOGI(
            TAG,
            "BEEP %d/%d",
            beep + 1,
            KOYODA_SPK_BEEP_COUNT);

        if (!write_tone_ms(
                speaker,
                KOYODA_SPK_TONE_MS))
        {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(
            KOYODA_SPK_GAP_MS));
    }

    codec_ret = esp_codec_dev_close(speaker);

    if (codec_ret != ESP_CODEC_DEV_OK)
    {
        ESP_LOGW(
            TAG,
            "esp_codec_dev_close returned: %d",
            codec_ret);
    }

    log_dma_heap("after speaker test");

    ESP_LOGI(
        TAG,
        "SPEAKER TEST COMPLETE");

    s_finished = true;

    /*
     * Probe is intentionally one-shot.
     * Mic, Wi-Fi and KOYODA UI continue running normally.
     */
    vTaskDelete(NULL);
}

esp_err_t koyoda_speaker_probe_start(void)
{
    if (s_started)
    {
        return ESP_OK;
    }

    s_started = true;

    BaseType_t task_result = xTaskCreate(
        speaker_probe_task,
        "speaker_probe",
        4096,
        NULL,
        2,
        NULL);

    if (task_result != pdPASS)
    {
        s_started = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Speaker probe scheduled");

    return ESP_OK;
}

bool koyoda_speaker_probe_is_finished(void)
{
    return s_finished;
}
