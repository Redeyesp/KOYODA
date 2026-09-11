#ifndef KOYODA_AI_OVERLAYS_H
#define KOYODA_AI_OVERLAYS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KOYODA_THINK_PATCH_X 204
#define KOYODA_THINK_PATCH_Y 79
#define KOYODA_THINK_PATCH_W 58
#define KOYODA_THINK_PATCH_H 10

#define KOYODA_SPEAK_PATCH_X 206
#define KOYODA_SPEAK_PATCH_Y 330
#define KOYODA_SPEAK_PATCH_W 53
#define KOYODA_SPEAK_PATCH_H 29

extern const lv_image_dsc_t koyoda_think_patch_01;
extern const lv_image_dsc_t koyoda_think_patch_02;
extern const lv_image_dsc_t koyoda_think_patch_03;

extern const lv_image_dsc_t koyoda_speak_patch_closed;
extern const lv_image_dsc_t koyoda_speak_patch_soft;
extern const lv_image_dsc_t koyoda_speak_patch_open;


#define KOYODA_CHARGE_PATCH_X 180
#define KOYODA_CHARGE_PATCH_Y 280
#define KOYODA_CHARGE_PATCH_W 106
#define KOYODA_CHARGE_PATCH_H 125

extern const lv_image_dsc_t koyoda_charge_patch_taste;
extern const lv_image_dsc_t koyoda_charge_patch_bite;
extern const lv_image_dsc_t koyoda_charge_patch_bolt;
extern const lv_image_dsc_t koyoda_charge_patch_glow;


#define KOYODA_BLINK_LEFT_X 65
#define KOYODA_BLINK_LEFT_Y 121
#define KOYODA_BLINK_LEFT_W 125

#define KOYODA_BLINK_RIGHT_X 278
#define KOYODA_BLINK_RIGHT_Y 121
#define KOYODA_BLINK_RIGHT_W 124

#define KOYODA_BLINK_TOP_H 75
#define KOYODA_BLINK_BOTTOM_Y 196
#define KOYODA_BLINK_BOTTOM_H 74

extern const lv_image_dsc_t koyoda_blink_half_left_top;
extern const lv_image_dsc_t koyoda_blink_half_left_bottom;
extern const lv_image_dsc_t koyoda_blink_half_right_top;
extern const lv_image_dsc_t koyoda_blink_half_right_bottom;

extern const lv_image_dsc_t koyoda_blink_closed_left_top;
extern const lv_image_dsc_t koyoda_blink_closed_left_bottom;
extern const lv_image_dsc_t koyoda_blink_closed_right_top;
extern const lv_image_dsc_t koyoda_blink_closed_right_bottom;

#ifdef __cplusplus
}
#endif

#endif
