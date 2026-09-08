#include "koyoda_audio_stream.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"

#include "koyoda_audio_duplex.h"

static const char *TAG = "KOYODA_STREAM";

#ifndef CONFIG_KOYODA_STREAM_HOST
#define CONFIG_KOYODA_STREAM_HOST "192.168.1.100"
#endif

#ifndef CONFIG_KOYODA_STREAM_PORT
#define CONFIG_KOYODA_STREAM_PORT 7777
#endif

#define STREAM_QUEUE_DEPTH 12
#define STREAM_PCM_SAMPLES_PER_MSG 256

#define STREAM_TYPE_START 1
#define STREAM_TYPE_PCM   2
#define STREAM_TYPE_END   3

typedef struct
{
    uint8_t type;
    uint16_t sample_count;
    int16_t samples[STREAM_PCM_SAMPLES_PER_MSG];
} stream_msg_t;

static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;
static volatile bool s_prev_vad = false;
static volatile uint32_t s_dropped_frames = 0;

static void enqueue_marker(uint8_t type)
{
    if (s_queue == NULL) return;

    stream_msg_t msg = {
        .type = type,
        .sample_count = 0,
    };

    if (xQueueSend(s_queue, &msg, 0) != pdTRUE)
        s_dropped_frames++;
}

static void audio_frame_callback(
    const int16_t *samples,
    size_t sample_count,
    bool vad_speaking,
    void *user_ctx)
{
    (void)user_ctx;
    if (s_queue == NULL) return;

    bool prev = s_prev_vad;

    if (!prev && vad_speaking)
        enqueue_marker(STREAM_TYPE_START);

    if (vad_speaking)
    {
        stream_msg_t msg = {
            .type = STREAM_TYPE_PCM,
            .sample_count = 0,
        };

        size_t n = sample_count;
        if (n > STREAM_PCM_SAMPLES_PER_MSG)
            n = STREAM_PCM_SAMPLES_PER_MSG;

        memcpy(msg.samples, samples, n * sizeof(int16_t));
        msg.sample_count = (uint16_t)n;

        if (xQueueSend(s_queue, &msg, 0) != pdTRUE)
            s_dropped_frames++;
    }

    if (prev && !vad_speaking)
        enqueue_marker(STREAM_TYPE_END);

    s_prev_vad = vad_speaking;
}

static bool send_all(int sock, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;

    while (sent < len)
    {
        int ret = send(sock, p + sent, len - sent, 0);
        if (ret <= 0) return false;
        sent += (size_t)ret;
    }
    return true;
}

static bool send_packet(
    int sock,
    uint8_t type,
    const void *payload,
    uint32_t payload_len)
{
    uint8_t header[8] = {
        'K','O','Y','A',
        type,
        (uint8_t)((payload_len >> 16) & 0xFF),
        (uint8_t)((payload_len >> 8) & 0xFF),
        (uint8_t)(payload_len & 0xFF)
    };

    if (!send_all(sock, header, sizeof(header))) return false;
    if (payload_len > 0)
        return send_all(sock, payload, payload_len);
    return true;
}

static int connect_receiver(void)
{
    char port_text[8];
    snprintf(port_text, sizeof(port_text), "%d", CONFIG_KOYODA_STREAM_PORT);

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *result = NULL;

    int gai = getaddrinfo(
        CONFIG_KOYODA_STREAM_HOST,
        port_text,
        &hints,
        &result);

    if (gai != 0 || result == NULL)
        return -1;

    int sock = socket(
        result->ai_family,
        result->ai_socktype,
        result->ai_protocol);

    if (sock < 0)
    {
        freeaddrinfo(result);
        return -1;
    }

    int ret = connect(sock, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);

    if (ret != 0)
    {
        close(sock);
        return -1;
    }

    return sock;
}

static void stream_task(void *arg)
{
    (void)arg;

    ESP_LOGI(
        TAG,
        "STREAM STEP1 ready -> %s:%d",
        CONFIG_KOYODA_STREAM_HOST,
        CONFIG_KOYODA_STREAM_PORT);

    int sock = -1;
    uint64_t utterance_bytes = 0;
    TickType_t utterance_start = 0;

    while (1)
    {
        stream_msg_t msg;

        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE)
            continue;

        if (sock < 0)
        {
            sock = connect_receiver();

            if (sock < 0)
            {
                if (msg.type == STREAM_TYPE_START)
                {
                    ESP_LOGW(
                        TAG,
                        "Receiver unavailable at %s:%d",
                        CONFIG_KOYODA_STREAM_HOST,
                        CONFIG_KOYODA_STREAM_PORT);
                }
                continue;
            }

            ESP_LOGI(TAG, "TCP connected to receiver");
        }

        bool ok = true;

        if (msg.type == STREAM_TYPE_START)
        {
            utterance_bytes = 0;
            utterance_start = xTaskGetTickCount();
            ok = send_packet(sock, STREAM_TYPE_START, NULL, 0);
            ESP_LOGI(TAG, "VOICE STREAM START");
        }
        else if (msg.type == STREAM_TYPE_PCM)
        {
            uint32_t bytes =
                (uint32_t)msg.sample_count * sizeof(int16_t);

            ok = send_packet(
                sock,
                STREAM_TYPE_PCM,
                msg.samples,
                bytes);

            if (ok)
                utterance_bytes += bytes;
        }
        else if (msg.type == STREAM_TYPE_END)
        {
            ok = send_packet(sock, STREAM_TYPE_END, NULL, 0);

            uint32_t duration_ms =
                (uint32_t)(
                    (xTaskGetTickCount() - utterance_start) *
                    portTICK_PERIOD_MS);

            ESP_LOGI(
                TAG,
                "VOICE STREAM END duration=%lums bytes=%llu dropped=%lu",
                (unsigned long)duration_ms,
                (unsigned long long)utterance_bytes,
                (unsigned long)s_dropped_frames);
        }

        if (!ok)
        {
            ESP_LOGW(
                TAG,
                "Socket send failed errno=%d; closing connection",
                errno);
            close(sock);
            sock = -1;
        }
    }
}

esp_err_t koyoda_audio_stream_start(void)
{
    if (s_task != NULL)
        return ESP_OK;

    s_queue = xQueueCreate(
        STREAM_QUEUE_DEPTH,
        sizeof(stream_msg_t));

    if (s_queue == NULL)
        return ESP_ERR_NO_MEM;

    BaseType_t result = xTaskCreate(
        stream_task,
        "audio_stream",
        4096,
        NULL,
        2,
        &s_task);

    if (result != pdPASS)
    {
        vQueueDelete(s_queue);
        s_queue = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    koyoda_audio_duplex_set_frame_callback(
        audio_frame_callback,
        NULL);

    return ESP_OK;
}
