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
 * STEP 5 ARCHITECTURE
 * -------------------
 * This deliberately returns to the one part that was already proven stable
 * on the real KOYODA hardware: QMI8658 reading from a small background task.
 *
 * IMPORTANT:
 * - This task NEVER calls LVGL.
 * - This task NEVER calls bsp_display_lock().
 * - This task NEVER changes face_img.
 * - The existing app_main()/face_animation_step() remains the ONLY owner of UI.
 *
 * Step 2 failed because the sensor task also touched LVGL.
 * Step 3 failed because QMI I2C was read from an LVGL timer.
 * Step 4 could block the existing app_main animation loop with synchronous I2C.
 *
 * Here the worker only updates a tiny reaction_state_t value.
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
#define SENSOR_START_DELAY_MS     1500U

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

static volatile bool s_worker_started = false;
static volatile bool s_sensor_ready = false;

/*
 * Protect the small state handoff between the sensor worker and app_main.
 * No LVGL work is done while holding this lock.
 */
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static reaction_state_t s_state = REACT_IDLE;

static TickType_t s_state_enter_tick = 0;
static TickType_t s_settle_since_tick = 0;
static TickType_t s_cooldown_until_tick = 0;

static bool s_strong_latched = false;

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

static uint32_t elapsed_ms(TickType_t start, TickType_t now)
{
    return (uint32_t)((now - start) * portTICK_PERIOD_MS);
}

static float clampf_local(float value, float low, float high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static void publish_state(reaction_state_t next)
{
    portENTER_CRITICAL(&s_state_mux);
    s_state = next;
    portEXIT_CRITICAL(&s_state_mux);
}

static reaction_state_t read_state(void)
{
    reaction_state_t state;

    portENTER_CRITICAL(&s_state_mux);
    state = s_state;
    portEXIT_CRITICAL(&s_state_mux);

    return state;
}

static void enter_state(
    reaction_state_t next,
    TickType_t now,
    float amag,
    float gmag,
    float tilt_deg)
{
    if (read_state() == next)
    {
        return;
    }

    publish_state(next);
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
    publish_state(REACT_IDLE);

    s_state_enter_tick = now;
    s_settle_since_tick = 0;
    s_strong_latched = false;
    s_cooldown_until_tick =
        now + pdMS_TO_TICKS(REACTION_COOLDOWN_MS);

    ESP_LOGI(TAG, "STATE -> IDLE");
}

static void update_state_machine(
    TickType_t now,
    float amag,
    float gmag,
    float tilt_deg)
{
    float accel_dev =
        fabsf(amag - KOYODA_GRAVITY_BASE);

    bool mild =
        (gmag > TRIGGER_GYRO) ||
        (accel_dev > TRIGGER_ACCEL_DEV) ||
        (tilt_deg > TRIGGER_TILT_DEG);

    bool strong =
        (gmag > STRONG_GYRO) ||
        (accel_dev > STRONG_ACCEL_DEV);

    bool calm =
        (gmag < CALM_GYRO) &&
        (accel_dev < CALM_ACCEL_DEV);

    bool upright =
        (tilt_deg < UPRIGHT_TILT_DEG);

    if (strong)
    {
        s_strong_latched = true;
    }

    reaction_state_t state = read_state();

    switch (state)
    {
        case REACT_IDLE:
            if ((int32_t)(now - s_cooldown_until_tick) < 0)
            {
                break;
            }

            if (mild)
            {
                s_strong_latched = strong;
                enter_state(
                    REACT_EXCITED,
                    now,
                    amag,
                    gmag,
                    tilt_deg);
            }
            break;

        case REACT_EXCITED:
            if (elapsed_ms(s_state_enter_tick, now) >= 200)
            {
                enter_state(
                    REACT_TILT,
                    now,
                    amag,
                    gmag,
                    tilt_deg);
            }
            break;

        case REACT_TILT:
            if (s_strong_latched &&
                elapsed_ms(s_state_enter_tick, now) >= 200)
            {
                enter_state(
                    REACT_DROP,
                    now,
                    amag,
                    gmag,
                    tilt_deg);
                break;
            }

            if (calm && upright)
            {
                if (s_settle_since_tick == 0)
                {
                    s_settle_since_tick = now;
                }
                else if (
                    elapsed_ms(
                        s_settle_since_tick,
                        now) >= 400)
                {
                    enter_state(
                        REACT_HAPPY,
                        now,
                        amag,
                        gmag,
                        tilt_deg);
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
                enter_state(
                    REACT_PEAK,
                    now,
                    amag,
                    gmag,
                    tilt_deg);
            }
            break;

        case REACT_PEAK:
            if (elapsed_ms(s_state_enter_tick, now) >= 400)
            {
                bool settled_enough =
                    (gmag < 3.0f) &&
                    (accel_dev < 2.5f);

                if (settled_enough ||
                    elapsed_ms(
                        s_state_enter_tick,
                        now) >= 1000)
                {
                    enter_state(
                        REACT_DIZZY,
                        now,
                        amag,
                        gmag,
                        tilt_deg);
                }
            }
            break;

        case REACT_DIZZY:
            if (elapsed_ms(s_state_enter_tick, now) >= 700)
            {
                enter_state(
                    REACT_HAPPY,
                    now,
                    amag,
                    gmag,
                    tilt_deg);
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

static void motion_sensor_task(void *arg)
{
    (void)arg;

    /*
     * Let KOYODA finish normal display / PMU / IO setup first.
     * The earlier standalone QMI probe was stable with delayed startup.
     */
    vTaskDelay(pdMS_TO_TICKS(SENSOR_START_DELAY_MS));

    i2c_master_bus_handle_t bus =
        bsp_i2c_get_handle();

    if (bus == NULL)
    {
        ESP_LOGE(
            TAG,
            "Sensor worker: BSP I2C bus not ready");
        s_worker_started = false;
        vTaskDelete(NULL);
        return;
    }

    esp_err_t ret = qmi8658_init(
        &s_imu,
        bus,
        QMI8658_ADDRESS_HIGH);

    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Sensor worker: QMI8658 init failed: %s",
            esp_err_to_name(ret));
        s_worker_started = false;
        vTaskDelete(NULL);
        return;
    }

    ret = qmi8658_set_accel_range(
        &s_imu,
        QMI8658_ACCEL_RANGE_8G);
    if (ret != ESP_OK) goto init_fail;

    ret = qmi8658_set_accel_odr(
        &s_imu,
        QMI8658_ACCEL_ODR_125HZ);
    if (ret != ESP_OK) goto init_fail;

    ret = qmi8658_set_gyro_range(
        &s_imu,
        QMI8658_GYRO_RANGE_512DPS);
    if (ret != ESP_OK) goto init_fail;

    ret = qmi8658_set_gyro_odr(
        &s_imu,
        QMI8658_GYRO_ODR_125HZ);
    if (ret != ESP_OK) goto init_fail;

    qmi8658_set_accel_unit_mps2(
        &s_imu,
        true);

    qmi8658_set_gyro_unit_rads(
        &s_imu,
        true);

    {
        TickType_t now =
            xTaskGetTickCount();

        s_state_enter_tick = now;
        s_settle_since_tick = 0;
        s_cooldown_until_tick =
            now + pdMS_TO_TICKS(500);

        publish_state(REACT_IDLE);
    }

    s_sensor_ready = true;

    ESP_LOGI(
        TAG,
        "Step5 QMI8658 ready: low-priority sensor-only task; UI stays in app_main");

    while (1)
    {
        bool data_ready = false;

        ret = qmi8658_is_data_ready(
            &s_imu,
            &data_ready);

        if (ret == ESP_OK && data_ready)
        {
            qmi8658_data_t data = {0};

            ret = qmi8658_read_sensor_data(
                &s_imu,
                &data);

            if (ret == ESP_OK)
            {
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
                 * Real KOYODA upright log has gravity mainly on +Y.
                 */
                float tilt_deg =
                    acosf(y_norm) * 57.2957795f;

                update_state_machine(
                    xTaskGetTickCount(),
                    amag,
                    gmag,
                    tilt_deg);
            }
            else
            {
                ESP_LOGW(
                    TAG,
                    "QMI8658 read failed: %s",
                    esp_err_to_name(ret));
            }
        }

        /*
         * A real delay is mandatory here.
         * It keeps this sensor worker far away from the watchdog issue
         * seen in the old UI-touching reaction task.
         */
        vTaskDelay(
            pdMS_TO_TICKS(SENSOR_POLL_MS));
    }

init_fail:
    ESP_LOGE(
        TAG,
        "Sensor worker configuration failed: %s",
        esp_err_to_name(ret));

    s_sensor_ready = false;
    s_worker_started = false;
    publish_state(REACT_IDLE);
    vTaskDelete(NULL);
}

esp_err_t koyoda_motion_init(void)
{
    if (s_worker_started)
    {
        return ESP_OK;
    }

    /*
     * IMPORTANT: return immediately.
     * QMI8658 is NOT initialized synchronously from app_main anymore.
     */
    s_worker_started = true;
    s_sensor_ready = false;
    publish_state(REACT_IDLE);

    BaseType_t result = xTaskCreate(
        motion_sensor_task,
        "motion_sensor",
        4096,
        NULL,
        1,
        NULL);

    if (result != pdPASS)
    {
        s_worker_started = false;

        ESP_LOGE(
            TAG,
            "Failed to create sensor-only task");

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Step5 sensor worker scheduled; normal KOYODA UI continues immediately");

    return ESP_OK;
}

void koyoda_motion_poll(void)
{
    /*
     * Intentionally empty.
     *
     * main.c can keep calling this function so no other file needs changing.
     * Sensor I2C work happens only in motion_sensor_task().
     */
}

bool koyoda_motion_is_active(void)
{
    return
        s_sensor_ready &&
        read_state() != REACT_IDLE;
}

const lv_image_dsc_t *koyoda_motion_current_image(void)
{
    if (!s_sensor_ready)
    {
        return NULL;
    }

    reaction_state_t state =
        read_state();

    return state_image(state);
}
