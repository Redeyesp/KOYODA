#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t koyoda_speaker_probe_start(void);
bool koyoda_speaker_probe_is_finished(void);

/* Queue one short confirmation beep.
 * Used for events such as stable USB/VBUS insertion.
 */
void koyoda_speaker_request_beep(void);

#ifdef __cplusplus
}
#endif
