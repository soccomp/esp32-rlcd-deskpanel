#pragma once

#include "lvgl.h"

/* 屏幕为横屏 400x300，顶部保留 20px 通用状态栏 */
#define SCREEN_W      400
#define SCREEN_H      300
#define STATUS_BAR_H  20

/* 创建带中文字体的标签（供各页面/会议页模块复用，定义见 ui.cpp） */
lv_obj_t * cn_label(lv_obj_t * parent, const char * text);

/* 构建全部 UI（在 Lvgl_lock 保护下首次调用） */
void ui_init(void);

/* 当前所在页面索引：0=主页 1=环境/吉他 2=摄像头 */
uint8_t ui_get_current_page(void);

/* 每秒更新一次的状态栏/主页时钟 */
void ui_update_clock(const char *time_str, const char *date_str);

/* Wi-Fi 连接状态 -> 状态栏图标 */
void ui_update_wifi(bool connected);

/* 电量百分比(0-100) -> 状态栏电池图标等级 */
void ui_update_battery(uint8_t percent);

/* SHTC3 真实温湿度 -> 环境页 */
void ui_update_ambient(float temp, float humi);

/* 按键回调：循环切到下一页（0->1->2->0） */
void ui_next_page(void);

/* 按键回调：循环切到上一页（0->2->1->0） */
void ui_prev_page(void);

/* 直接跳转到指定页面索引（0=主页 1=环境/吉他 2=摄像头） */
void ui_goto_page(uint8_t p);
