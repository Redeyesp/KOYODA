#include "esp_log.h"

/*
 * Emergency recovery build:
 * The QMI8658 reaction task is intentionally disabled.
 *
 * This file has NO constructor and creates NO FreeRTOS task.
 * It exists only so the local component still builds cleanly.
 */

static const char *TAG = "KOYODA_MOTION";

void koyoda_motion_recovery_marker(void)
{
    ESP_LOGI(TAG, "Motion reaction disabled for recovery build");
}
