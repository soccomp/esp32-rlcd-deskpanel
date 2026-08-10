#pragma once
/* ============================================================
 *  本地后端地址配置模板（提交 GitHub 的公开版本，不含真实主机名/IP）
 *
 *  使用方式：
 *    1. 把本文件复制为同目录下的 local_config.h
 *    2. 在 local_config.h 中填入你 Mac 的主机名 / 局域网 IP
 *      （该文件已被 .gitignore 忽略）
 *
 *  需要覆盖的宏（默认占位见 ui_schedule.h / cam_client.cpp / weather_client.cpp）：
 *    - SCHEDULE_API_HOST          Mac 的 mDNS 主机名（不含 .local）
 *    - SCHEDULE_API_FALLBACK_URL  后端兜底 URL（mDNS 解析失败时用）
 *    - CAM_PROXY_HOST             摄像头帧代理（同 Mac 后端）主机名
 *    - CAM_PROXY_FALLBACK         摄像头帧代理兜底 IP
 *    - WEATHER_QH_BACKEND         天气回退代理 URL
 * ============================================================ */
#define SCHEDULE_API_HOST         "YOUR_MAC_HOSTNAME"
#define SCHEDULE_API_FALLBACK_URL "http://YOUR_MAC_IP:8100/api/schedule"

#define CAM_PROXY_HOST            "YOUR_MAC_HOSTNAME"
#define CAM_PROXY_FALLBACK        "YOUR_MAC_IP"

#define WEATHER_QH_BACKEND        "http://YOUR_MAC_IP:8100/api/weather_qh"
