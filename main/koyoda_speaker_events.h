#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Event-driven ES8311 speaker service.
 *
 * Sound policy:
 *   - one short beep after boot
 *   - one short beep after confirmed USB/VBUS insertion
 *   - one short beep only when the Volume page requests TEST
 *   - NO repeating timer
 */
esp_err_t koyoda_speaker_events_start(void);

void koyoda_speaker_events_beep_charge(void);
void koyoda_speaker_events_beep_test(void);

/*
 * Persistent master speaker volume.
 * Range: 0..100 (%)
 * Stored in NVS namespace "koyoda_audio".
 */
int koyoda_speaker_events_get_volume(void);
void koyoda_speaker_events_set_volume(int percent);

#ifdef __cplusplus
}
#endif
