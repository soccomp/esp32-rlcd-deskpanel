#include "ui.h"
#include "ui_clock.h"          // 主页（翻页钟 + 环境 + 行情/天气）模块
#include "ui_ambient.h"        // 环境与趣味页（木鱼 + 雷达 + 和弦）模块
#include "ui_camera.h"         // 摄像头预览页（第 3 页）
#include "lvgl.h"
#include "lv_font_chinese_18.h"

/* ----- 全局句柄 ----- */
static lv_obj_t * g_tileview   = NULL;
static lv_obj_t * g_sb_wifi    = NULL;
static lv_obj_t * g_sb_battery = NULL;

static uint8_t g_page = 0;

/* ----- 辅助：创建带中文字体的标签（非 static，供各页面模块复用） ----- */
lv_obj_t * cn_label(lv_obj_t * parent, const char * text)
{
    lv_obj_t * lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, &lv_font_chinese_18, 0);
    lv_label_set_text(lbl, text);
    return lbl;
}

/* ----- 三个页面内容 ----- */
static void build_clock_page(lv_obj_t * parent, lv_obj_t * status_bar)
{
    /* 主页交给独立模块：翻页数字钟 + 环境监测 + 行情/天气卡 */
    ui_clock_init(parent, status_bar);
}

static void build_ambient_page(lv_obj_t * parent)
{
    /* 环境与趣味页交给独立模块：电子木鱼 + 环境监测卡 + 办公室热闹雷达 */
    ui_ambient_init(parent);
}

static void build_camera_page(lv_obj_t * parent)
{
    /* 摄像头预览页：320x240 全屏预览（cam_client 提供 1bit 帧） */
    ui_camera_init(parent);
}

/* ----- 构建 UI ----- */
void ui_init(void)
{
    lv_obj_t * scr = lv_scr_act();

    /* 状态栏（固定在顶部 20px，不参与 tileview 滚动） */
    lv_obj_t * sb = lv_obj_create(scr);
    lv_obj_remove_style_all(sb);
    lv_obj_set_size(sb, SCREEN_W, STATUS_BAR_H);
    lv_obj_set_pos(sb, 0, 0);
    lv_obj_set_style_bg_opa(sb, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(sb, LV_OBJ_FLAG_SCROLLABLE);

    g_sb_wifi = lv_label_create(sb);
    lv_obj_set_style_text_font(g_sb_wifi, &lv_font_montserrat_14, 0);
    lv_label_set_text(g_sb_wifi, LV_SYMBOL_WIFI);
    lv_obj_align(g_sb_wifi, LV_ALIGN_LEFT_MID, 8, 0);

    g_sb_battery = lv_label_create(sb);
    lv_obj_set_style_text_font(g_sb_battery, &lv_font_montserrat_14, 0);
    lv_label_set_text(g_sb_battery, LV_SYMBOL_BATTERY_FULL);
    lv_obj_align(g_sb_battery, LV_ALIGN_RIGHT_MID, -8, 0);

    /* Tileview：三页水平排列，位于状态栏下方 */
    g_tileview = lv_tileview_create(scr);
    lv_obj_set_size(g_tileview, SCREEN_W, SCREEN_H - STATUS_BAR_H);
    lv_obj_set_pos(g_tileview, 0, STATUS_BAR_H);

    lv_obj_t * p0 = lv_tileview_add_tile(g_tileview, 0, 0, LV_DIR_RIGHT);
    build_clock_page(p0, sb);

    lv_obj_t * p1 = lv_tileview_add_tile(g_tileview, 1, 0, LV_DIR_HOR);
    build_ambient_page(p1);

    lv_obj_t * p2 = lv_tileview_add_tile(g_tileview, 2, 0, LV_DIR_LEFT);
    build_camera_page(p2);

    lv_obj_set_tile_id(g_tileview, 0, 0, LV_ANIM_OFF);
}

/* ----- 更新接口 ----- */
void ui_update_clock(const char *time_str, const char *date_str)
{
    /* 状态栏小钟已移除（大时钟由 ui_clock 定时器刷新），此接口保留签名以兼容 main.cpp */
    (void)time_str;
    (void)date_str;
}

void ui_update_wifi(bool connected)
{
    if (g_sb_wifi) lv_label_set_text(g_sb_wifi, connected ? LV_SYMBOL_WIFI : "");
}

void ui_update_battery(uint8_t percent)
{
    if (!g_sb_battery) return;
    const char * sym = LV_SYMBOL_BATTERY_EMPTY;
    if (percent >= 90)      sym = LV_SYMBOL_BATTERY_FULL;
    else if (percent >= 65) sym = LV_SYMBOL_BATTERY_3;
    else if (percent >= 40) sym = LV_SYMBOL_BATTERY_2;
    else if (percent >= 15) sym = LV_SYMBOL_BATTERY_1;
    lv_label_set_text(g_sb_battery, sym);
}

void ui_update_ambient(float temp, float humi)
{
    /* 转发到环境与趣味页的环境监测卡（含舒适度评价与趋势） */
    ui_ambient_update_env(temp, humi);
}

void ui_next_page(void)
{
    if (!g_tileview) return;
    g_page = (g_page + 1) % 3;
    /* 反射屏刷新慢，关掉切换动画，直接跳页更干脆 */
    lv_obj_set_tile_id(g_tileview, g_page, 0, LV_ANIM_OFF);
}

void ui_prev_page(void)
{
    if (!g_tileview) return;
    g_page = (g_page + 2) % 3;   // 上一页：0->2->1->0
    lv_obj_set_tile_id(g_tileview, g_page, 0, LV_ANIM_OFF);
}

/* 直接跳转到指定页面 */
void ui_goto_page(uint8_t p)
{
    if (!g_tileview) return;
    g_page = p % 3;
    lv_obj_set_tile_id(g_tileview, g_page, 0, LV_ANIM_OFF);
}

uint8_t ui_get_current_page(void)
{
    return g_page;
}
