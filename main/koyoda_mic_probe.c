#include "koyoda_mic_probe.h"

#include <stdint.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "bsp/esp-bsp.h"

/*
 * KOYODA Microphone Probe Step 1
 *
 * IMPORTANT:
 * KOYODA is pinned to Waveshare BSP 3.0.1.
 * The public BSP header used by the build exposes
 * bsp_audio_codec_microphone_init(), but not bsp_audio_init_voice_24k().
 *
 * Therefore this probe deliberately uses the BSP's public/default
 * microphone path first:
 *   ES7210 -> standard I2S RX -> 22050 Hz / 16-bit / mono
 *
 * The goal of Step 1 is only to prove that real microphone samples change
 * with sound without disturbing KOYODA's stable UI/animation/Wi-Fi.
 */

static const char *TAG = "KOYODA_MIC";

#define KOYODA_MIC_SAMPLE_RATE_HZ      22050
#define KOYODA_MIC_SAMPLES_PER_READ    256
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
     * Diagnostic meter only.  This is NOT calibrated SPL.
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
     * Let the already-stable display, animation, battery and Wi-Fi
     * systems settle before audio starts.
     */
    vTaskDelay(pdMS_TO_TICKS(KOYODA_MIC_START_DELAY_MS));

    ESP_LOGI(TAG, "Initializing ES7210 microphone probe");

    /*
     * The BSP initializes its default audio/I2S path internally when
     * the microphone codec is requested and audio has not been started yet.
     */
    esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();

    if (mic == NULL)
    {
        ESP_LOGE(
            TAG,
            "bsp_audio_codec_microphone_init returned NULL");
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    esp_codec_dev_sample_info_t format = {
        .sample_rate = KOYODA_MIC_SAMPLE_RATE_HZ,
        .channel = 1,
        .bits_per_sample = 16,
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

    codec_ret = esp_codec_dev_set_in_gain(
        mic,
        KOYODA_MIC_GAIN_DB);

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
        "MIC READY: ES7210 22050 Hz / 16-bit / mono");

    s_running = true;

    int16_t samples[KOYODA_MIC_SAMPLES_PER_READ];

    uint64_t sum = 0;
    uint32_t count = 0;
    uint32_t peak = 0;

    TickType_t last_report = xTaskGetTickCount();

    while (1)
    {
        codec_ret = esp_codec_dev_read(
            mic,
            samples,
            sizeof(samples));

        if (codec_ret != ESP_CODEC_DEV_OK)
        {
            ESP_LOGE(
                TAG,
                "Microphone read failed: %d",
                codec_ret);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        for (size_t i = 0;
             i < KOYODA_MIC_SAMPLES_PER_READ;
             ++i)
        {
            uint32_t level =
                (uint32_t)abs_sample(samples[i]);

            sum += level;
            count++;

            if (level > peak)
            {
                peak = level;
            }
        }

        TickType_t now = xTaskGetTickCount();

        if ((now - last_report) >=
            pdMS_TO_TICKS(KOYODA_MIC_REPORT_MS))
        {
            uint32_t avg =
                (count > 0)
                    ? (uint32_t)(sum / count)
                    : 0;

            ESP_LOGI(
                TAG,
                "MIC %3d%% avg=%5lu peak=%5lu",
                level_to_percent(avg),
                (unsigned long)avg,
                (unsigned long)peak);

            sum = 0;
            count = 0;
            peak = 0;
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
