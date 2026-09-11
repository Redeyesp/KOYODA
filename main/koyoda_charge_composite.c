#include "koyoda_charge_composite.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "koyoda_ai_overlays.h"

LV_IMAGE_DECLARE(koyoda_idle);

static const char *TAG = "KOYODA_CHARGE";

#define KOYODA_FACE_W 466U
#define KOYODA_FACE_H 466U
#define KOYODA_RGB565_BYTES_PER_PIXEL 2U

static uint8_t *s_charge_frame = NULL;
static lv_image_dsc_t s_charge_images[4];

static const lv_image_dsc_t *patch_for_step(unsigned step)
{
    switch (step)
    {
        case 1U:
            return &koyoda_charge_patch_taste;
        case 2U:
            return &koyoda_charge_patch_bite;
        case 3U:
            return &koyoda_charge_patch_bolt;
        case 4U:
            return &koyoda_charge_patch_glow;
        default:
            return NULL;
    }
}

bool koyoda_charge_composite_init(void)
{
    if (s_charge_frame != NULL)
    {
        return true;
    }

    const size_t expected_bytes =
        KOYODA_FACE_W * KOYODA_FACE_H * KOYODA_RGB565_BYTES_PER_PIXEL;

    if (koyoda_idle.data == NULL || koyoda_idle.data_size < expected_bytes)
    {
        ESP_LOGE(TAG,
                 "Idle image is not raw 466x466 RGB565 (data_size=%u)",
                 (unsigned)koyoda_idle.data_size);
        return false;
    }

    /*
     * Force this work frame into external PSRAM.  It must not consume the
     * scarce internal DMA-capable heap that the CO5300 SPI driver and audio
     * path share.
     */
    s_charge_frame = (uint8_t *)heap_caps_malloc(
        expected_bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (s_charge_frame == NULL)
    {
        ESP_LOGE(TAG,
                 "Failed to allocate %u-byte PSRAM charge frame",
                 (unsigned)expected_bytes);
        return false;
    }

    memcpy(s_charge_frame, koyoda_idle.data, expected_bytes);

    /*
     * Four tiny descriptors all point at the same PSRAM work frame.
     * Using a different descriptor address for each visual step makes LVGL
     * take the exact same source-change path as the old stable full-screen
     * charging frames, without storing four/six full-screen images.
     */
    for (unsigned i = 0; i < 4U; ++i)
    {
        s_charge_images[i] = koyoda_idle;
        s_charge_images[i].data = s_charge_frame;
        s_charge_images[i].data_size = expected_bytes;
    }

    ESP_LOGI(TAG,
             "Charge composite ready in PSRAM: %u bytes; compact patches only",
             (unsigned)expected_bytes);

    return true;
}

bool koyoda_charge_composite_apply(unsigned step)
{
    if (!koyoda_charge_composite_init())
    {
        return false;
    }

    const lv_image_dsc_t *patch = patch_for_step(step);
    if (patch == NULL || patch->data == NULL)
    {
        return false;
    }

    if (patch->header.w != KOYODA_CHARGE_PATCH_W ||
        patch->header.h != KOYODA_CHARGE_PATCH_H)
    {
        ESP_LOGE(TAG,
                 "Unexpected charge patch size: %ux%u",
                 (unsigned)patch->header.w,
                 (unsigned)patch->header.h);
        return false;
    }

    const size_t patch_stride =
        KOYODA_CHARGE_PATCH_W * KOYODA_RGB565_BYTES_PER_PIXEL;
    const size_t expected_patch_bytes =
        patch_stride * KOYODA_CHARGE_PATCH_H;

    if (patch->data_size < expected_patch_bytes)
    {
        ESP_LOGE(TAG,
                 "Charge patch data too small: %u < %u",
                 (unsigned)patch->data_size,
                 (unsigned)expected_patch_bytes);
        return false;
    }

    /*
     * Compose in the ORIGINAL 466x466 asset coordinate system.  face_img is
     * already rotated 90 degrees by LVGL, exactly like koyoda_idle, so the
     * compact patch lands in the same place as the old full-screen frames.
     */
    for (unsigned row = 0; row < KOYODA_CHARGE_PATCH_H; ++row)
    {
        const size_t dst_offset =
            (((size_t)KOYODA_CHARGE_PATCH_Y + row) * KOYODA_FACE_W +
             KOYODA_CHARGE_PATCH_X) * KOYODA_RGB565_BYTES_PER_PIXEL;
        const size_t src_offset = (size_t)row * patch_stride;

        memcpy(
            s_charge_frame + dst_offset,
            patch->data + src_offset,
            patch_stride);
    }

    return true;
}

const lv_image_dsc_t *koyoda_charge_composite_image(unsigned step)
{
    if (s_charge_frame == NULL || step < 1U || step > 4U)
    {
        return &koyoda_idle;
    }

    return &s_charge_images[step - 1U];
}
