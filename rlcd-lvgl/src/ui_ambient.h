#pragma once

#include "lvgl.h"

/* ============================================================
 *  Rock Station (Page 2) - Gallery-level UI
 *  Left: Fender Stratocaster image
 *  Right: ROCK LEVEL card / Chord learning panel / TEMP+HUMI card
 * ============================================================ */

/* 构建 Page 2 内容（在 Lvgl_lock 保护下由 ui_init 调用） */
void ui_ambient_init(lv_obj_t *parent);

/* KEY 键 (GPIO18) 在 Page 2：切换到下一个和弦 + 播放音频 */
void ui_ambient_next_chord(void);

/* BOOT 键 (GPIO0) 在 Page 2：摇滚计数 +1 + 播放当前和弦 */
void ui_ambient_tap(void);

/* BOOT 长按：切换 OPEN / 7TH 练习组并播放该组第一个和弦 */
void ui_ambient_next_group(void);

/* SHTC3 温湿度 -> 右下卡片（由 ui_clock 5s 定时器转发） */
void ui_ambient_update_env(float temp, float humi);

/* 进入 Page 2 时触发 */
void ui_ambient_on_show(void);

/* 启动后台任务（保留接口，实际不依赖雷达） */
void ui_ambient_start_radar(void);

/* 当前和弦序号（用于 CHORD 切换） */
uint8_t ui_ambient_current_chord(void);
