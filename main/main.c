#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "lvgl.h"

#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "esp_io_expander.h"

#include "pmu_bridge.h"

LV_IMAGE_DECLARE(koyoda_idle);
LV_IMAGE_DECLARE(koyoda_half);
LV_IMAGE_DECLARE(koyoda_closed);
LV_IMAGE_DECLARE(koyoda_charge_1);
LV_IMAGE_DECLARE(koyoda_charge_2);
LV_IMAGE_DECLARE(koyoda_charge_3);
LV_IMAGE_DECLARE(koyoda_charge_4);
LV_IMAGE_DECLARE(koyoda_charge_5);
LV_IMAGE_DECLARE(koyoda_charge_6);

static const char *TAG = "KOYODA";

static lv_obj_t *face_img = NULL;
static lv_obj_t *power_overlay = NULL;

static esp_io_expander_handle_t io_expander = NULL;

static volatile bool power_dialog_open = false;

/* =========================================================
 * Face
 * ========================================================= */

static void show_face(const lv_image_dsc_t *face)
{
    if (power_dialog_open)
    {
        return;
    }

    bsp_display_lock(-1);

    if (face_img != NULL)
    {
        lv_image_set_src(face_img, face);
    }

    bsp_display_unlock();
}

/* =========================================================
 * Power Dialog
 * ========================================================= */

static void close_power_dialog(void)
{
    if (power_overlay != NULL)
    {
        lv_obj_delete(power_overlay);
        power_overlay = NULL;
    }

    power_dialog_open = false;

    ESP_LOGI(TAG, "Power dialog closed");
}

static void cancel_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        ESP_LOGI(TAG, "Power off cancelled");
        close_power_dialog();
    }
}

static void actual_power_off_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGI(TAG, "Sending shutdown command to AXP2101");
    pmu_bridge_shutdown();

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void power_off_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        ESP_LOGI(TAG, "POWER OFF requested");
        close_power_dialog();

        BaseType_t result = xTaskCreate(
            actual_power_off_task,
            "power_off",
            4096,
            NULL,
            6,
            NULL);

        if (result != pdPASS)
        {
            ESP_LOGE(TAG, "Failed to create power off task");
        }
    }
}

static void show_power_dialog(void)
{
    if (power_dialog_open)
    {
        return;
    }

    power_dialog_open = true;

    bsp_display_lock(-1);

    power_overlay = lv_obj_create(lv_screen_active());

    lv_obj_set_style_transform_pivot_x(power_overlay, 233, 0);
    lv_obj_set_style_transform_pivot_y(power_overlay, 233, 0);
    lv_obj_set_style_transform_rotation(power_overlay, 900, 0);

    lv_obj_set_size(power_overlay, 466, 466);
    lv_obj_center(power_overlay);
    lv_obj_set_style_bg_color(power_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(power_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(power_overlay, 0, 0);
    lv_obj_set_style_pad_all(power_overlay, 0, 0);
    lv_obj_clear_flag(power_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = lv_obj_create(power_overlay);
    lv_obj_set_size(panel, 340, 220);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x181818), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(panel, 28, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "Power off?");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

    lv_obj_t *cancel_btn = lv_button_create(panel);
    lv_obj_set_size(cancel_btn, 125, 62);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 22, -25);
    lv_obj_set_style_radius(cancel_btn, 18, 0);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x444444), 0);
    lv_obj_add_event_cb(cancel_btn, cancel_button_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(cancel_label);

    lv_obj_t *power_btn = lv_button_create(panel);
    lv_obj_set_size(power_btn, 145, 62);
    lv_obj_align(power_btn, LV_ALIGN_BOTTOM_RIGHT, -22, -25);
    lv_obj_set_style_radius(power_btn, 18, 0);
    lv_obj_set_style_bg_color(power_btn, lv_color_hex(0x9C2525), 0);
    lv_obj_add_event_cb(power_btn, power_off_button_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *power_label = lv_label_create(power_btn);
    lv_label_set_text(power_label, "Power Off");
    lv_obj_set_style_text_color(power_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(power_label);

    bsp_display_unlock();

    ESP_LOGI(TAG, "Power dialog shown");
}

/* =========================================================
 * Power Button Task
 * ========================================================= */

static void power_button_task(void *arg)
{
    bool was_pressed = false;
    bool long_press_reported = false;
    TickType_t press_start = 0;

    while (1)
    {
        uint32_t level = 0;

        esp_err_t err = esp_io_expander_get_level(
            io_expander,
            IO_EXPANDER_PIN_NUM_4,
            &level);

        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to read Power button: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        bool pressed = (level & IO_EXPANDER_PIN_NUM_4) != 0;

        if (pressed && !was_pressed)
        {
            ESP_LOGI(TAG, "PWR pressed");
            press_start = xTaskGetTickCount();
            long_press_reported = false;
            was_pressed = true;
        }

        if (pressed && was_pressed && !long_press_reported)
        {
            TickType_t held_ticks = xTaskGetTickCount() - press_start;
            uint32_t held_ms = held_ticks * portTICK_PERIOD_MS;

            if (held_ms >= 2000)
            {
                ESP_LOGI(TAG, "PWR LONG PRESS detected");
                long_press_reported = true;
                show_power_dialog();
            }
        }

        if (!pressed && was_pressed)
        {
            ESP_LOGI(TAG, "PWR released");
            was_pressed = false;
            long_press_reported = false;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* =========================================================
 * Charging animation helpers
 * ========================================================= */

static void play_charging_animation_once(void)
{
    const lv_image_dsc_t *frames[] = {
        &koyoda_charge_1,
        &koyoda_charge_2,
        &koyoda_charge_3,
        &koyoda_charge_4,
        &koyoda_charge_5,
        &koyoda_charge_6,
        &koyoda_charge_5,
        &koyoda_charge_1,
    };

    const uint16_t delays_ms[] = {120, 100, 100, 120, 100, 180, 100, 180};

    for (int i = 0; i < 8; i++)
    {
        while (power_dialog_open)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        show_face(frames[i]);
        vTaskDelay(pdMS_TO_TICKS(delays_ms[i]));
    }

    show_face(&koyoda_idle);
    ESP_LOGI(TAG, "Charging animation finished, back to idle");
}

static void idle_blink_step(void)
{
    if (power_dialog_open)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
        return;
    }

    show_face(&koyoda_idle);
    vTaskDelay(pdMS_TO_TICKS(3000));

    if (power_dialog_open) return;
    show_face(&koyoda_half);
    vTaskDelay(pdMS_TO_TICKS(60));

    if (power_dialog_open) return;
    show_face(&koyoda_closed);
    vTaskDelay(pdMS_TO_TICKS(90));

    if (power_dialog_open) return;
    show_face(&koyoda_half);
    vTaskDelay(pdMS_TO_TICKS(60));

    if (power_dialog_open) return;
    show_face(&koyoda_idle);
}

/* =========================================================
 * Main
 * ========================================================= */

void app_main(void)
{
    ESP_LOGI(TAG, "Starting KOYODA Face + Charging Once + Idle Blink");

    bsp_display_start();

    if (pmu_bridge_init() != 0)
    {
        ESP_LOGE(TAG, "PMU init failed");
    }
    else
    {
        ESP_LOGI(TAG, "PMU initialized");
    }

    io_expander = bsp_io_expander_init();
    if (io_expander == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize TCA9554 IO expander");
        return;
    }

    ESP_ERROR_CHECK(
        esp_io_expander_set_dir(
            io_expander,
            IO_EXPANDER_PIN_NUM_4,
            IO_EXPANDER_INPUT));

    bsp_display_lock(-1);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    face_img = lv_image_create(screen);
    lv_image_set_src(face_img, &koyoda_charge_1);
    lv_image_set_pivot(face_img, 233, 233);
    lv_image_set_rotation(face_img, 900);
    lv_obj_center(face_img);

    bsp_display_unlock();

    ESP_LOGI(TAG, "KOYODA startup charging animation ready.");

    xTaskCreate(
        power_button_task,
        "power_button",
        4096,
        NULL,
        5,
        NULL);

    play_charging_animation_once();

    while (1)
    {
        idle_blink_step();
    }
}
