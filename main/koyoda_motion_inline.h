#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Step 4 architecture:
 * - no constructor
 * - no custom FreeRTOS task
 * - no LVGL timer
 * - no display lock in this module
 *
 * app_main calls koyoda_motion_poll() in its existing loop.
 */
esp_err_t koyoda_motion_init(void);
void koyoda_motion_poll(void);

bool koyoda_motion_is_active(void);
const lv_image_dsc_t *koyoda_motion_current_image(void);

#ifdef __cplusplus
}
#endif
