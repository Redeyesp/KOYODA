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
 * KOYODA Speaker Solo v3
 *
 * Mic remains intentionally OFF for this diagnostic baseline.
 *
 * Sound policy:
 *   - ONE short beep when KOYODA boots and speaker becomes ready
 *   - ONE short beep when USB/VBUS is newly inserted
 *   - NO repeating beep
 */

#define KOYODA_SPK_SAMPLE_RATE_HZ      22050
#define KOYODA_SPK_CHUNK_SAMPLES         128
#define KOYODA_SPK_VOLUME_PERCENT          25
#define KOYODA_SPK_TONE_HZ                660
#define KOYODA_SPK_TONE_MS                120
#define KOYODA_SPK_START_DELAY_MS         3000

static volatile bool s_started = false;
static volatile bool s_finished = false;
static volatile bool s_beep_pending = false;

static esp_codec_dev_handle_t s_speaker = NULL;

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

    const int16_t amplitude = 3500;

    for (size_t i = 0; i < count; ++i)
    {
        uint32_t p = (*phase) % period;
        samples[i] = (p < (period / 2U))
                         ? amplitude
                         : (int16_t)-amplitude;
        (*phase)++;
    }
}

static bool play_beep(void)
{
    if (s_speaker == NULL)
    {
        return false;
    }

    int16_t samples[KOYODA_SPK_CHUNK_SAMPLES];
    uint32_t phase = 0;

    const uint32_t total_samples =
        (KOYODA_SPK_SAMPLE_RATE_HZ * KOYODA_SPK_TONE_MS) / 1000U;

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

static void speaker_probe_task(void *arg)
{
    (void)arg;

    ESP_LOGI(
        TAG,
        "Speaker v3: one boot beep + charge-insertion beep only");

    vTaskDelay(pdMS_TO_TICKS(KOYODA_SPK_START_DELAY_MS));

    log_memory("before speaker init");

    s_speaker = bsp_audio_codec_speaker_init();

    if (s_speaker == NULL)
    {
        ESP_LOGE(TAG, "bsp_audio_codec_speaker_init returned NULL");
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    esp_codec_dev_sample_info_t format = {
        .sample_rate = KOYODA_SPK_SAMPLE_RATE_HZ,
        .channel = 1,
        .bits_per_sample = 16,
    };

    int ret = esp_codec_dev_open(s_speaker, &format);

    if (ret != ESP_CODEC_DEV_OK)
    {
        ESP_LOGE(TAG, "esp_codec_dev_open failed: %d", ret);
        s_speaker = NULL;
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    ret = esp_codec_dev_set_out_vol(
        s_speaker,
        KOYODA_SPK_VOLUME_PERCENT);

    if (ret != ESP_CODEC_DEV_OK)
    {
        ESP_LOGW(TAG, "set volume returned: %d", ret);
    }

    ESP_LOGI(
        TAG,
        "SPEAKER READY: volume=%d%%",
        KOYODA_SPK_VOLUME_PERCENT);

    /* One short boot confirmation beep. */
    ESP_LOGI(TAG, "BOOT BEEP");
    play_beep();
    s_finished = true;

    /*
     * Stay alive quietly and service event beeps only.
     * There is intentionally no repeating sound.
     */
    while (1)
    {
        if (s_beep_pending)
        {
            s_beep_pending = false;
            ESP_LOGI(TAG, "EVENT BEEP");
            play_beep();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
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

    ESP_LOGI(TAG, "Speaker v3 scheduled");
    return ESP_OK;
}

void koyoda_speaker_request_beep(void)
{
    s_beep_pending = true;
}

bool koyoda_speaker_probe_is_finished(void)
{
    return s_finished;
}
