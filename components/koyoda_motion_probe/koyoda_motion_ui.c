#include "koyoda_motion_ui.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"

#include "bsp/esp-bsp.h"
#include "qmi8658.h"

#include "fun_faces.h"

/*
 * These three symbols already exist in KOYODA's normal face firmware.
 * They let us recognize "normal face ownership" without knowing about
 * charging/sleep asset names.
 */
LV_IMAGE_DECLARE(koyoda_idle);
LV_IMAGE_DECLARE(koyoda_half);
LV_IMAGE_DECLARE(koyoda_closed);

static const char *TAG = "KOYODA_MOTION";

#define KOYODA_GRAVITY_BASE         10.30f

/* Tuned from KOYODA's real QMI8658 log on 2026-09-06. */
#define TRIGGER_GYRO                 0.80f
#define TRIGGER_ACCEL_DEV            0.90f
#define TRIGGER_TILT_DEG            12.0f

#define STRONG_GYRO                  4.00f
#define STRONG_ACCEL_DEV             2.50f

#define PEAK_GYRO                    7.50f
#define PEAK_ACCEL_DEV               5.50f

#define CALM_GYRO                    0.55f
#define CALM_ACCEL_DEV               0.80f
#define UPRIGHT_TILT_DEG            12.0f

#define MOTION_TIMER_MS             100

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
static lv_obj_t *s_face = NULL;
static lv_timer_t *s_timer = NULL;

static volatile reaction_state_t s_state = REACT_IDLE;
static uint32_t s_state_enter_ms = 0;
static uint32_t s_settle_since_ms = 0;
static uint32_t s_cooldown_until_ms = 0;

static bool s_strong_latched = false;
static bool s_peak_latched = false;

static const char *state_name(reaction_state_t s)
{
    switch (s) {
        case REACT_IDLE:    return "IDLE";
        case REACT_EXCITED: return "EXCITED";
        case REACT_TILT:    return "TILT";
        case REACT_DROP:    return "DROP";
        case REACT_PEAK:    return "PEAK_THRILL";
        case REACT_DIZZY:   return "DIZZY";
        case REACT_HAPPY:   return "HAPPY";
        default:            return "?";
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
        default:            return &koyoda_idle;
    }
}

static bool source_is_normal_or_motion(const void *src)
{
    return
        src == &koyoda_idle ||
        src == &koyoda_half ||
        src == &koyoda_closed ||
        src == &fun_excited ||
        src == &fun_tilt ||
        src == &fun_drop ||
        src == &fun_peak_thrill ||
        src == &fun_dizzy ||
        src == &fun_happy;
}

static uint32_t elapsed_from(uint32_t start_ms, uint32_t now_ms)
{
    return now_ms - start_ms;
}

static float clampf_local(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void reset_state_without_touching_face(uint32_t now_ms)
{
    s_state = REACT_IDLE;
    s_state_enter_ms = now_ms;
    s_settle_since_ms = 0;
    s_strong_latched = false;
    s_peak_latched = false;
}

static void set_state(
    reaction_state_t next,
    uint32_t now_ms,
    float amag,
    float gmag,
    float tilt_deg)
{
    if (next == s_state) {
        return;
    }

    s_state = next;
    s_state_enter_ms = now_ms;
    s_settle_since_ms = 0;

    ESP_LOGI(
        TAG,
        "STATE -> %s  |A|=%.2f  |G|=%.2f  tilt=%.1f deg",
        state_name(next),
        amag,
        gmag,
        tilt_deg);

    /*
     * IMPORTANT:
     * This function is called only from an LVGL timer callback.
     * Therefore we are already in LVGL context and must NOT call
     * bsp_display_lock() here.
     */
    if (s_face != NULL) {
        lv_image_set_src(s_face, state_image(next));
    }
}

static void finish_to_idle(uint32_t now_ms)
{
    s_state = REACT_IDLE;
    s_state_enter_ms = now_ms;
    s_settle_since_ms = 0;
    s_strong_latched = false;
    s_peak_latched = false;
    s_cooldown_until_ms = now_ms + 700;

    if (s_face != NULL) {
        lv_image_set_src(s_face, &koyoda_idle);
    }

    ESP_LOGI(TAG, "STATE -> IDLE");
}

static void update_state_machine(
    uint32_t now_ms,
    float amag,
    float gmag,
    float tilt_deg)
{
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
            if ((int32_t)(now_ms - s_cooldown_until_ms) < 0) {
                return;
            }

            if (mild) {
                s_strong_latched = strong;
                s_peak_latched = peak;
                set_state(REACT_EXCITED, now_ms, amag, gmag, tilt_deg);
            }
            break;

        case REACT_EXCITED:
            if (elapsed_from(s_state_enter_ms, now_ms) >= 200) {
                set_state(REACT_TILT, now_ms, amag, gmag, tilt_deg);
            }
            break;

        case REACT_TILT:
            if (s_strong_latched &&
                elapsed_from(s_state_enter_ms, now_ms) >= 200) {
                set_state(REACT_DROP, now_ms, amag, gmag, tilt_deg);
                break;
            }

            if (calm && upright) {
                if (s_settle_since_ms == 0) {
                    s_settle_since_ms = now_ms;
                } else if (elapsed_from(s_settle_since_ms, now_ms) >= 400) {
                    set_state(REACT_HAPPY, now_ms, amag, gmag, tilt_deg);
                }
            } else {
                s_settle_since_ms = 0;
            }
            break;

        case REACT_DROP:
            if (elapsed_from(s_state_enter_ms, now_ms) >= 200) {
                set_state(REACT_PEAK, now_ms, amag, gmag, tilt_deg);
            }
            break;

        case REACT_PEAK:
            /*
             * Hold the peak long enough to read visually.
             * Strong movement can keep it alive up to 1 second.
             */
            if (elapsed_from(s_state_enter_ms, now_ms) >= 400) {
                bool settled_enough =
                    (gmag < 3.0f) &&
                    (accel_dev < 2.5f);

                if (settled_enough ||
                    elapsed_from(s_state_enter_ms, now_ms) >= 1000) {
                    set_state(REACT_DIZZY, now_ms, amag, gmag, tilt_deg);
                }
            }
            break;

        case REACT_DIZZY:
            if (elapsed_from(s_state_enter_ms, now_ms) >= 700) {
                set_state(REACT_HAPPY, now_ms, amag, gmag, tilt_deg);
            }
            break;

        case REACT_HAPPY:
            if (elapsed_from(s_state_enter_ms, now_ms) >= 800) {
                finish_to_idle(now_ms);
            }
            break;
    }
}

static void motion_lvgl_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (s_face == NULL) {
        return;
    }

    uint32_t now_ms = lv_tick_get();

    /*
     * Battery page in the current KOYODA UI hides the face object.
     * Do nothing while hidden.
     */
    if (lv_obj_has_flag(s_face, LV_OBJ_FLAG_HIDDEN)) {
        reset_state_without_touching_face(now_ms);
        return;
    }

    /*
     * Charging and sleep animations use image sources that are NOT part of
     * normal blink or our motion assets. Treat them as higher-priority UI.
     *
     * This avoids coupling the gyro module to charging/sleep symbol names.
     */
    const void *src = lv_image_get_src(s_face);

    if (!source_is_normal_or_motion(src)) {
        reset_state_without_touching_face(now_ms);
        return;
    }

    bool ready = false;
    esp_err_t ret = qmi8658_is_data_ready(&s_imu, &ready);

    if (ret != ESP_OK || !ready) {
        return;
    }

    qmi8658_data_t d = {0};
    ret = qmi8658_read_sensor_data(&s_imu, &d);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 read failed: %s", esp_err_to_name(ret));
        return;
    }

    float amag = sqrtf(
        d.accelX * d.accelX +
        d.accelY * d.accelY +
        d.accelZ * d.accelZ);

    float gmag = sqrtf(
        d.gyroX * d.gyroX +
        d.gyroY * d.gyroY +
        d.gyroZ * d.gyroZ);

    float y_norm = 1.0f;

    if (amag > 0.001f) {
        y_norm = clampf_local(d.accelY / amag, -1.0f, 1.0f);
    }

    /*
     * KOYODA's normal upright log has gravity mainly on +Y.
     */
    float tilt_deg = acosf(y_norm) * 57.2957795f;

    update_state_machine(
        now_ms,
        amag,
        gmag,
        tilt_deg);

    /*
     * If a normal blink task briefly writes idle/half/closed during an active
     * reaction, restore our active expression on the next 100 ms tick.
     */
    if (s_state != REACT_IDLE) {
        const void *current_src = lv_image_get_src(s_face);

        if (current_src == &koyoda_idle ||
            current_src == &koyoda_half ||
            current_src == &koyoda_closed) {
            lv_image_set_src(s_face, state_image(s_state));
        }
    }
}

bool koyoda_motion_ui_is_active(void)
{
    return s_state != REACT_IDLE;
}

esp_err_t koyoda_motion_ui_init(lv_obj_t *face_img)
{
    if (face_img == NULL) {
        ESP_LOGE(TAG, "Cannot init: face_img is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_timer != NULL) {
        ESP_LOGW(TAG, "Motion UI already initialized");
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();

    if (bus == NULL) {
        ESP_LOGE(TAG, "BSP I2C bus is not ready");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = qmi8658_init(
        &s_imu,
        bus,
        QMI8658_ADDRESS_HIGH);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "QMI8658 init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = qmi8658_set_accel_range(
        &s_imu,
        QMI8658_ACCEL_RANGE_8G);

    if (ret != ESP_OK) {
        return ret;
    }

    ret = qmi8658_set_accel_odr(
        &s_imu,
        QMI8658_ACCEL_ODR_125HZ);

    if (ret != ESP_OK) {
        return ret;
    }

    ret = qmi8658_set_gyro_range(
        &s_imu,
        QMI8658_GYRO_RANGE_512DPS);

    if (ret != ESP_OK) {
        return ret;
    }

    ret = qmi8658_set_gyro_odr(
        &s_imu,
        QMI8658_GYRO_ODR_125HZ);

    if (ret != ESP_OK) {
        return ret;
    }

    qmi8658_set_accel_unit_mps2(&s_imu, true);
    qmi8658_set_gyro_unit_rads(&s_imu, true);

    s_face = face_img;

    /*
     * Create the timer under the BSP display lock exactly once.
     * The callback itself runs in LVGL context and NEVER takes this lock.
     */
    if (!bsp_display_lock(1000)) {
        ESP_LOGE(TAG, "Could not lock display to create motion timer");
        s_face = NULL;
        return ESP_ERR_TIMEOUT;
    }

    s_timer = lv_timer_create(
        motion_lvgl_timer_cb,
        MOTION_TIMER_MS,
        NULL);

    bsp_display_unlock();

    if (s_timer == NULL) {
        ESP_LOGE(TAG, "lv_timer_create failed");
        s_face = NULL;
        return ESP_ERR_NO_MEM;
    }

    reset_state_without_touching_face(lv_tick_get());

    ESP_LOGI(
        TAG,
        "Integrated QMI8658 reaction engine ready; no custom FreeRTOS task");

    ESP_LOGI(
        TAG,
        "Thresholds: trigger G>%.2f / tilt>%.0f deg; strong G>%.2f; peak G>%.2f",
        TRIGGER_GYRO,
        TRIGGER_TILT_DEG,
        STRONG_GYRO,
        PEAK_GYRO);

    return ESP_OK;
}
