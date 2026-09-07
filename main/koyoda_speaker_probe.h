#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t koyoda_speaker_probe_start(void);
bool koyoda_speaker_probe_is_finished(void);

#ifdef __cplusplus
}
#endif
