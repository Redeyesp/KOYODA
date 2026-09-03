#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

LV_IMAGE_DECLARE(koyoda_idle);
LV_IMAGE_DECLARE(koyoda_half);
LV_IMAGE_DECLARE(koyoda_closed);

static const char *TAG = "KOYODA";
static lv_obj_t *face_img = NULL;

static void show_face(const lv_image_dsc_t *face)
{
    bsp_display_lock(-1);
    lv_image_set_src(face_img, face);
    bsp_display_unlock();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting KOYODA Face Step 2 - Blink");

    /* Initialize Waveshare AMOLED + LVGL */
    bsp_display_start();

    bsp_display_lock(-1);

    /* Black screen background */
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Create KOYODA face */
    face_img = lv_image_create(screen);
    lv_image_set_src(face_img, &koyoda_idle);

    /* Rotate FACE clockwise 90 degrees */
    lv_image_set_pivot(face_img, 233, 233);
    lv_image_set_rotation(face_img, 900);

    lv_obj_center(face_img);

    bsp_display_unlock();

    ESP_LOGI(TAG, "KOYODA idle displayed.");

    while (1)
    {
        /* Idle */
        vTaskDelay(pdMS_TO_TICKS(3000));

        /* Closing */
        show_face(&koyoda_half);
        vTaskDelay(pdMS_TO_TICKS(60));

        show_face(&koyoda_closed);
        vTaskDelay(pdMS_TO_TICKS(90));

        /* Opening */
        show_face(&koyoda_half);
        vTaskDelay(pdMS_TO_TICKS(60));

        show_face(&koyoda_idle);
    }
}