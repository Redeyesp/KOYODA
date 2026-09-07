#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Step 1 is intentionally diagnostic only.
 *
 * It starts the onboard ES7210 microphones in the Waveshare 24 kHz
 * voice/TDM profile and prints audio levels to Serial.
 *
 * No LVGL/UI functions are called by this module.
 */
esp_err_t koyoda_mic_probe_start(void);
bool koyoda_mic_probe_is_running(void);

#ifdef __cplusplus
}
#endif
