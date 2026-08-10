#pragma once
/* ============================================================
 *  WiFi 凭据配置模板（提交 GitHub 的公开版本，不含真实密码）
 *
 *  struct WifiCredential 由 main.cpp 定义（先于本文件 include）。
 *  使用方式：
 *    1. 把本文件复制为同目录下的 wifi_config.h
 *    2. 在 wifi_config.h 中填入你自己的 Wi-Fi 网络（该文件已被 .gitignore 忽略）
 *
 *  固件会按顺序尝试列表中的网络，哪个先连上就用哪个。
 * ============================================================ */
#define WIFI_CONFIG_PRESENT

static const WifiCredential wifi_list[] = {
    { "YOUR_WIFI_SSID", "YOUR_WIFI_PASSWORD" },
    // { "你的热点名",     "密码"       },
};
static const int wifi_count = sizeof(wifi_list) / sizeof(wifi_list[0]);
