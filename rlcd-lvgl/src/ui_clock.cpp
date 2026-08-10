#include "ui_clock.h"
#include "ui.h"                 // cn_label()
#include "ui_schedule.h"       // ui_schedule_get_next_my_meeting()
#include "ui_camera.h"         // ui_camera_thumb_init()：首页摄像头缩略预览
#include "rtc_pcf85063.h"      // rtc_read_time / rtc_process_pending
#include "lv_font_chinese_18.h"
#include "lv_font_chinese_14.h"

/* LVGL 内置字体声明（默认 lvgl.h 不导出，独立引用） */
LV_FONT_DECLARE(lv_font_montserrat_12);
#include "weather_icons.h"     // 单色天气图标（24x24 indexed-1bit）
#include "brand_logo.h"        // 单色品牌标识（160x48 RGB565）
#include "french_quotes.h"     // 法语短句（内置 100 句，SD 词库兜底）
#include "french_lib.h"        // 法语词库抽象：SD 优先 + 内置兜底
#include <time.h>
#include <string.h>
#include <Arduino.h>

/* ============================================================
 *  控件句柄（仅本模块访问）
 * ============================================================ */
static lv_obj_t *g_flip_digits[4] = {nullptr}; // 四个数字位：HHMM（普通时钟，无翻页动画层）
static char g_flip_values[4] = {'\0', '\0', '\0', '\0'};
static lv_obj_t *g_date_label= nullptr;   // 日期 + 星期
static lv_obj_t *g_fr_quote  = nullptr;   // 顶部法语短句 label
static uint8_t   g_fr_idx    = 0;         // 当前法语短句索引
static int       g_fr_last_hour = -1;     // 上次轮换的"10分钟槽"标识(hour*6+min/10)
static uint8_t   g_fr_show_cn = 0;        // 0=显示法语 1=显示中文（50s法+10s中循环）
static uint32_t  g_fr_phase_last = 0;     // 上次 法/中 切换的时刻（epoch 秒）

/* ---- 右上：股票指数行情卡（上证/沪深300/创业板指） ---- */
/* ---- 右上：股票指数行情卡（弧形边框与底部卡一致，170x88 原 CAM 区域） ----
 * 三行：名称 点位 ▲/▼百分比；上涨=黑底白字反显，下跌=正常黑字。
 * 每行 = 行底条容器(固定 158×22, bg 黑/透明) + 内嵌文本 label。
 * 数据由 M1 后端 /api/stocks 提供，交易时段（工作日 09:30-11:30/13:00-15:30）
 * 每 10 分钟刷新（main.cpp），非交易时段不刷新、保留最后一次数据。 */
#define STOCK_ROWS 4
static lv_obj_t *g_stk_card = nullptr;            // 行情卡容器（弧形边框）
static lv_obj_t *g_stk_bg[STOCK_ROWS] = {0};     // 每行底条容器（决定反显宽度 = 158 全宽）
static lv_obj_t *g_stk_text[STOCK_ROWS] = {0};    // 每行内嵌文本 label
static lv_obj_t *g_stk_loading = nullptr;         // “行情获取中...” 占位

/* 行情数据缓存：set 侧（网络任务）只写缓存，update 侧（LVGL 任务）渲染 */
static volatile bool g_stk_valid = false;
static char g_stk_name[STOCK_ROWS][16] = {{0}};
static float g_stk_value[STOCK_ROWS] = {0};
static float g_stk_pct[STOCK_ROWS] = {0};
static bool  g_stk_up[STOCK_ROWS] = {false};

/* ---- 室内外气象卡（左下） ---- */
static lv_obj_t *g_env_icon_temp = nullptr; // 室内：温度计图标
static lv_obj_t *g_env_icon_humi = nullptr; // 室内：水滴图标
static lv_obj_t *g_env_temp  = nullptr;   // 室内：温度数值（l2 行）
static lv_obj_t *g_env_humi  = nullptr;   // 室内：湿度数值（l2 行）
static lv_obj_t *g_wx_loading   = nullptr; // “天气获取中...” 占位
static lv_obj_t *g_wx_row_today = nullptr; // 今日天气行
static lv_obj_t *g_wx_row_tomo  = nullptr; // 明日天气行
static lv_obj_t *g_wx_icon_today= nullptr; // 今日图标
static lv_obj_t *g_wx_icon_tomo = nullptr; // 明日图标
static lv_obj_t *g_wx_today_l1  = nullptr; // “今 晴”
static lv_obj_t *g_wx_today_l2  = nullptr; // “22~31°”
static lv_obj_t *g_wx_tomo_l1   = nullptr; // “明 阵雨”
static lv_obj_t *g_wx_tomo_l2   = nullptr; // “20~28°”
static lv_obj_t *g_wx_uptime    = nullptr; // 天气卡顶部“获取时间”label（两铆钉之间右对齐）
static char      g_wx_uptime_buf[16] = {0}; // 缓存的获取时间字符串

static const char *g_weekday_en[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

/* ============================================================
 *  天气数据缓存
 *  set 侧（网络任务）只写这份缓存，绝不触碰 LVGL 对象；
 *  update 侧（LVGL 任务/持锁）把缓存渲染到控件——两侧解耦，
 *  与 RTC 的 rtc_request_set/rtc_process_pending 同一套路。
 * ============================================================ */
typedef struct {
    volatile bool valid;       // 是否拿到过有效天气
    char city[32];
    char today_text[32];       // 今日天气描述（如 “雷阵雨”）
    char today_code[12];       // sun / cloud / rain / snow
    int  today_high, today_low;
    char tomo_text[32];
    char tomo_code[12];
    int  tomo_high, tomo_low;
} weather_info_t;

static weather_info_t g_weather = {0};

static const lv_coord_t FLIP_CARD_W = 40;
static const lv_coord_t FLIP_CARD_H = 64;   /* 缩小反色黑卡，让时钟居中且四角铆钉露出 */

/* 定时器回调前向声明（定义见文件末尾） */
static void clock_tick_cb(lv_timer_t *t);
static void env_tick_cb(lv_timer_t *t);

static lv_obj_t *make_rivet(lv_obj_t *parent)
{
    lv_obj_t *rivet = lv_obj_create(parent);
    lv_obj_remove_style_all(rivet);
    lv_obj_set_size(rivet, 7, 7);
    lv_obj_set_style_radius(rivet, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(rivet, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(rivet, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(rivet, lv_color_white(), 0);
    lv_obj_set_style_border_width(rivet, 1, 0);
    return rivet;
}

static void add_corner_rivets(lv_obj_t *parent, lv_coord_t inset)
{
    lv_obj_align(make_rivet(parent), LV_ALIGN_TOP_LEFT, inset, inset);
    lv_obj_align(make_rivet(parent), LV_ALIGN_TOP_RIGHT, -inset, inset);
    lv_obj_align(make_rivet(parent), LV_ALIGN_BOTTOM_LEFT, inset, -inset);
    lv_obj_align(make_rivet(parent), LV_ALIGN_BOTTOM_RIGHT, -inset, -inset);
}

static void make_dotted_rule(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                             lv_coord_t length, bool vertical,
                             lv_point_t points[2])
{
    points[0] = {0, 0};
    points[1] = {(lv_coord_t)(vertical ? 0 : length),
                 (lv_coord_t)(vertical ? length : 0)};

    /* 原生虚线只用一个对象；不要恢复成“每个点一个对象”，否则会耗尽 LVGL 堆。 */
    lv_obj_t *rule = lv_line_create(parent);
    lv_line_set_points(rule, points, 2);
    lv_obj_set_pos(rule, x, y);
    lv_obj_set_style_line_color(rule, lv_color_black(), 0);
    lv_obj_set_style_line_opa(rule, LV_OPA_60, 0);
    lv_obj_set_style_line_width(rule, 1, 0);
    lv_obj_set_style_line_dash_width(rule, 2, 0);
    lv_obj_set_style_line_dash_gap(rule, 3, 0);
}

/* ============================================================
 *  构建：单个数字卡片（普通时钟）
 *  保留原有 40x80 黑卡 + 白色大数字，去掉机械铰链/页缝/翻页动画层，
 *  外观与间隔和原翻页钟完全一致，只是数字直接刷新。
 * ============================================================ */
static lv_obj_t *make_flip_card(lv_obj_t *parent, const char *init)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, FLIP_CARD_W, FLIP_CARD_H);
    lv_obj_set_style_bg_color(card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 4, 0);
    lv_obj_set_style_border_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_outline_color(card, lv_color_black(), 0);
    lv_obj_set_style_outline_width(card, 1, 0);
    lv_obj_set_style_outline_pad(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dig = lv_label_create(card);
    lv_obj_set_style_text_font(dig, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(dig, lv_color_white(), 0);
    lv_label_set_text(dig, init);
    lv_obj_center(dig);

    return dig;
}

static void set_flip_digit(int index, char value)
{
    if (index < 0 || index >= 4 || !g_flip_digits[index]) return;
    if (g_flip_values[index] == value) return;   /* 未变化则跳过，减少刷新 */

    char text[2] = {value, '\0'};
    lv_label_set_text(g_flip_digits[index], text);
    g_flip_values[index] = value;
}

/* ============================================================
 *  构建：天气行（[24x24 图标] + 两行文字）
 *  普通态配色：黑图标 + 黑字（浅底深字，与整体调性一致）
 * ============================================================ */
static lv_obj_t *make_weather_row(lv_obj_t *parent,
                                  lv_obj_t **out_icon,
                                  lv_obj_t **out_l1, lv_obj_t **out_l2)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, 5, 0);

    lv_obj_t *icon = lv_img_create(row);
    lv_img_set_src(icon, &weather_icon_cloud);   // 占位，update 时按 code 换

    lv_obj_t *col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, 0, 0);

    lv_obj_t *l1 = lv_label_create(col);
    lv_obj_set_style_text_font(l1, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_color(l1, lv_color_black(), 0);
    lv_obj_set_width(l1, lv_pct(100));
    lv_label_set_long_mode(l1, LV_LABEL_LONG_DOT);
    lv_label_set_text(l1, "--");

    lv_obj_t *l2 = lv_label_create(col);
    lv_obj_set_style_text_font(l2, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_color(l2, lv_color_black(), 0);
    lv_obj_set_style_text_opa(l2, LV_OPA_80, 0);
    lv_obj_set_width(l2, lv_pct(100));
    lv_label_set_long_mode(l2, LV_LABEL_LONG_DOT);
    lv_label_set_text(l2, "--~--°");

    *out_icon = icon;
    *out_l1 = l1;
    *out_l2 = l2;
    return row;
}

/* ============================================================
 *  构建 Page 0 全部内容
 * ============================================================ */
void ui_clock_init(lv_obj_t *parent, lv_obj_t *status_bar)
{
    french_lib_init();   /* SD 词库优先，无卡/无文件用内置 100 句 */

    /* ---- 日期并入全局状态栏：MM-DD + 英文星期缩写，紧贴 WiFi 图标 ---- */
    g_date_label = lv_label_create(status_bar);
    lv_obj_set_style_text_font(g_date_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_date_label, lv_color_black(), 0);
    lv_label_set_text(g_date_label, "--.-----");
    lv_obj_align(g_date_label, LV_ALIGN_LEFT_MID, 26, 0);

    /* ---- 法语短句：紧贴日期右侧（留一个空格），每小时轮换（仅工作日 08-18 刷新）
     * 用 chinese_14 字体：含 Latin-1 重音字符(Ààèéêù)，montserrat_14 只含 ASCII 会出方框 ---- */
    g_fr_quote = lv_label_create(status_bar);
    lv_obj_set_style_text_font(g_fr_quote, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_color(g_fr_quote, lv_color_black(), 0);
    const char *fr0 = NULL, *cn0 = NULL;
    french_lib_get(0, &fr0, &cn0);
    lv_label_set_text(g_fr_quote, fr0 ? fr0 : kFrenchQuotes[0]);
    lv_obj_align_to(g_fr_quote, g_date_label, LV_ALIGN_OUT_RIGHT_MID, 4, 0);

    /* 页面从状态栏下方立即开始，释放原顶部题签占用的 26px。 */
    lv_obj_t *rule = lv_obj_create(parent);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, 384, 1);
    lv_obj_set_style_bg_color(rule, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_pos(rule, 8, 0);

    /* ---- 数字时钟：带外框与按钉的仪表模块（无翻页机械件） ---- */
    lv_obj_t *clock_frame = lv_obj_create(parent);
    lv_obj_remove_style_all(clock_frame);
    lv_obj_set_size(clock_frame, 207, 104);
    lv_obj_set_pos(clock_frame, 4, 5);
    lv_obj_set_style_bg_opa(clock_frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(clock_frame, lv_color_black(), 0);
    lv_obj_set_style_border_width(clock_frame, 2, 0);
    lv_obj_set_style_radius(clock_frame, 10, 0);
    lv_obj_set_style_pad_all(clock_frame, 0, 0);
    lv_obj_clear_flag(clock_frame, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *row = lv_obj_create(clock_frame);
    lv_obj_remove_style_all(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_set_height(row, FLIP_CARD_H);
    lv_obj_set_width(row, LV_SIZE_CONTENT);
    lv_obj_center(row);   /* 时钟整体在卡片内居中，四角铆钉露出 */

    g_flip_digits[0] = make_flip_card(row, "-");
    g_flip_digits[1] = make_flip_card(row, "-");

    lv_obj_t *colon = lv_label_create(row);
    lv_obj_set_style_text_font(colon, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(colon, lv_color_black(), 0);
    lv_label_set_text(colon, ":");
    lv_obj_set_style_pad_hor(colon, 0, 0);

    g_flip_digits[2] = make_flip_card(row, "-");
    g_flip_digits[3] = make_flip_card(row, "-");

    add_corner_rivets(clock_frame, 5);

    /* ---- 中央分区线：分隔左侧时钟列与右侧摄像头预留区 ---- */
    lv_obj_t *hero_div = lv_obj_create(parent);
    lv_obj_remove_style_all(hero_div);
    lv_obj_set_size(hero_div, 1, 96);
    lv_obj_set_pos(hero_div, 215, 8);
    lv_obj_set_style_bg_color(hero_div, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(hero_div, LV_OPA_30, 0);

    /* ---- 品牌 logo：移到时钟卡片正下方（左侧竖列第二项） ---- */
    lv_obj_t *logo = lv_img_create(parent);
    lv_img_set_src(logo, &shenzhou_media_logo);
    lv_obj_set_pos(logo, 8, 113);

    /* ---- 右上：股票指数行情卡（弧形边框与底部卡一致，170x104）
     * 四行：名称 点位 百分比；上涨=黑底白字反显，下跌=正常黑字。
     * 顶边 y=5 与左侧时钟卡(4,5)对齐；数据由 M1 后端 /api/stocks 提供。 */
    g_stk_card = lv_obj_create(parent);
    lv_obj_remove_style_all(g_stk_card);
    lv_obj_set_size(g_stk_card, 170, 104);
    lv_obj_set_pos(g_stk_card, 226, 5);
    lv_obj_set_style_bg_opa(g_stk_card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(g_stk_card, lv_color_black(), 0);
    lv_obj_set_style_border_width(g_stk_card, 2, 0);
    lv_obj_set_style_radius(g_stk_card, 10, 0);
    lv_obj_set_style_pad_all(g_stk_card, 4, 0);
    lv_obj_clear_flag(g_stk_card, LV_OBJ_FLAG_SCROLLABLE);

    /* 四行行情：每行 = 底条容器(固定 149×20, bg 黑/透明) + 内嵌文本 label
     * 容器 x=6(相对卡) → 实际左缘 238；宽 149 → 右缘 387（向右扩 3px 底色区域）；
     * 卡片右缘 396 → 右侧留白 9px（左留 12px）。y 起点 2、行距 22：整体垂直居中。 */
    for (int i = 0; i < STOCK_ROWS; ++i) {
        g_stk_bg[i] = lv_obj_create(g_stk_card);
        lv_obj_remove_style_all(g_stk_bg[i]);
        lv_obj_set_size(g_stk_bg[i], 149, 20);
        lv_obj_set_style_radius(g_stk_bg[i], 3, 0);
        lv_obj_set_style_pad_left(g_stk_bg[i], 4, 0);
        lv_obj_set_style_pad_top(g_stk_bg[i], 1, 0);
        lv_obj_clear_flag(g_stk_bg[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(g_stk_bg[i], LV_ALIGN_TOP_LEFT, 6, 2 + i * 22);

        g_stk_text[i] = lv_label_create(g_stk_bg[i]);
        lv_obj_set_style_text_font(g_stk_text[i], &lv_font_chinese_14, 0);
        lv_obj_set_style_text_color(g_stk_text[i], lv_color_black(), 0);
        lv_label_set_long_mode(g_stk_text[i], LV_LABEL_LONG_CLIP);
        lv_label_set_text(g_stk_text[i], "--  ---.--  ---.--%");
    }

    g_stk_loading = lv_label_create(g_stk_card);
    lv_obj_set_style_text_font(g_stk_loading, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_opa(g_stk_loading, LV_OPA_60, 0);
    lv_label_set_text(g_stk_loading, "行情\n获取中...");
    lv_obj_center(g_stk_loading);

    /* ============================================================
     * ---- 底部左：室内外气象卡（浅底深字 + 圆角细黑框） ----
     *  本卡缩窄为 184x111（右侧让出 34px 给 X 帖文卡）
     *   左半 72px  = 室内 SHTC3 大字温湿度
     *   分隔线 1px
     *   右半 91px  = 今日天气行 / 明日天气行（或“天气获取中...”）
     * ============================================================ */
    lv_obj_t *env = lv_obj_create(parent);
    lv_obj_remove_style_all(env);
    lv_obj_set_size(env, 184, 111);
    lv_obj_set_pos(env, 4, 165);
    lv_obj_set_style_bg_opa(env, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(env, lv_color_black(), 0);
    lv_obj_set_style_border_width(env, 2, 0);
    lv_obj_set_style_radius(env, 10, 0);
    lv_obj_set_style_pad_all(env, 6, 0);
    lv_obj_clear_flag(env, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 左半区：室内 SHTC3，与右侧天气行同构：图标 + 两行 14px 文字 ---- */
    lv_obj_t *indoor = lv_obj_create(env);
    lv_obj_remove_style_all(indoor);
    lv_obj_set_size(indoor, 72, 99);
    lv_obj_set_pos(indoor, 0, 0);
    lv_obj_set_flex_flow(indoor, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(indoor, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* 温度行：温度计图标 + 「室温」/「26.5°」 */
    lv_obj_t *g_env_l1_temp = nullptr, *g_env_l2_temp = nullptr;
    lv_obj_t *row_temp = make_weather_row(indoor, &g_env_icon_temp, &g_env_l1_temp, &g_env_l2_temp);
    lv_img_set_src(g_env_icon_temp, &weather_icon_thermo);
    lv_label_set_text(g_env_l1_temp, "室温");
    lv_label_set_text(g_env_l2_temp, "--.-°");
    g_env_temp = g_env_l2_temp;

    /* 湿度行：水滴图标 + 「湿度」/「52%」 */
    lv_obj_t *g_env_l1_humi = nullptr, *g_env_l2_humi = nullptr;
    lv_obj_t *row_humi = make_weather_row(indoor, &g_env_icon_humi, &g_env_l1_humi, &g_env_l2_humi);
    lv_img_set_src(g_env_icon_humi, &weather_icon_drop);
    lv_label_set_text(g_env_l1_humi, "湿度");
    lv_label_set_text(g_env_l2_humi, "--%");
    g_env_humi = g_env_l2_humi;
    (void)row_temp; (void)row_humi;

    /* ---- 右半区：今日 / 明日天气 ---- */
    lv_obj_t *outdoor = lv_obj_create(env);
    lv_obj_remove_style_all(outdoor);
    lv_obj_set_size(outdoor, 91, 99);
    lv_obj_set_pos(outdoor, 78, 0);
    lv_obj_set_flex_flow(outdoor, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(outdoor, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    g_wx_row_today = make_weather_row(outdoor, &g_wx_icon_today,
                                      &g_wx_today_l1, &g_wx_today_l2);
    g_wx_row_tomo  = make_weather_row(outdoor, &g_wx_icon_tomo,
                                      &g_wx_tomo_l1, &g_wx_tomo_l2);

    /* “天气获取中...” 占位（黑字，未取到数据时显示，天气行隐藏） */
    g_wx_loading = cn_label(outdoor, "天气\n获取中...");
    lv_obj_set_style_text_font(g_wx_loading, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_opa(g_wx_loading, LV_OPA_60, 0);

    /* 初始为“获取中”态；若解析兜底/缓存数据时已拿到天气则立即应用 */
    lv_obj_add_flag(g_wx_row_today, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_wx_row_tomo,  LV_OBJ_FLAG_HIDDEN);
    ui_clock_update_weather();

    /* 仪表卡内部的点阵格线：室内/室外分栏，两列横线齐平拆分上下两行 */
    static lv_point_t grid_points[3][2];
    make_dotted_rule(env, 76, 6, 88, true, grid_points[0]);
    make_dotted_rule(env, 8, 49, 62, false, grid_points[1]);
    make_dotted_rule(env, 82, 49, 78, false, grid_points[2]);

    /* 天气获取时间：顶部两铆钉之间靠左侧，避开右半天气内容。
     * 用"固定宽 + 文本右对齐"，不依赖 label 自动宽度（避免 align 时宽度未 recalc 导致越界不可见）。
     * 注：1bit 单色屏不支持中间灰度（LV_OPA_60 会被映射为完全透明→不可见），必须用 LV_OPA_COVER。 */
    g_wx_uptime = lv_label_create(env);
    lv_obj_set_style_text_font(g_wx_uptime, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_wx_uptime, lv_color_black(), 0);
    lv_obj_set_style_text_opa(g_wx_uptime, LV_OPA_COVER, 0);   /* 单色屏实心黑，COVER 不能用 60% */
    lv_obj_set_width(g_wx_uptime, 86);
    lv_obj_set_height(g_wx_uptime, 14);
    lv_label_set_long_mode(g_wx_uptime, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(g_wx_uptime, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_pad_top(g_wx_uptime, -3, 0);          /* 文字基线整体上移 3px，与铆钉圆心对齐 */
    lv_label_set_text(g_wx_uptime, "");
    /* 左起点 69 → 右缘 155；y=-1 上移2px（原 69,1） */
    lv_obj_set_pos(g_wx_uptime, 69, -1);

    add_corner_rivets(env, 3);

    /* ---- 底部右：摄像头缩略预览（原 X 帖文卡位置 196,116 200x160，改为 CAM 预览） ---- */
    ui_camera_thumb_init(parent, 196, 122, 200, 150);

    /* 定时器（运行于 LVGL 任务内，独占 I²C 总线） */
    lv_timer_create(clock_tick_cb, 1000, NULL);
    lv_timer_create(env_tick_cb,   5000, NULL);
}

/* ============================================================
 *  天气：缓存写入（任意任务安全，不触碰 LVGL 对象）
 * ============================================================ */
void ui_clock_set_weather(const char *city,
                          const char *today_text, const char *today_code,
                          int today_high, int today_low,
                          const char *tomorrow_text, const char *tomorrow_code,
                          int tomorrow_high, int tomorrow_low)
{
    if (!today_text || !today_code || !tomorrow_text || !tomorrow_code) return;
    if (today_text[0] == '\0') return;   /* 空描述视为无效，保持“获取中”态 */

    snprintf(g_weather.city,       sizeof(g_weather.city),       "%s", city ? city : "");
    snprintf(g_weather.today_text, sizeof(g_weather.today_text), "%s", today_text);
    snprintf(g_weather.today_code, sizeof(g_weather.today_code), "%s", today_code);
    snprintf(g_weather.tomo_text,  sizeof(g_weather.tomo_text),  "%s", tomorrow_text);
    snprintf(g_weather.tomo_code,  sizeof(g_weather.tomo_code),  "%s", tomorrow_code);
    g_weather.today_high = today_high;
    g_weather.today_low  = today_low;
    g_weather.tomo_high  = tomorrow_high;
    g_weather.tomo_low   = tomorrow_low;
    g_weather.valid = true;
}

/* ============================================================
 *  天气：渲染缓存到卡片（调用方需持有 Lvgl_lock 或处于 LVGL 任务）
 *  无有效数据 → 显示“天气获取中...”，两行天气隐藏（优雅退化）
 * ============================================================ */
void ui_clock_update_weather(void)
{
    if (!g_wx_loading) return;    /* UI 尚未构建 */

    if (!g_weather.valid) {
        lv_obj_clear_flag(g_wx_loading, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_wx_row_today, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_wx_row_tomo,  LV_OBJ_FLAG_HIDDEN);
        return;
    }

    char buf[48];

    lv_img_set_src(g_wx_icon_today, weather_icon_by_code(g_weather.today_code));
    snprintf(buf, sizeof(buf), "今 %s", g_weather.today_text);
    lv_label_set_text(g_wx_today_l1, buf);
    snprintf(buf, sizeof(buf), "%d~%d°", g_weather.today_low, g_weather.today_high);
    lv_label_set_text(g_wx_today_l2, buf);

    lv_img_set_src(g_wx_icon_tomo, weather_icon_by_code(g_weather.tomo_code));
    snprintf(buf, sizeof(buf), "明 %s", g_weather.tomo_text);
    lv_label_set_text(g_wx_tomo_l1, buf);
    snprintf(buf, sizeof(buf), "%d~%d°", g_weather.tomo_low, g_weather.tomo_high);
    lv_label_set_text(g_wx_tomo_l2, buf);

    lv_obj_add_flag(g_wx_loading, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_wx_row_today, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_wx_row_tomo,  LV_OBJ_FLAG_HIDDEN);

    /* 获取时间也随天气一并应用（若有缓存）；位置固定(87,3)，宽86右对齐，无需重align */
    if (g_wx_uptime && g_wx_uptime_buf[0]) {
        lv_label_set_text(g_wx_uptime, g_wx_uptime_buf);
    }
}

/* ============================================================
 *  天气：记录获取时间（MM-DD HH:MM），显示在天气卡顶部两铆钉之间
 *  仅缓存字符串，不触碰 LVGL 对象（任意任务安全）
 * ============================================================ */
void ui_clock_set_weather_time(const char *time_str)
{
    if (time_str && time_str[0]) {
        snprintf(g_wx_uptime_buf, sizeof(g_wx_uptime_buf), "%s", time_str);
    }
}

/* ============================================================
 *  迷你会议提示：首页反色卡已全部用于 X 帖文，不再显示会议。
 *  保留空函数以兼容 ui_schedule.cpp 的调用（会议页数据流不变）。
 * ============================================================ */
void ui_clock_sync_meeting(void)
{
}

/* ============================================================
 *  股票指数行情：缓存写入（任意任务安全，不触碰 LVGL 对象）
 *  由 fetch_stocks_data() 解析后端 /api/stocks 后调用。
 * ============================================================ */
void ui_clock_set_stocks(const char *names[3], const float values[3],
                         const float pcts[3], const bool ups[3])
{
    for (int i = 0; i < STOCK_ROWS; ++i) {
        if (names && names[i]) {
            snprintf(g_stk_name[i], sizeof(g_stk_name[i]), "%s", names[i]);
        }
        if (values) g_stk_value[i] = values[i];
        if (pcts)   g_stk_pct[i]   = pcts[i];
        if (ups)    g_stk_up[i]    = ups[i];
    }
    g_stk_valid = true;
}

/* ============================================================
 *  股票指数行情：渲染缓存到卡片（调用方需持有 Lvgl_lock 或处于 LVGL 任务）
 *  无有效数据 → 显示“行情获取中...”；有数据 → 三行行情。
 *  上涨(up)行黑底白字反显；下跌行保持透明底黑字。
 * ============================================================ */
void ui_clock_update_stocks(void)
{
    if (!g_stk_card) return;      /* UI 尚未构建 */

    if (!g_stk_valid) {
        lv_obj_clear_flag(g_stk_loading, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < STOCK_ROWS; ++i)
            lv_obj_add_flag(g_stk_bg[i], LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_add_flag(g_stk_loading, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < STOCK_ROWS; ++i) {
        char buf[64];
        /* 无箭头，靠反显/普通区分涨跌；名称左对齐 4 字符，点位固定宽度 9.2f，百分比 6.2f% */
        snprintf(buf, sizeof(buf), "%-4s %9.2f %6.2f%%",
                 g_stk_name[i][0] ? g_stk_name[i] : "--",
                 g_stk_value[i],
                 g_stk_pct[i] < 0 ? -g_stk_pct[i] : g_stk_pct[i]);
        lv_label_set_text(g_stk_text[i], buf);

        /* 上涨：底条容器黑底白字（黑底宽度 = 容器宽 146 全宽，左右各留 12px 对称）；
         * 下跌：底条容器透明底黑字 */
        if (g_stk_up[i]) {
            lv_obj_set_style_bg_color(g_stk_bg[i], lv_color_black(), 0);
            lv_obj_set_style_bg_opa(g_stk_bg[i], LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(g_stk_text[i], lv_color_white(), 0);
        } else {
            lv_obj_set_style_bg_opa(g_stk_bg[i], LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(g_stk_text[i], lv_color_black(), 0);
        }
        lv_obj_clear_flag(g_stk_bg[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* ============================================================
 *  1 秒定时器：读 PCF85063A RTC，刷新数字时钟 + 日期
 * ============================================================ */
static void clock_tick_cb(lv_timer_t *t)
{
    (void)t;
    rtc_process_pending();   /* 若 NTP 已同步，先把系统时间写入 RTC */

    struct tm tm;
    bool ok = rtc_read_time(&tm);

    if (!ok) {
        /* RTC 未就绪/掉电：退化为系统时间；系统时间也不合理则显示占位 */
        time_t now = time(nullptr);
        if (now > 1700000000UL) {
            localtime_r(&now, &tm);
        } else {
            for (int i = 0; i < 4; ++i) set_flip_digit(i, '-');
            lv_label_set_text(g_date_label, "--.-- ---");
            return;   /* 时间不可用，跳过后续刷新 */
        }
    }

    char hh[4], mm[4];
    strftime(hh, sizeof(hh), "%H", &tm);
    strftime(mm, sizeof(mm), "%M", &tm);
    set_flip_digit(0, hh[0]);
    set_flip_digit(1, hh[1]);
    set_flip_digit(2, mm[0]);
    set_flip_digit(3, mm[1]);

    char db[32];
    snprintf(db, sizeof(db), "%02d-%02d%s",
             tm.tm_mon + 1, tm.tm_mday,
             g_weekday_en[tm.tm_wday % 7]);
    lv_label_set_text(g_date_label, db);

    /* 日期文本变化后宽度可能改变：重新对齐法语短句，避免与星期缩写重叠
     * （align_to 只在调用时生效一次，必须在每次日期更新后重新执行） */
    if (g_fr_quote) {
        lv_obj_align_to(g_fr_quote, g_date_label, LV_ALIGN_OUT_RIGHT_MID, 4, 0);
    }

    /* ---- 法语短句轮换：每 10 分钟一句，期间 50s 法语 + 10s 中文循环 ----
     * 用 hour*6+min/10 作为"10 分钟槽"：槽变化即切下一句（随机），并重置为法语。
     * 同槽内按秒计数：法语 50s → 中文 10s → 法语 50s → …（整分钟对齐）。
     * 仅工作日(周一~周五) 08:00-18:00 刷新。 */
    {
        int slot = tm.tm_hour * 6 + tm.tm_min / 10;
        bool workday = (tm.tm_wday >= 1 && tm.tm_wday <= 5);
        bool worktime = (tm.tm_hour >= 8 && tm.tm_hour < 18);
        time_t now_epoch = time(nullptr);   /* NTP 同步后有效；与 RTC 显示同一时间源 */

        if (g_fr_quote && workday && worktime) {
            uint16_t fr_total = french_lib_count();
            if (slot != g_fr_last_hour) {
                /* 换新句：随机抽取（避免与上一句相同），显示法语 */
                g_fr_last_hour = slot;
                uint16_t next = (uint16_t)(esp_random() % fr_total);
                if (fr_total > 1 && next == g_fr_idx) {
                    next = (uint16_t)((next + 1) % fr_total);
                }
                g_fr_idx = next;
                g_fr_show_cn = 0;
                g_fr_phase_last = now_epoch;
                const char *fr = NULL, *cn = NULL;
                french_lib_get(g_fr_idx, &fr, &cn);
                lv_label_set_text(g_fr_quote, fr ? fr : kFrenchQuotes[g_fr_idx]);
            } else {
                /* 同槽：50s 法语 + 10s 中文循环（整分钟从法语开始） */
                uint32_t elapsed = (uint32_t)(now_epoch - g_fr_phase_last);
                const char *fr = NULL, *cn = NULL;
                french_lib_get(g_fr_idx, &fr, &cn);
                if (g_fr_show_cn == 0 && elapsed >= 50) {
                    g_fr_show_cn = 1;
                    g_fr_phase_last = now_epoch;
                    lv_label_set_text(g_fr_quote, cn ? cn : kFrenchTranslations[g_fr_idx]);
                } else if (g_fr_show_cn == 1 && elapsed >= 10) {
                    g_fr_show_cn = 0;
                    g_fr_phase_last = now_epoch;
                    lv_label_set_text(g_fr_quote, fr ? fr : kFrenchQuotes[g_fr_idx]);
                }
            }
        }
    }
}

/* ============================================================
 *  5 秒定时器：读 SHTC3 温湿度，刷新室内区 + 环境页
 *  注意：与天气数据完全独立——网络断开/天气未取到不影响本刷新
 * ============================================================ */
static void env_tick_cb(lv_timer_t *t)
{
    (void)t;
    float temp = 0, humi = 0;
    if (shtc3_read(&temp, &humi)) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%.1f°", temp);       /* 温度数值行 */
        lv_label_set_text(g_env_temp, buf);
        snprintf(buf, sizeof(buf), "%.0f%%", humi);      /* 湿度数值行（「湿度」已由 l1 标签提供） */
        lv_label_set_text(g_env_humi, buf);
        ui_update_ambient(temp, humi);   /* 同步第三页（环境页） */
    }
}
