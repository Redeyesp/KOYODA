#include <stdio.h>
#include <stdlib.h>

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
static lv_obj_t *battery_page = NULL;
static lv_obj_t *battery_fill = NULL;
static lv_obj_t *battery_percent_label = NULL;
static lv_obj_t *battery_status_label = NULL;
static lv_obj_t *battery_voltage_label = NULL;
static lv_obj_t *swipe_layer = NULL;

static esp_io_expander_handle_t io_expander = NULL;

static volatile bool power_dialog_open = false;
static volatile bool battery_refresh_requested = false;

/* =========================================================
 * Page navigation
 *
 * PAGE_FUTURE is deliberately reserved now.  The enabled page
 * count stays at 2, so a second left swipe from Battery does
 * nothing yet.  Later we can enable PAGE_FUTURE without
 * rewriting the swipe system.
 * ========================================================= */

typedef enum
{
    PAGE_FACE = 0,
    PAGE_BATTERY,
    PAGE_FUTURE,
    PAGE_COUNT
} koyoda_page_t;

#define KOYODA_ENABLED_PAGE_COUNT 2
#define KOYODA_SWIPE_THRESHOLD_PX 70

static volatile koyoda_page_t current_page = PAGE_FACE;
static lv_point_t swipe_start = {0, 0};
static bool swipe_tracking = false;

/* =========================================================
 * Face
 * ========================================================= */

static void show_face(const lv_image_dsc_t *face)
{
    if (power_dialog_open || current_page != PAGE_FACE)
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
 * Battery UI
 * ========================================================= */

static void update_battery_ui_locked(const pmu_battery_status_t *status, bool valid)
{
    if (battery_page == NULL)
    {
        return;
    }

    if (!valid)
    {
        lv_label_set_text(battery_percent_label, "--%");
        lv_label_set_text(battery_status_label, "Battery unavailable");
        lv_label_set_text(battery_voltage_label, "");
        lv_obj_add_flag(battery_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(battery_status_label, lv_color_hex(0x888888), 0);
        return;
    }

    if (!status->battery_connected)
    {
        lv_label_set_text(battery_percent_label, "--%");
        lv_obj_add_flag(battery_fill, LV_OBJ_FLAG_HIDDEN);

        if (status->vbus_in)
        {
            lv_label_set_text(battery_status_label, "USB Power");
            lv_label_set_text(battery_voltage_label, "No Battery");
            lv_obj_set_style_text_color(battery_status_label, lv_color_hex(0x00D5D5), 0);
        }
        else
        {
            lv_label_set_text(battery_status_label, "No Battery");
            lv_label_set_text(battery_voltage_label, "");
            lv_obj_set_style_text_color(battery_status_label, lv_color_hex(0x888888), 0);
        }

        return;
    }

    int percent = status->battery_percent;
    if (percent < 0)
    {
        percent = 0;
        lv_label_set_text(battery_percent_label, "--%");
    }
    else
    {
        if (percent > 100)
        {
            percent = 100;
        }

        char percent_text[16];
        snprintf(percent_text, sizeof(percent_text), "%d%%", percent);
        lv_label_set_text(battery_percent_label, percent_text);
    }

    lv_obj_clear_flag(battery_fill, LV_OBJ_FLAG_HIDDEN);

    int fill_width = (190 * percent) / 100;
    if (fill_width < 4)
    {
        fill_width = 4;
    }
    lv_obj_set_width(battery_fill, fill_width);

    /* If external power is present while a battery is connected,
       present the state as Charging, even if the PMIC has already
       tapered/stopped charge current at full capacity. */
    if (status->vbus_in)
    {
        lv_label_set_text(battery_status_label, "Charging");
        lv_obj_set_style_text_color(battery_status_label, lv_color_hex(0xFF7FA3), 0);
    }
    else
    {
        lv_label_set_text(battery_status_label, "Battery");
        lv_obj_set_style_text_color(battery_status_label, lv_color_hex(0x00D5D5), 0);
    }

    char voltage_text[24];
    snprintf(
        voltage_text,
        sizeof(voltage_text),
        "%u.%03u V",
        (unsigned)(status->battery_voltage_mv / 1000),
        (unsigned)(status->battery_voltage_mv % 1000));
    lv_label_set_text(battery_voltage_label, voltage_text);
}

static void battery_status_task(void *arg)
{
    (void)arg;

    while (1)
    {
        if (current_page == PAGE_BATTERY || battery_refresh_requested)
        {
            pmu_battery_status_t status = {0};
            bool valid = (pmu_bridge_get_battery_status(&status) == 0);

            bsp_display_lock(-1);
            update_battery_ui_locked(&status, valid);
            bsp_display_unlock();

            if (valid)
            {
                ESP_LOGI(
                    TAG,
                    "Battery: present=%d vbus=%d charging=%d percent=%d voltage=%umV",
                    status.battery_connected,
                    status.vbus_in,
                    status.charging,
                    status.battery_percent,
                    status.battery_voltage_mv);
            }

            battery_refresh_requested = false;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void create_battery_page(lv_obj_t *screen)
{
    battery_page = lv_obj_create(screen);
    lv_obj_set_size(battery_page, 466, 466);
    lv_obj_center(battery_page);
    lv_obj_set_style_bg_color(battery_page, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(battery_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(battery_page, 0, 0);
    lv_obj_set_style_pad_all(battery_page, 0, 0);
    lv_obj_set_style_radius(battery_page, 0, 0);
    lv_obj_clear_flag(battery_page, LV_OBJ_FLAG_SCROLLABLE);

    /* Same confirmed physical orientation as the face assets. */
    lv_obj_set_style_transform_pivot_x(battery_page, 233, 0);
    lv_obj_set_style_transform_pivot_y(battery_page, 233, 0);
    lv_obj_set_style_transform_rotation(battery_page, 900, 0);

    lv_obj_t *title = lv_label_create(battery_page);
    lv_label_set_text(title, "BATTERY");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 70);

    /* Pixel-pet inspired battery: white shell + KOYODA cyan fill. */
    lv_obj_t *battery_body = lv_obj_create(battery_page);
    lv_obj_set_size(battery_body, 230, 108);
    lv_obj_align(battery_body, LV_ALIGN_CENTER, -10, -20);
    lv_obj_set_style_bg_color(battery_body, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(battery_body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(battery_body, 8, 0);
    lv_obj_set_style_border_color(battery_body, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(battery_body, 16, 0);
    lv_obj_set_style_pad_all(battery_body, 10, 0);
    lv_obj_clear_flag(battery_body, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *battery_tip = lv_obj_create(battery_page);
    lv_obj_set_size(battery_tip, 20, 48);
    lv_obj_align_to(battery_tip, battery_body, LV_ALIGN_OUT_RIGHT_MID, 2, 0);
    lv_obj_set_style_bg_color(battery_tip, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(battery_tip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(battery_tip, 0, 0);
    lv_obj_set_style_radius(battery_tip, 5, 0);
    lv_obj_clear_flag(battery_tip, LV_OBJ_FLAG_SCROLLABLE);

    battery_fill = lv_obj_create(battery_body);
    lv_obj_set_size(battery_fill, 190, 72);
    lv_obj_align(battery_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(battery_fill, lv_color_hex(0x00D5D5), 0);
    lv_obj_set_style_bg_opa(battery_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(battery_fill, 0, 0);
    lv_obj_set_style_radius(battery_fill, 8, 0);
    lv_obj_clear_flag(battery_fill, LV_OBJ_FLAG_SCROLLABLE);

    battery_percent_label = lv_label_create(battery_page);
    lv_label_set_text(battery_percent_label, "--%");
    lv_obj_set_style_text_color(battery_percent_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(battery_percent_label, &lv_font_montserrat_14, 0);
    lv_obj_align(battery_percent_label, LV_ALIGN_CENTER, 0, 70);

    battery_status_label = lv_label_create(battery_page);
    lv_label_set_text(battery_status_label, "Battery");
    lv_obj_set_style_text_color(battery_status_label, lv_color_hex(0x00D5D5), 0);
    lv_obj_set_style_text_font(battery_status_label, &lv_font_montserrat_14, 0);
    lv_obj_align(battery_status_label, LV_ALIGN_CENTER, 0, 102);

    battery_voltage_label = lv_label_create(battery_page);
    lv_label_set_text(battery_voltage_label, "");
    lv_obj_set_style_text_color(battery_voltage_label, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(battery_voltage_label, &lv_font_montserrat_14, 0);
    lv_obj_align(battery_voltage_label, LV_ALIGN_CENTER, 0, 132);

    lv_obj_add_flag(battery_page, LV_OBJ_FLAG_HIDDEN);
}

/* Called only from an LVGL event callback, so do not take the BSP LVGL lock here. */
static void set_page_from_lvgl(koyoda_page_t page)
{
    if (page < PAGE_FACE || page >= KOYODA_ENABLED_PAGE_COUNT)
    {
        return;
    }

    current_page = page;

    if (page == PAGE_FACE)
    {
        lv_obj_add_flag(battery_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(face_img, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(face_img, &koyoda_idle);
        ESP_LOGI(TAG, "Page -> FACE");
    }
    else if (page == PAGE_BATTERY)
    {
        lv_obj_add_flag(face_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(battery_page, LV_OBJ_FLAG_HIDDEN);
        battery_refresh_requested = true;
        ESP_LOGI(TAG, "Page -> BATTERY");
    }
}

static void navigate_next_from_lvgl(void)
{
    int next = (int)current_page + 1;

    if (next >= KOYODA_ENABLED_PAGE_COUNT)
    {
        ESP_LOGI(TAG, "Next page slot is reserved for a future menu");
        return;
    }

    set_page_from_lvgl((koyoda_page_t)next);
}

static void navigate_previous_from_lvgl(void)
{
    int previous = (int)current_page - 1;

    if (previous < PAGE_FACE)
    {
        return;
    }

    set_page_from_lvgl((koyoda_page_t)previous);
}

static void swipe_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_event_get_indev(e);

    if (indev == NULL || power_dialog_open)
    {
        swipe_tracking = false;
        return;
    }

    if (code == LV_EVENT_PRESSED)
    {
        lv_indev_get_point(indev, &swipe_start);
        swipe_tracking = true;
        return;
    }

    if (code == LV_EVENT_PRESS_LOST)
    {
        swipe_tracking = false;
        return;
    }

    if (code != LV_EVENT_RELEASED || !swipe_tracking)
    {
        return;
    }

    lv_point_t end = {0, 0};
    lv_indev_get_point(indev, &end);
    swipe_tracking = false;

    int dx = (int)end.x - (int)swipe_start.x;
    int dy = (int)end.y - (int)swipe_start.y;
    int abs_dx = abs(dx);
    int abs_dy = abs(dy);

    if (abs_dx < KOYODA_SWIPE_THRESHOLD_PX && abs_dy < KOYODA_SWIPE_THRESHOLD_PX)
    {
        return;
    }

    /* The confirmed UI is rotated 90 degrees on this board.  Depending on
       whether the BSP has already rotated touch coordinates, a physical
       left/right swipe can arrive on either the X or Y axis.  Supporting
       the dominant axis keeps navigation correct in both cases.

       Physical LEFT  -> negative dominant delta -> next page
       Physical RIGHT -> positive dominant delta -> previous page
     */
    int dominant_delta = (abs_dy >= abs_dx) ? dy : dx;

    if (dominant_delta < 0)
    {
        navigate_next_from_lvgl();
    }
    else
    {
        navigate_previous_from_lvgl();
    }
}

static void create_swipe_layer(lv_obj_t *screen)
{
    swipe_layer = lv_obj_create(screen);
    lv_obj_set_size(swipe_layer, 466, 466);
    lv_obj_center(swipe_layer);
    lv_obj_set_style_bg_opa(swipe_layer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(swipe_layer, 0, 0);
    lv_obj_set_style_pad_all(swipe_layer, 0, 0);
    lv_obj_set_style_radius(swipe_layer, 0, 0);
    lv_obj_clear_flag(swipe_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(swipe_layer, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(swipe_layer, swipe_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(swipe_layer, swipe_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(swipe_layer, swipe_event_cb, LV_EVENT_PRESS_LOST, NULL);
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
    (void)arg;
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
    (void)arg;
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
    ESP_LOGI(TAG, "Starting KOYODA Face + Swipe Battery Page");

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

    create_battery_page(screen);
    create_swipe_layer(screen);

    bsp_display_unlock();

    ESP_LOGI(TAG, "KOYODA UI ready: Face <-> Battery; future page slot reserved");

    xTaskCreate(
        power_button_task,
        "power_button",
        4096,
        NULL,
        5,
        NULL);

    xTaskCreate(
        battery_status_task,
        "battery_status",
        4096,
        NULL,
        4,
        NULL);

    play_charging_animation_once();

    while (1)
    {
        idle_blink_step();
    }
}
