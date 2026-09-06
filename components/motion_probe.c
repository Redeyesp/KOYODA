#include <math.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "bsp/esp-bsp.h"
#include "qmi8658.h"

static const char *TAG = "KOYODA_MOTION";
static qmi8658_dev_t s_imu;

static void motion_probe_task(void *arg)
{
    (void)arg;

    /*
     * This component auto-starts without modifying main/main.c.
     * Wait for KOYODA's normal app_main() to bring up the BSP/I2C bus.
     */
    i2c_master_bus_handle_t bus = NULL;

    for (int retry = 0; retry < 40 && bus == NULL; ++retry) {
        vTaskDelay(pdMS_TO_TICKS(250));
        bus = bsp_i2c_get_handle();
    }

    if (bus == NULL) {
        ESP_LOGE(TAG, "BSP I2C bus was not ready");
        vTaskDelete(NULL);
        return;
    }

    /*
     * KOYODA's Waveshare 1.75 board uses the QMI8658 at 7-bit address 0x6B,
     * which is QMI8658_ADDRESS_HIGH in the Waveshare driver.
     */
    esp_err_t ret = qmi8658_init(&s_imu, bus, QMI8658_ADDRESS_HIGH);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "QMI8658 init failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    ret = qmi8658_set_accel_range(&s_imu, QMI8658_ACCEL_RANGE_8G);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set accel range failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    ret = qmi8658_set_accel_odr(&s_imu, QMI8658_ACCEL_ODR_125HZ);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set accel ODR failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    ret = qmi8658_set_gyro_range(&s_imu, QMI8658_GYRO_RANGE_512DPS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set gyro range failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    ret = qmi8658_set_gyro_odr(&s_imu, QMI8658_GYRO_ODR_125HZ);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set gyro ODR failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    qmi8658_set_accel_unit_mps2(&s_imu, true);
    qmi8658_set_gyro_unit_rads(&s_imu, true);

    ESP_LOGI(TAG, "QMI8658 ready - sensor probe running at 10 Hz");
    ESP_LOGI(TAG, "Move/tilt KOYODA gently; do not physically drop the board");

    while (1) {
        bool ready = false;
        ret = qmi8658_is_data_ready(&s_imu, &ready);

        if (ret == ESP_OK && ready) {
            qmi8658_data_t d = {0};

            ret = qmi8658_read_sensor_data(&s_imu, &d);
            if (ret == ESP_OK) {
                float accel_mag = sqrtf(
                    d.accelX * d.accelX +
                    d.accelY * d.accelY +
                    d.accelZ * d.accelZ
                );

                float gyro_mag = sqrtf(
                    d.gyroX * d.gyroX +
                    d.gyroY * d.gyroY +
                    d.gyroZ * d.gyroZ
                );

                ESP_LOGI(
                    TAG,
                    "A x=%+.2f y=%+.2f z=%+.2f |A|=%.2f m/s2  "
                    "G x=%+.2f y=%+.2f z=%+.2f |G|=%.2f rad/s",
                    d.accelX, d.accelY, d.accelZ, accel_mag,
                    d.gyroX, d.gyroY, d.gyroZ, gyro_mag
                );
            } else {
                ESP_LOGW(TAG, "sensor read failed: %s", esp_err_to_name(ret));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/*
 * The component is linked WHOLE_ARCHIVE so this constructor is kept even
 * though the existing KOYODA main.c does not explicitly reference it.
 *
 * The constructor only creates a FreeRTOS task. The task itself waits for
 * the normal KOYODA BSP initialization before touching I2C.
 */
static void __attribute__((constructor)) motion_probe_autostart(void)
{
    BaseType_t ok = xTaskCreate(
        motion_probe_task,
        "koyoda_motion",
        4096,
        NULL,
        3,
        NULL
    );

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create motion probe task");
    }
}
