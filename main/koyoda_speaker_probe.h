#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One-shot ES8311 speaker coexistence test.
 *
 * Waits for the current ES7210 mic probe, then plays three short beeps.
 * No LVGL/UI calls are made by this module.
 */
esp_err_t koyoda_speaker_probe_start(void);
bool koyoda_speaker_probe_is_finished(void);

#ifdef __cplusplus
}
#endif
