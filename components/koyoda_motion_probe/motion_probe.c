#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "qmi8658.h"

#include "fun_faces.h"

static const char *TAG = "KOYODA_MOTION";

/*
 * Thresholds tuned from the real KOYODA log captured on 2026-09-06.
 *
 * Rest:
 *   |A| ~= 10.29..10.31 m/s2
 *   |G| ~= 0.10..0.14 rad/s
 *
 * Hand motion:
 *   |G| often 2..7 rad/s
 *
 * Strong roller-coaster-like motion:
 *   |G| 8..12.64 rad/s
 *   |A| excursions up to ~40.62 m/s2
 */
#define KOYODA_GRAVITY_BASE        10.30f

#define TRIGGER_GYRO                0.80f
#define TRIGGER_ACCEL_DEV           0.90f
#define TRIGGER_TILT_DEG           12.0f

#define STRONG_GYRO                 4.00f
#define STRONG_ACCEL_DEV            2.50f

#define PEAK_GYRO                   7.50f
#define PEAK_ACCEL_DEV              5.50f

#define CALM_GYRO                   0.55f
#define CALM_ACCEL_DEV              0.80f
#define UPRIGHT_TILT_DEG            12.0f

typedef enum {
    REACT_IDLE = 0,
    REACT_EXCITED,
    REACT_TILT,
    REACT_DROP,
    REACT_PEAK,
    REACT_DIZZY,
    REACT_HAPPY,
} reaction_state_t;

static qmi8658_dev_t s_imu;
static lv_obj_t *s_face_candidate = NULL;
static lv_obj_t *s_overlay = NULL;

static reaction_state_t s_state = REACT_IDLE;
static TickType_t s_state_enter_tick = 0;
static TickType_t s_settle_since = 0;
static TickType_t s_cooldown_until = 0;

static bool s_strong_latched = false;
static bool s_peak_latched = false;

static const char *state_name(reaction_state_t s)
{
    switch (s) {
        case REACT_IDLE:         return "IDLE";
        case REACT_EXCITED:      return "EXCITED";
        case REACT_TILT:         return "TILT";
        case REACT_DROP:         return "DROP";
        case REACT_PEAK:         return "PEAK_THRILL";
        case REACT_DIZZY:        return "DIZZY";
        case REACT_HAPPY:        return "HAPPY";
        default:                 return "?";
    }
}

static const lv_image_dsc_t *state_image(reaction_state_t s)
{
    switch (s) {
        case REACT_EXCITED: return &fun_excited;
        case REACT_TILT:    return &fun_tilt;
        case REACT_DROP:    return &fun_drop;
        case REACT_PEAK:    return &fun_peak_thrill;
        case REACT_DIZZY:   return &fun_dizzy;
        case REACT_HAPPY:   return &fun_happy;
        default:            return NULL;
    }
}

static uint32_t elapsed_ms(TickType_t since)
{
    return (uint32_t)((xTaskGetTickCount() - since) * portTICK_PERIOD_MS);
}

static float clampf_local(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void render_state_locked(void)
{
    if (s_overlay == NULL || s_face_candidate == NULL) {
        return;
    }

    /*
     * Current KOYODA creates the main face image as the first direct child
     * of the active LVGL screen. When Battery is open, main.c hides that face.
     * Respect that state so motion reactions do not cover the Battery page.
     */
    bool face_visible = !lv_obj_has_flag(s_face_candidate, LV_OBJ_FLAG_HIDDEN);

    if (!face_visible || s_state == REACT_IDLE) {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const lv_image_dsc_t *img = state_image(s_state);
    if (img == NULL) {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_image_set_src(s_overlay, img);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void set_state(reaction_state_t next, float amag, float gmag, float tilt_deg)
{
    if (next == s_state) {
        return;
    }

    s_state = next;
    s_state_enter_tick = xTaskGetTickCount();
    s_settle_since = 0;

    ESP_LOGI(
        TAG,
        "STATE -> %s  |A|=%.2f  |G|=%.2f  tilt=%.1f deg",
        state_name(next), amag, gmag, tilt_deg
    );

    bsp_display_lock(-1);
    render_state_locked();
    bsp_display_unlock();
}

static bool create_overlay(void)
{
    bsp_display_lock(-1);

    lv_obj_t *screen = lv_screen_active();
    uint32_t count = lv_obj_get_child_count(screen);

    if (count == 0) {
        bsp_display_unlock();
        return false;
    }

    /*
     * This matches the current KOYODA UI construction order:
     * face image first, then Battery page, then swipe layer.
     */
    s_face_candidate = lv_obj_get_child(screen, 0);

    if (s_face_candidate == NULL) {
        bsp_display_unlock();
        return false;
    }

    s_overlay = lv_image_create(screen);
    lv_image_set_src(s_overlay, &fun_excited);
    lv_image_set_pivot(s_overlay, 233, 233);
    lv_image_set_rotation(s_overlay, 900);
    lv_obj_center(s_overlay);

    /* Never steal swipe/touch input from KOYODA. */
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    bsp_display_unlock();

    ESP_LOGI(TAG, "Reaction overlay ready");
    return true;
}

static void update_state_machine(
    float amag,
    float gmag,
    float tilt_deg
)
{
    TickType_t now = xTaskGetTickCount();
    float accel_dev = fabsf(amag - KOYODA_GRAVITY_BASE);

    bool mild =
        (gmag > TRIGGER_GYRO) ||
        (accel_dev > TRIGGER_ACCEL_DEV) ||
        (tilt_deg > TRIGGER_TILT_DEG);

    bool strong =
        (gmag > STRONG_GYRO) ||
        (accel_dev > STRONG_ACCEL_DEV);

    bool peak =
        (gmag > PEAK_GYRO) ||
        (accel_dev > PEAK_ACCEL_DEV);

    bool calm =
        (gmag < CALM_GYRO) &&
        (accel_dev < CALM_ACCEL_DEV);

    bool upright =
        (tilt_deg < UPRIGHT_TILT_DEG);

    if (strong) s_strong_latched = true;
    if (peak)   s_peak_latched = true;

    switch (s_state) {
        case REACT_IDLE:
            if ((int32_t)(now - s_cooldown_until) < 0) {
                break;
            }

            if (mild) {
                s_strong_latched = strong;
                s_peak_latched = peak;
                set_state(REACT_EXCITED, amag, gmag, tilt_deg);
            }
            break;

        case REACT_EXCITED:
            /*
             * Always give Excited enough screen time to read visually,
             * even if the user immediately makes a strong motion.
             */
            if (elapsed_ms(s_state_enter_tick) >= 160) {
                set_state(REACT_TILT, amag, gmag, tilt_deg);
            }
            break;

        case REACT_TILT:
            /*
             * Strong motion gets the full roller-coaster sequence.
             * A gentle tilt simply stays Tilt until returned upright.
             */
            if (s_strong_latched && elapsed_ms(s_state_enter_tick) >= 140) {
                set_state(REACT_DROP, amag, gmag, tilt_deg);
                break;
            }

            if (calm && upright) {
                if (s_settle_since == 0) {
                    s_settle_since = now;
                } else if (elapsed_ms(s_settle_since) >= 300) {
                    set_state(REACT_HAPPY, amag, gmag, tilt_deg);
                }
            } else {
                s_settle_since = 0;
            }
            break;

        case REACT_DROP:
            /*
             * Drop is intentionally a short punchy frame.
             * Once we reached this state, play Peak Thrill too.
             */
            if (elapsed_ms(s_state_enter_tick) >= 180) {
                set_state(REACT_PEAK, amag, gmag, tilt_deg);
            }
            break;

        case REACT_PEAK:
            /*
             * Hold Peak while the ride is still intense, but never forever.
             */
            if (elapsed_ms(s_state_enter_tick) >= 300) {
                bool settled_enough =
                    (gmag < 3.0f) &&
                    (accel_dev < 2.5f);

                if (settled_enough || elapsed_ms(s_state_enter_tick) >= 900) {
                    set_state(REACT_DIZZY, amag, gmag, tilt_deg);
                }
            }
            break;

        case REACT_DIZZY:
            if (elapsed_ms(s_state_enter_tick) >= 650) {
                set_state(REACT_HAPPY, amag, gmag, tilt_deg);
            }
            break;

        case REACT_HAPPY:
            if (elapsed_ms(s_state_enter_tick) >= 700) {
                s_state = REACT_IDLE;
                s_state_enter_tick = now;
                s_settle_since = 0;
                s_strong_latched = false;
                s_peak_latched = false;

                /*
                 * Prevent tiny settling vibrations from immediately starting
                 * another ride.
                 */
                s_cooldown_until = now + pdMS_TO_TICKS(700);

                ESP_LOGI(TAG, "STATE -> IDLE");

                bsp_display_lock(-1);
                render_state_locked();
                bsp_display_unlock();
            }
            break;
    }
}

static void reaction_task(void *arg)
{
    (void)arg;

    /*
     * Wait for the normal KOYODA app_main() to initialize BSP/I2C and UI.
     */
    i2c_master_bus_handle_t bus = NULL;

    for (int retry = 0; retry < 60 && bus == NULL; ++retry) {
        vTaskDelay(pdMS_TO_TICKS(250));
        bus = bsp_i2c_get_handle();
    }

    if (bus == NULL) {
        ESP_LOGE(TAG, "BSP I2C bus was not ready");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t ret = qmi8658_init(&s_imu, bus, QMI8658_ADDRESS_HIGH);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "QMI8658 init failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    ESP_ERROR_CHECK(qmi8658_set_accel_range(&s_imu, QMI8658_ACCEL_RANGE_8G));
    ESP_ERROR_CHECK(qmi8658_set_accel_odr(&s_imu, QMI8658_ACCEL_ODR_125HZ));
    ESP_ERROR_CHECK(qmi8658_set_gyro_range(&s_imu, QMI8658_GYRO_RANGE_512DPS));
    ESP_ERROR_CHECK(qmi8658_set_gyro_odr(&s_imu, QMI8658_GYRO_ODR_125HZ));
    qmi8658_set_accel_unit_mps2(&s_imu, true);
    qmi8658_set_gyro_unit_rads(&s_imu, true);

    /* Give KOYODA's existing UI a moment to finish creating its children. */
    vTaskDelay(pdMS_TO_TICKS(800));

    if (!create_overlay()) {
        ESP_LOGE(TAG, "Could not find/create KOYODA face overlay");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "QMI8658 reaction engine ready");
    ESP_LOGI(
        TAG,
        "Thresholds: trigger G>%.2f / tilt>%.0f deg; strong G>%.2f; peak G>%.2f",
        TRIGGER_GYRO, TRIGGER_TILT_DEG, STRONG_GYRO, PEAK_GYRO
    );

    while (1) {
        bool ready = false;
        ret = qmi8658_is_data_ready(&s_imu, &ready);

        if (ret == ESP_OK && ready) {
            qmi8658_data_t d = {0};

            ret = qmi8658_read_sensor_data(&s_imu, &d);

            if (ret == ESP_OK) {
                float amag = sqrtf(
                    d.accelX * d.accelX +
                    d.accelY * d.accelY +
                    d.accelZ * d.accelZ
                );

                float gmag = sqrtf(
                    d.gyroX * d.gyroX +
                    d.gyroY * d.gyroY +
                    d.gyroZ * d.gyroZ
                );

                float y_norm = 1.0f;
                if (amag > 0.001f) {
                    y_norm = clampf_local(d.accelY / amag, -1.0f, 1.0f);
                }

                float tilt_deg = acosf(y_norm) * (180.0f / (float)M_PI);

                update_state_machine(amag, gmag, tilt_deg);
            }
        }

        /*
         * Keep the overlay synchronized with FACE/BATTERY visibility even
         * when the reaction state itself has not changed.
         */
        bsp_display_lock(-1);
        render_state_locked();
        bsp_display_unlock();

        /* 20 Hz is responsive while remaining light on the ESP32-S3. */
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void __attribute__((constructor)) reaction_autostart(void)
{
    BaseType_t ok = xTaskCreate(
        reaction_task,
        "koyoda_reaction",
        6144,
        NULL,
        3,
        NULL
    );

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create reaction task");
    }
}
