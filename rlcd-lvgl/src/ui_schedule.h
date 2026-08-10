#pragma once

#include "lvgl.h"
#include <time.h>      // time_t / mktime / localtime（会议 epoch 比较）

/* ============================================================
 *  ESP32 端「会议日程页」模块
 *  - 结构体数组驻留 PSRAM
 *  - HTTPClient 拉取后端 GET /api/schedule
 *  - ArduinoJson 解析
 *  - LVGL v8 卡片式渲染（个人会议反色高亮）
 * ============================================================ */

/* 后端 API 地址：通过 mDNS 解析 Mac 主机名动态获取 IP，无需硬编码 IP。
 * SCHEDULE_API_HOST 是 Mac 的 mDNS 主机名（不含 .local 后缀）。
 * 如果 mDNS 解析失败，回退到 SCHEDULE_API_FALLBACK_URL。
 * 真实主机名/IP 在本地 src/local_config.h（被 .gitignore 忽略，不提交 GitHub）；
 * 公开仓库无该文件时使用下方占位（复制 local_config.example.h 为 local_config.h 并填入自己的值）。 */
#if __has_include("local_config.h")
#include "local_config.h"
#endif
#ifndef SCHEDULE_API_HOST
#define SCHEDULE_API_HOST "YOUR_MAC_HOSTNAME"
#endif
#ifndef SCHEDULE_API_PORT
#define SCHEDULE_API_PORT 8100
#endif
#ifndef SCHEDULE_API_PATH
#define SCHEDULE_API_PATH "/api/schedule"
#endif
#ifndef SCHEDULE_API_FALLBACK_URL
#define SCHEDULE_API_FALLBACK_URL "http://YOUR_MAC_IP:8100/api/schedule"
#endif

#define MAX_MEETINGS 64

/* 单条会议记录：所有字符串驻留 PSRAM */
struct Meeting {
    char *date;          // YYYY-MM-DD
    char *weekday;       // 星期一
    char *time;          // 时间段，如 "13:30" / "09:30—12:00"
    char *location;      // 会议室
    char *title;         // 会议名称
    char *host;          // 主持人
    char *attendees;     // 参加单位及人员
    char *organizer;     // 承办部门
    bool  is_video_conf; // ★ 视频会议
    bool  is_secret_conf;// ▲ 涉密会议
    bool  is_my_meeting; // 是否我的会议（名字命中）
};

/* 初始化 Schedule 页 UI（在 Lvgl_lock 保护下由 ui_init 调用） */
void ui_schedule_init(lv_obj_t *parent);

/* 用最新数据重建卡片列表（调用者需持有 Lvgl_lock） */
void ui_schedule_rebuild(void);

/* 切换筛选：全部 <-> 我的会议（调用者需持有 Lvgl_lock） */
void ui_schedule_toggle_filter(void);

/* 翻页：KEY=下一页 / BOOT=上一页（调用者需持有 Lvgl_lock） */
void ui_schedule_next_page(void);
void ui_schedule_prev_page(void);

/* 当前筛选模式：0=全部, 1=我的会议 */
uint8_t ui_schedule_get_filter(void);

/* 是否已成功拉取过数据 */
bool ui_schedule_has_data(void);

/* 取“我的下一场会议”：在 is_my_meeting==true 中找距离当前最近的一场未来会议。
 * 找到返回 true 并填充 out_time/out_loc/out_title；否则返回 false（调用方显示“无”）。 */
bool ui_schedule_get_next_my_meeting(char *out_time, size_t tsz,
                                     char *out_loc,  size_t lsz,
                                     char *out_title, size_t ttl_sz);

/* 取“我的下一场会议”的开始 epoch（本地时区），供会议 10 分钟预告提醒判定。
 * 找到返回 true 并填充 *out_epoch；否则返回 false。 */
bool ui_schedule_get_next_my_meeting_epoch(time_t *out_epoch);

/* 从后端拉取会议日程并解析（内部自管理 Lvgl_lock 用于重建 UI） */
void fetch_schedule_data(void);

/* 轻量探测后端是否可达（GET /api/schedule，只看 HTTP 状态不解析）。
 * 非工作时段低频调用：后端恢复（电脑开机）时返回 true，供 main.cpp 触发全量刷新。 */
bool probe_backend(void);

/* 股票指数行情：mDNS 找本机后端 GET /api/stocks，解析后刷新主页右上卡 */
void fetch_stocks_data(void);

/* 从 SD 离线缓存恢复行情（fetch 失败/后端不可达时调用） */
void load_cached_stocks(void);

/* 覆盖默认 API 地址（需在 fetch 前调用） */
void ui_schedule_set_api_url(const char *url);
