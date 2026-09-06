#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KOYODA_WIFI_DISABLED, KOYODA_WIFI_STARTING, KOYODA_WIFI_CONNECTING,
    KOYODA_WIFI_CONNECTED, KOYODA_WIFI_RETRYING, KOYODA_WIFI_ERROR
} koyoda_wifi_state_t;
typedef struct {
    koyoda_wifi_state_t state;
    char ip[16];
    int rssi; /* dBm; -127 when unavailable */
    uint8_t disconnect_reason; /* ESP-IDF reason code */
    esp_err_t last_error;
} koyoda_wifi_status_t;

/* Non-blocking and idempotent. Call after UI creation, outside its lock.
 * Initialization errors leave the pet alive; reboot to retry.
 * This module owns the STA interface; do not initialize another STA. */
esp_err_t koyoda_wifi_start(void);
/* Getters only copy cached values: no network calls or LVGL access. */
void koyoda_wifi_get_status(koyoda_wifi_status_t *out);
bool koyoda_wifi_is_connected(void);
int koyoda_wifi_get_rssi(void);

#ifdef __cplusplus
}
#endif
