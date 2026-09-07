#include "koyoda_mic_probe.h"

#include <stdint.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "bsp/esp-bsp.h"

/*
 * Waveshare ESP32-S3-Touch-AMOLED-1.75 audio wiring:
 *
 * ES7210 RX uses four TDM slots:
 *   slot 0 = MIC1 (front microphone)
 *   slot 1 = MIC3 (playback reference)
 *   slot 2 = MIC2 (front microphone)
 *   slot 3 = MIC4 (not connected)
 *
 * For this first probe we inspect only slot 0 and slot 2.
 */

static const char *TAG = "KOYODA_MIC";

#define KOYODA_MIC_SAMPLE_RATE_HZ      24000
#define KOYODA_MIC_TDM_CHANNELS        4
#define KOYODA_MIC_FRAMES_PER_READ     240
#define KOYODA_MIC_START_DELAY_MS      3000
#define KOYODA_MIC_REPORT_MS           300
#define KOYODA_MIC_GAIN_DB             30.0f

static volatile bool s_started = false;
static volatile bool s_running = false;

static int32_t abs_sample(int16_t sample)
{
    int32_t value = sample;
    return (value < 0) ? -value : value;
}

static int level_to_percent(uint32_t level)
{
    /*
     * Diagnostic meter only; this is deliberately not calibrated SPL.
     * 12000 average absolute sample value maps to 100%.
     */
    if (level >= 12000U)
    {
        return 100;
    }

    return (int)((level * 100U) / 12000U);
}

static void mic_probe_task(void *arg)
{
    (void)arg;

    /*
     * Give display, animations, battery task and Wi-Fi time to reach
     * their stable baseline before the audio peripheral is initialized.
     */
    vTaskDelay(pdMS_TO_TICKS(KOYODA_MIC_START_DELAY_MS));

    ESP_LOGI(TAG, "Initializing ES7210 microphone probe");

    /*
     * The maintained Waveshare BSP uses:
     * - 24 kHz
     * - 16-bit
     * - TX standard I2S
     * - RX four-slot TDM
     * - MCLK x256
     */
    esp_err_t err = bsp_audio_init_voice_24k();

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "bsp_audio_init_voice_24k failed: %s",
            esp_err_to_name(err));
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();

    if (mic == NULL)
    {
        ESP_LOGE(TAG, "bsp_audio_codec_microphone_init returned NULL");
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    esp_codec_dev_sample_info_t format = {
        .sample_rate = KOYODA_MIC_SAMPLE_RATE_HZ,
        .channel = KOYODA_MIC_TDM_CHANNELS,
        .bits_per_sample = 16,
        .channel_mask = 0x0F,
        .mclk_multiple = 256,
    };

    int codec_ret = esp_codec_dev_open(mic, &format);

    if (codec_ret != ESP_CODEC_DEV_OK)
    {
        ESP_LOGE(
            TAG,
            "esp_codec_dev_open failed: %d",
            codec_ret);
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    codec_ret = esp_codec_dev_set_in_gain(mic, KOYODA_MIC_GAIN_DB);

    if (codec_ret != ESP_CODEC_DEV_OK)
    {
        ESP_LOGW(
            TAG,
            "Could not set mic gain to %.1f dB: %d",
            KOYODA_MIC_GAIN_DB,
            codec_ret);
    }

    ESP_LOGI(
        TAG,
        "MIC READY: ES7210 24kHz 16-bit 4-slot TDM; monitoring MIC1 + MIC2");

    s_running = true;

    int16_t samples[
        KOYODA_MIC_FRAMES_PER_READ *
        KOYODA_MIC_TDM_CHANNELS];

    uint64_t sum_mic1 = 0;
    uint64_t sum_mic2 = 0;
    uint32_t count = 0;
    uint32_t peak_mic1 = 0;
    uint32_t peak_mic2 = 0;

    TickType_t last_report = xTaskGetTickCount();

    while (1)
    {
        codec_ret = esp_codec_dev_read(
            mic,
            samples,
            sizeof(samples));

        if (codec_ret != ESP_CODEC_DEV_OK)
        {
            ESP_LOGE(TAG, "Microphone read failed: %d", codec_ret);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        for (size_t frame = 0;
             frame < KOYODA_MIC_FRAMES_PER_READ;
             ++frame)
        {
            /*
             * TDM order documented by the Waveshare BSP:
             * MIC1, playback-ref, MIC2, unused.
             */
            uint32_t mic1 = (uint32_t)abs_sample(
                samples[(frame * KOYODA_MIC_TDM_CHANNELS) + 0]);

            uint32_t mic2 = (uint32_t)abs_sample(
                samples[(frame * KOYODA_MIC_TDM_CHANNELS) + 2]);

            sum_mic1 += mic1;
            sum_mic2 += mic2;
            count++;

            if (mic1 > peak_mic1)
            {
                peak_mic1 = mic1;
            }

            if (mic2 > peak_mic2)
            {
                peak_mic2 = mic2;
            }
        }

        TickType_t now = xTaskGetTickCount();

        if ((now - last_report) >= pdMS_TO_TICKS(KOYODA_MIC_REPORT_MS))
        {
            uint32_t avg_mic1 =
                (count > 0) ? (uint32_t)(sum_mic1 / count) : 0;

            uint32_t avg_mic2 =
                (count > 0) ? (uint32_t)(sum_mic2 / count) : 0;

            ESP_LOGI(
                TAG,
                "MIC1 %3d%% avg=%5lu peak=%5lu | MIC2 %3d%% avg=%5lu peak=%5lu",
                level_to_percent(avg_mic1),
                (unsigned long)avg_mic1,
                (unsigned long)peak_mic1,
                level_to_percent(avg_mic2),
                (unsigned long)avg_mic2,
                (unsigned long)peak_mic2);

            sum_mic1 = 0;
            sum_mic2 = 0;
            count = 0;
            peak_mic1 = 0;
            peak_mic2 = 0;
            last_report = now;
        }
    }
}

esp_err_t koyoda_mic_probe_start(void)
{
    if (s_started)
    {
        return ESP_OK;
    }

    s_started = true;

    BaseType_t task_result = xTaskCreate(
        mic_probe_task,
        "mic_probe",
        6144,
        NULL,
        1,
        NULL);

    if (task_result != pdPASS)
    {
        s_started = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Mic probe scheduled; UI/animation untouched");

    return ESP_OK;
}

bool koyoda_mic_probe_is_running(void)
{
    return s_running;
}
