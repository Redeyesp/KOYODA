#include "esp_log.h"

/* Old experimental motion component intentionally disabled.
 * Step 4 motion lives in main/koyoda_motion_inline.c.
 */
void koyoda_motion_probe_disabled(void)
{
    ESP_LOGD("KOYODA_MOTION", "legacy motion component disabled");
}
