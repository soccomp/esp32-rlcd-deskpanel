#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/* Fender Stratocaster 单色轮廓图 104x240（INDEXED_1BIT，idx0 透明 / idx1 黑）
 * 竖向：琴头在上、琴体在下。琴弦另由 lv_line 覆盖绘制（可振动）。
 * 关键几何（图内局部坐标，供 ui_ambient.cpp 对齐琴弦用）： */
#define GUITAR_IMG_W        104
#define GUITAR_IMG_H        240
#define GUITAR_NUT_Y        44    /* 上弦枕 y */
#define GUITAR_NUT_HALF     8    /* 上弦枕半宽 */
#define GUITAR_BRIDGE_Y     219    /* 琴桥弦马 y */
#define GUITAR_BRIDGE_HALF  11    /* 琴桥弦距半宽 */
#define GUITAR_CX           52    /* 中轴 x */

extern const lv_img_dsc_t guitar_strat;

#ifdef __cplusplus
}
#endif
