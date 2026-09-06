#include "koyoda_motion_inline.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "bsp/esp-bsp.h"
#include "qmi8658.h"

#include "fun_faces.h"

static const char *TAG = "KOYODA_MOTION";

/*
 * Thresholds tuned from KOYODA's real QMI8658 log.
 *
 * Rest:
 *   |A| ~= 10.29..10.31 m/s2
 *   |G| ~= 0.10..0.14 rad/s
 *
 * Strong hand motion:
 *   |G| commonly 4..8 rad/s
 *   peaks about 12.64 rad/s
 */
#define KOYODA_GRAVITY_BASE         10.30f

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

#define SENSOR_POLL_MS             100U
#define REACTION_COOLDOWN_MS       700U

typedef enum
{
    REACT_IDLE = 0,
    REACT_EXCITED,
    REACT_TILT,
    REACT_DROP,
    REACT_PEAK,
    REACT_DIZZY,
    REACT_HAPPY
} reaction_state_t;

static qmi8658_dev_t s_imu;
static bool s_ready = false;

static reaction_state_t s_state = REACT_IDLE;

static TickType_t s_last_poll_tick = 0;
static TickType_t s_state_enter_tick = 0;
static TickType_t s_settle_since_tick = 0;
static TickType_t s_cooldown_until_tick = 0;

static bool s_strong_latched = false;
static bool s_peak_latched = false;

static const char *state_name(reaction_state_t state)
{
    switch (state)
    {
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

static const lv_image_dsc_t *state_image(reaction_state_t state)
{
    switch (state)
    {
        case REACT_EXCITED: return &fun_excited;
        case REACT_TILT:    return &fun_tilt;
        case REACT_DROP:    return &fun_drop;
        case REACT_PEAK:    return &fun_peak_thrill;
        case REACT_DIZZY:   return &fun_dizzy;
        case REACT_HAPPY:   return &fun_happy;
        default:            return NULL;
    }
}

static uint32_t ticks_to_ms(TickType_t ticks)
{
    return (uint32_t)(ticks * portTICK_PERIOD_MS);
}

static uint32_t elapsed_ms(TickType_t start, TickType_t now)
{
    return ticks_to_ms(now - start);
}

static float clampf_local(float value, float low, float high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static void enter_state(
    reaction_state_t next,
    TickType_t now,
    float amag,
    float gmag,
    float tilt_deg)
{
    if (next == s_state)
    {
        return;
    }

    s_state = next;
    s_state_enter_tick = now;
    s_settle_since_tick = 0;

    ESP_LOGI(
        TAG,
        "STATE -> %s |A|=%.2f |G|=%.2f tilt=%.1f",
        state_name(next),
        amag,
        gmag,
        tilt_deg);
}

static void finish_reaction(TickType_t now)
{
    s_state = REACT_IDLE;
    s_state_enter_tick = now;
    s_settle_since_tick = 0;
    s_strong_latched = false;
    s_peak_latched = false;
    s_cooldown_until_tick = now + pdMS_TO_TICKS(REACTION_COOLDOWN_MS);

    ESP_LOGI(TAG, "STATE -> IDLE");
}

static void update_state_machine(
    TickType_t now,
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

    if (strong)
    {
        s_strong_latched = true;
    }

    if (peak)
    {
        s_peak_latched = true;
    }

    switch (s_state)
    {
        case REACT_IDLE:
            if ((int32_t)(now - s_cooldown_until_tick) < 0)
            {
                break;
            }

            if (mild)
            {
                s_strong_latched = strong;
                s_peak_latched = peak;
                enter_state(REACT_EXCITED, now, amag, gmag, tilt_deg);
            }
            break;

        case REACT_EXCITED:
            if (elapsed_ms(s_state_enter_tick, now) >= 200)
            {
                enter_state(REACT_TILT, now, amag, gmag, tilt_deg);
            }
            break;

        case REACT_TILT:
            if (s_strong_latched &&
                elapsed_ms(s_state_enter_tick, now) >= 200)
            {
                enter_state(REACT_DROP, now, amag, gmag, tilt_deg);
                break;
            }

            if (calm && upright)
            {
                if (s_settle_since_tick == 0)
                {
                    s_settle_since_tick = now;
                }
                else if (elapsed_ms(s_settle_since_tick, now) >= 400)
                {
                    enter_state(REACT_HAPPY, now, amag, gmag, tilt_deg);
                }
            }
            else
            {
                s_settle_since_tick = 0;
            }
            break;

        case REACT_DROP:
            if (elapsed_ms(s_state_enter_tick, now) >= 200)
            {
                enter_state(REACT_PEAK, now, amag, gmag, tilt_deg);
            }
            break;

        case REACT_PEAK:
            if (elapsed_ms(s_state_enter_tick, now) >= 400)
            {
                bool settled_enough =
                    (gmag < 3.0f) &&
                    (accel_dev < 2.5f);

                if (settled_enough ||
                    elapsed_ms(s_state_enter_tick, now) >= 1000)
                {
                    enter_state(REACT_DIZZY, now, amag, gmag, tilt_deg);
                }
            }
            break;

        case REACT_DIZZY:
            if (elapsed_ms(s_state_enter_tick, now) >= 700)
            {
                enter_state(REACT_HAPPY, now, amag, gmag, tilt_deg);
            }
            break;

        case REACT_HAPPY:
            if (elapsed_ms(s_state_enter_tick, now) >= 800)
            {
                finish_reaction(now);
            }
            break;
    }
}

esp_err_t koyoda_motion_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();

    if (bus == NULL)
    {
        ESP_LOGE(TAG, "BSP I2C bus is not ready");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = qmi8658_init(
        &s_imu,
        bus,
        QMI8658_ADDRESS_HIGH);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "QMI8658 init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = qmi8658_set_accel_range(
        &s_imu,
        QMI8658_ACCEL_RANGE_8G);
    if (ret != ESP_OK) return ret;

    ret = qmi8658_set_accel_odr(
        &s_imu,
        QMI8658_ACCEL_ODR_125HZ);
    if (ret != ESP_OK) return ret;

    ret = qmi8658_set_gyro_range(
        &s_imu,
        QMI8658_GYRO_RANGE_512DPS);
    if (ret != ESP_OK) return ret;

    ret = qmi8658_set_gyro_odr(
        &s_imu,
        QMI8658_GYRO_ODR_125HZ);
    if (ret != ESP_OK) return ret;

    qmi8658_set_accel_unit_mps2(&s_imu, true);
    qmi8658_set_gyro_unit_rads(&s_imu, true);

    TickType_t now = xTaskGetTickCount();

    s_last_poll_tick = now;
    s_state_enter_tick = now;
    s_cooldown_until_tick = now + pdMS_TO_TICKS(500);
    s_state = REACT_IDLE;
    s_ready = true;

    ESP_LOGI(
        TAG,
        "Step4 QMI8658 ready: inline app_main polling; no reaction task/timer");

    return ESP_OK;
}

void koyoda_motion_poll(void)
{
    if (!s_ready)
    {
        return;
    }

    TickType_t now = xTaskGetTickCount();

    if (elapsed_ms(s_last_poll_tick, now) < SENSOR_POLL_MS)
    {
        return;
    }

    s_last_poll_tick = now;

    bool data_ready = false;
    esp_err_t ret = qmi8658_is_data_ready(
        &s_imu,
        &data_ready);

    if (ret != ESP_OK || !data_ready)
    {
        return;
    }

    qmi8658_data_t data = {0};

    ret = qmi8658_read_sensor_data(
        &s_imu,
        &data);

    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "QMI8658 read failed: %s", esp_err_to_name(ret));
        return;
    }

    float amag = sqrtf(
        data.accelX * data.accelX +
        data.accelY * data.accelY +
        data.accelZ * data.accelZ);

    float gmag = sqrtf(
        data.gyroX * data.gyroX +
        data.gyroY * data.gyroY +
        data.gyroZ * data.gyroZ);

    float y_norm = 1.0f;

    if (amag > 0.001f)
    {
        y_norm = clampf_local(
            data.accelY / amag,
            -1.0f,
            1.0f);
    }

    /*
     * On the real KOYODA log, normal upright gravity is mainly +Y.
     */
    float tilt_deg =
        acosf(y_norm) * 57.2957795f;

    update_state_machine(
        now,
        amag,
        gmag,
        tilt_deg);
}

bool koyoda_motion_is_active(void)
{
    return s_ready && s_state != REACT_IDLE;
}

const lv_image_dsc_t *koyoda_motion_current_image(void)
{
    if (!koyoda_motion_is_active())
    {
        return NULL;
    }

    return state_image(s_state);
}
