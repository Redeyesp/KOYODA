#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Call ONCE after KOYODA has created face_img and released the BSP display lock.
 *
 * This implementation creates only an LVGL timer.
 * It does NOT create a FreeRTOS task and does NOT use a constructor.
 */
esp_err_t koyoda_motion_ui_init(lv_obj_t *face_img);

/* True only while a gyro reaction currently owns face_img. */
bool koyoda_motion_ui_is_active(void);

#ifdef __cplusplus
}
#endif
