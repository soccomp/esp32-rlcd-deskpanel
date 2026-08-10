#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/* 单色天气图标 24x24（LV_IMG_CF_INDEXED_1BIT，idx0 透明 / idx1 黑）
 * 晴=带射线圆圈  多云=云朵剪影  雨=云+下斜虚线  雪=云+雪花星
 * 室内指标：thermo=温度计  drop=水滴 */
extern const lv_img_dsc_t weather_icon_sun;
extern const lv_img_dsc_t weather_icon_cloud;
extern const lv_img_dsc_t weather_icon_rain;
extern const lv_img_dsc_t weather_icon_snow;
extern const lv_img_dsc_t weather_icon_thermo;
extern const lv_img_dsc_t weather_icon_drop;

/* 后端 weather code 字符串 -> 图标（未知 code 回退云朵） */
const lv_img_dsc_t *weather_icon_by_code(const char *code);

#ifdef __cplusplus
}
#endif
