#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start the speaker event service.
 *
 * Behavior:
 *   - one short boot beep after ES8311 is ready
 *   - then blocks silently forever
 *   - another beep happens ONLY when beep_charge() is explicitly called
 */
esp_err_t koyoda_speaker_events_start(void);

/* Request one short beep for a confirmed USB/VBUS insertion event. */
void koyoda_speaker_events_beep_charge(void);

#ifdef __cplusplus
}
#endif
