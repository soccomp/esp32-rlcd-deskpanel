#pragma once

#include <stdint.h>

/* ============================================================
 *  ES8311 音频模块（板载 CODEC + 功放 PA）
 *  - I²C 复用 SHTC3 同一条 Wire 总线（SDA=13/SCL=14），ES8311 从地址 0x18
 *  - I²S 使用 legacy driver/i2s.h（本框架为 ESP-IDF 4.4 风格，无 i2c_master.h）
 *    引脚：DOUT=GPIO8  BCLK=GPIO9  DIN=GPIO10  MCLK=GPIO16  LRCLK=GPIO45
 *  - 功放使能 PA_EN=GPIO46（拉高才出声）
 *  - 采样率 16kHz / 16bit / 立体声（标准 I2S），MCLK = 256×16000 = 4.096MHz
 *  - 播放走独立 FreeRTOS 任务，调用方仅向队列投递请求，绝不阻塞 LVGL 渲染任务
 *  - 吉他模式：根据 ChordData 预合成开放和弦与七和弦 Karplus-Strong 清音琶音，
 *    连按可打断当前播放立即响应新和弦。
 * ============================================================ */

/* 初始化 ES8311 + I²S，并使能功放，启动播放任务。须在 Wire.begin 之后调用一次 */
void audio_init(void);

/* 非阻塞：播放 ChordData 中第 idx 段吉他琶音。连按会打断上一段。 */
void audio_play_chord(uint8_t idx);

/* 返回可用和弦数量（用于取模循环） */
uint8_t audio_chord_count(void);

/* 非阻塞：播放两声温和提醒 beep（通用提醒用） */
void audio_play_beep(void);

/* 设置 ES8311 DAC 音量寄存器 0..255（约 -95.5dB .. +32dB），默认 0xA8 */
void audio_set_volume(uint8_t reg);

/* 调试辅助（仅供临时音频测试固件使用） */
bool     audio_diag_ready(void);
int      audio_diag_write_result(void);
int      audio_diag_reg_read(uint8_t reg);    /* 回读 ES8311 寄存器，-1=无应答 */
