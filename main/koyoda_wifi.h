#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t koyoda_wifi_start(void);
bool koyoda_wifi_is_connected(void);
int koyoda_wifi_get_rssi(void);

#ifdef __cplusplus
}
#endif
