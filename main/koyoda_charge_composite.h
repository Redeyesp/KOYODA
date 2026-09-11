#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Charge LITE v2
 *
 * Uses one full-screen RGB565 work frame in external PSRAM. The frame is
 * initialized from koyoda_idle and only the compact charge mouth/electricity
 * rectangle is changed. main.c renders it through the existing face_img.
 * There is NO separate charging LVGL overlay object.
 */
bool koyoda_charge_composite_init(void);
bool koyoda_charge_composite_apply(unsigned step);
const lv_image_dsc_t *koyoda_charge_composite_image(unsigned step);

#ifdef __cplusplus
}
#endif
