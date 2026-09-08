#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * KOYODA Shared Audio Step 1
 *
 * One owner task initializes the Waveshare duplex I2S path once and keeps
 * both codecs open:
 *   ES7210 -> microphone input
 *   ES8311 -> speaker output
 *
 * Mic capture is continuous except for the very short event beep itself.
 * No second task re-initializes I2S.
 */
esp_err_t koyoda_audio_duplex_start(void);

bool koyoda_audio_duplex_is_ready(void);
bool koyoda_audio_duplex_mic_is_running(void);

/*
 * VAD Step 1:
 * true while KOYODA currently considers the user to be speaking.
 * This is intentionally exposed now so the next streaming step can reuse it.
 */
bool koyoda_audio_duplex_vad_is_speaking(void);

typedef void (*koyoda_audio_frame_cb_t)(
    const int16_t *samples,
    size_t sample_count,
    bool vad_speaking,
    void *user_ctx);

void koyoda_audio_duplex_set_frame_callback(
    koyoda_audio_frame_cb_t callback,
    void *user_ctx);

/* Quiet event beeps. */
void koyoda_audio_duplex_beep_charge(void);
void koyoda_audio_duplex_beep_test(void);

/* Persistent master speaker volume, 0..100. */
int  koyoda_audio_duplex_get_volume(void);
void koyoda_audio_duplex_set_volume(int percent);

#ifdef __cplusplus
}
#endif
