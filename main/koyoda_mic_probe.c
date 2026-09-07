#include "koyoda_mic_probe.h"

#include <stdint.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "bsp/esp-bsp.h"

/*
 * KOYODA Microphone Safe Probe v2
 *
 * Goal: prove that the onboard ES7210 can be initialized and continuously
 * captured WITHOUT disturbing the already-stable KOYODA UI/Wi-Fi/charging.
 *
 * This deliberately uses only the public BSP 3.0.1 API:
 *   bsp_audio_init(NULL) -> default 22050 Hz / 16-bit / mono
 *   bsp_audio_codec_microphone_init()
 *   esp_codec_dev_open/read()
 *
 * Once this baseline is stable we can move to the board's 24 kHz 4-slot TDM
 * voice profile for MIC1 + MIC2.  Do not mix that larger change into this test.
 */

static const char *TAG = "KOYODA_MIC";

#define KOYODA_MIC_SAMPLE_RATE_HZ      22050
#define KOYODA_MIC_SAMPLES_PER_READ    128
#define KOYODA_MIC_START_DELAY_MS      8000
#define KOYODA_MIC_REPORT_MS           500
#define KOYODA_MIC_GAIN_DB             24.0f

static volatile bool s_started = false;
static volatile bool s_running = false;

static int32_t abs_sample(int16_t sample)
{
    int32_t value = sample;
    return value < 0 ? -value : value;
}

static int level_to_percent(uint32_t level)
{
    if (level >= 12000U) return 100;
    return (int)((level * 100U) / 12000U);
}

static void fail_and_stop(const char *step, int code)
{
    ESP_LOGE(TAG, "%s failed: %d", step, code);
    s_running = false;
    s_started = false;
    vTaskDelete(NULL);
}

static void mic_probe_task(void *arg)
{
    (void)arg;

    /* Let Face/Battery/Wi-Fi/charging settle and visibly prove they are alive. */
    ESP_LOGI(TAG, "SAFE PROBE scheduled; audio init in %d ms", KOYODA_MIC_START_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(KOYODA_MIC_START_DELAY_MS));

    ESP_LOGI(TAG, "PHASE 1/4: initializing BSP I2S");
    esp_err_t err = bsp_audio_init(NULL);
    if (err != ESP_OK)
    {
        fail_and_stop("bsp_audio_init", err);
        return;
    }

    /* Yield deliberately between hardware-init phases. */
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "PHASE 2/4: creating ES7210 microphone codec");
    esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();
    if (mic == NULL)
    {
        ESP_LOGE(TAG, "bsp_audio_codec_microphone_init returned NULL");
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "PHASE 3/4: opening ES7210 capture stream");
    esp_codec_dev_sample_info_t format = {
        .sample_rate = KOYODA_MIC_SAMPLE_RATE_HZ,
        .channel = 1,
        .bits_per_sample = 16,
    };

    int codec_ret = esp_codec_dev_open(mic, &format);
    if (codec_ret != ESP_CODEC_DEV_OK)
    {
        fail_and_stop("esp_codec_dev_open", codec_ret);
        return;
    }

    codec_ret = esp_codec_dev_set_in_gain(mic, KOYODA_MIC_GAIN_DB);
    if (codec_ret != ESP_CODEC_DEV_OK)
    {
        ESP_LOGW(TAG, "Mic gain %.1f dB not accepted: %d", KOYODA_MIC_GAIN_DB, codec_ret);
    }

    ESP_LOGI(TAG, "PHASE 4/4: MIC READY 22050 Hz / 16-bit / mono");
    s_running = true;

    int16_t samples[KOYODA_MIC_SAMPLES_PER_READ];
    uint64_t sum = 0;
    uint32_t count = 0;
    uint32_t peak = 0;
    TickType_t last_report = xTaskGetTickCount();

    while (1)
    {
        codec_ret = esp_codec_dev_read(mic, samples, sizeof(samples));

        if (codec_ret != ESP_CODEC_DEV_OK)
        {
            ESP_LOGE(TAG, "Microphone read failed: %d", codec_ret);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        for (size_t i = 0; i < KOYODA_MIC_SAMPLES_PER_READ; ++i)
        {
            uint32_t level = (uint32_t)abs_sample(samples[i]);
            sum += level;
            count++;
            if (level > peak) peak = level;
        }

        /* Important: never let a fast audio-read loop monopolize a core. */
        vTaskDelay(pdMS_TO_TICKS(1));

        TickType_t now = xTaskGetTickCount();
        if ((now - last_report) >= pdMS_TO_TICKS(KOYODA_MIC_REPORT_MS))
        {
            uint32_t avg = count ? (uint32_t)(sum / count) : 0;
            ESP_LOGI(TAG,
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
    if (s_started) return ESP_OK;

    s_started = true;

    BaseType_t result = xTaskCreate(
        mic_probe_task,
        "mic_probe",
        6144,
        NULL,
        1,
        NULL);

    if (result != pdPASS)
    {
        s_started = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Mic safe probe task created; KOYODA core UI remains independent");
    return ESP_OK;
}

bool koyoda_mic_probe_is_running(void)
{
    return s_running;
}
