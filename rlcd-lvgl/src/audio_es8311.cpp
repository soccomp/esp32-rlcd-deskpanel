#include "audio_es8311.h"
#include "chord_data.h"

#include <Arduino.h>
#include <math.h>        // sinf / expf / PI
#include <Wire.h>
#include "driver/i2s.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

/* ---------------- 硬件常量 ---------------- */
#define ES8311_ADDR   0x18
#define I2S_PORT      I2S_NUM_0
#define PA_PIN        46

#define I2S_BCK       9
#define I2S_WS        45
#define I2S_DO        8
#define I2S_DI        -1
#define I2S_MCK       16

#define SAMPLE_RATE   16000
#define BEEP_FRAMES   4480           // 两声 beep + 间隔

/* 吉他琶音：每和弦 5 音，总时长 ~1.38s @16k 立体声帧数 */
#define CHORD_FRAMES  22080          // ≈1.38s（含尾音衰减）
#define NOTE_GAP      1440           // 音间间隔 ≈90ms（琶音扫弦感）
#define NOTE_RING     10240          // 单音延音 ≈0.64s

/* ES8311 寄存器（见 es8311_reg.h） */
#define R00 0x00
#define R01 0x01
#define R02 0x02
#define R03 0x03
#define R04 0x04
#define R05 0x05
#define R06 0x06
#define R07 0x07
#define R08 0x08
#define R09 0x09
#define R0A 0x0A
#define R0B 0x0B
#define R0C 0x0C
#define R0D 0x0D
#define R0E 0x0E
#define R10 0x10
#define R11 0x11
#define R12 0x12
#define R13 0x13
#define R14 0x14
#define R15 0x15
#define R16 0x16
#define R17 0x17
#define R1B 0x1B
#define R1C 0x1C
#define R31 0x31
#define R32 0x32
#define R37 0x37
#define R44 0x44
#define R45 0x45

/* ---------------- 静态状态 ---------------- */
static int16_t *g_chord[CHORD_DATA_COUNT] = {nullptr};
static int16_t *g_beep     = nullptr;
static QueueHandle_t g_audio_q = nullptr;
static bool     g_audio_ready = false;
static int      g_last_wr = 0;

/* ---------------- I²C 寄存器写（ES8311，Wire 总线） ---------------- */
static bool es_wr(uint8_t reg, uint8_t val)
{
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

/* 调试用：读 ES8311 寄存器（返回 -1 表示无应答/I2C 失败） */
static int es_rd(uint8_t reg)
{
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return -1;
    if (Wire.requestFrom((int)ES8311_ADDR, 1, true) != 1) return -1;
    return Wire.read();
}

/* ---------------- ES8311 完整初始化序列 ----------------
 * 忠实移植官方例程 esp_codec_dev 的 es8311_open + es8311_start + es8311_set_fs，
 * 针对本板：从模式(slave)、使用外部 MCLK(use_mclk)、16bit、标准 I2S、16kHz。
 */
static void es8311_init(void)
{
    /* ---- es8311_open ---- */
    es_wr(R44, 0x08);            // 增强 I2C 抗噪（写两次，规避首字节偶发失败）
    es_wr(R44, 0x08);
    es_wr(R01, 0x30);
    es_wr(R02, 0x00);
    es_wr(R03, 0x10);
    es_wr(R16, 0x24);
    es_wr(R04, 0x10);
    es_wr(R05, 0x00);
    es_wr(R0B, 0x00);
    es_wr(R0C, 0x00);
    es_wr(R10, 0x1F);
    es_wr(R11, 0x7F);
    es_wr(R00, 0x80);            // 退出复位（从模式：清 bit6）
    es_wr(R00, 0x80);
    es_wr(R01, 0x3F);            // 选择 MCLK 为时钟源（use_mclk：清 bit7）
    es_wr(R06, 0x00);            // SCLK 不反相
    es_wr(R13, 0x10);
    es_wr(R1B, 0x0A);
    es_wr(R1C, 0x6A);
    es_wr(R44, 0x58);            // 内部参考信号（dac 参考）

    /* ---- es8311_start（DAC 工作模式）---- */
    es_wr(R00, 0x80);
    es_wr(R01, 0x3F);
    es_wr(R09, 0x00);            // DAC 数字口：I2S 标准、16bit（bit[1:0]=00, bit[5:4]=00）
    es_wr(R0A, 0x40);            // ADC 数字口：bit6 置位（ADC 路径在此板未用）
    es_wr(R17, 0xBF);
    es_wr(R0E, 0x02);
    es_wr(R12, 0x00);
    es_wr(R14, 0x1A);
    es_wr(R14, 0x1A);
    es_wr(R0D, 0x01);
    es_wr(R15, 0x40);
    es_wr(R37, 0x08);
    es_wr(R45, 0x00);

    /* ---- es8311_set_fs(16000, 16bit, I2S) ---- */
    // 16bit
    es_wr(R09, 0x0C);            // DAC 口 bit[3:2]=11 -> 16bit
    es_wr(R0A, 0x4C);            // ADC 口 16bit（bit[3:2]=11）且保留 bit6
    // I2S 标准格式（清 bit[1:0]=00）
    es_wr(R09, 0x0C);
    es_wr(R0A, 0x4C);
    // 采样率系数 mclk=4096000 rate=16000 -> pre_div=1 pre_multi=1 adc_div=1 dac_div=1
    es_wr(R02, 0x00);
    es_wr(R05, 0x00);
    es_wr(R03, 0x10);            // fs_mode=0, adc_osr=0x10
    es_wr(R04, 0x20);            // dac_osr=0x20
    es_wr(R07, 0x00);            // lrck_h
    es_wr(R08, 0xFF);            // lrck_l
    es_wr(R06, 0x03);            // bclk_div=4 -> (4-1)=3

    /* 音量（0xA0 ≈ -7.5dB，清晰不炸；吉他清音可适当推高至 0xB0） */
    es_wr(R32, 0xA8);
}

/* ============================================================
 *  Karplus-Strong 合成：单音拨弦 → 清音电吉他分解和弦
 *
 *  算法：
 *    1) 用低通滤波后的白噪声初始化延迟线（模拟拨弦瞬态）
 *    2) 循环反馈：每采样输出 = 延迟线头部值；
 *       尾部写入 = (a+b)*0.5*decay（一阶低通 + 衰减）
 *    3) 结果：自然衰减的拨弦音色，类似 Fender Strat 清音
 *
 *  参数：
 *    freq   - 基频 Hz
 *    buf    - 输出单声道浮点缓冲（frames 长）
 *    offset - 该音在输出中的起始偏移（>0 则前面静音）
 *    ring   - 该音的延音长度（≤ frames-offset）
 * ============================================================ */

// 确定性伪随机（避免依赖 srand）
static uint32_t g_lcg = 0x12345678u;
static inline float lcg_noise(void)
{
    g_lcg = g_lcg * 1103515245u + 12345u;
    return ((int32_t)(g_lcg >> 16) / 32768.0f) - 0.5f;
}

static void ks_pluck(float freq, float *buf, size_t frames, size_t offset, size_t ring)
{
    if (freq < 60.0f || offset >= frames || ring == 0) return;

    size_t N = (size_t)((float)SAMPLE_RATE / freq + 0.5f);  // 延迟线长度
    if (N < 12) N = 12; if (N > 2048) N = 2048;             // 安全钳位

    float *dl = (float *)malloc(N * sizeof(float));
    if (!dl) return;

    // 初始化：白噪声经简单低通（模拟软拨弦，非硬击打）
    for (size_t i = 0; i < N; i++) {
        dl[i] = (lcg_noise() + (i > 0 ? dl[i-1]*0.35f : 0.0f)) * 0.65f;
    }

    const float decay = 0.996f;         // 衰减系数（~0.64s 到 -60dB）

    size_t pos = 0;
    for (size_t i = 0; i < frames; i++) {
        if (i < offset || i >= offset + ring) {
            buf[i] = 0.0f;
            continue;
        }
        size_t ri = i - offset;          // 相对该音起点的相对位置

        // 包络：快速起音(8ms) + 指数衰减
        float env = 1.0f;
        if (ri < 128) {
            env = (float)ri / 128.0f;   // 线性起音
        } else {
            float t = (float)(ri - 128) / (float)SAMPLE_RATE;
            env = expf(-t / 0.42f);     // τ≈420ms 的指数衰减
        }

        // Karplus-Strong 环形延迟线：避免逐采样 memmove，扩充和弦库后仍能快速启动
        float s = dl[pos];
        float tail = dl[(pos + 1) % N];
        dl[pos] = (s + tail) * 0.5f * decay;
        pos = (pos + 1) % N;

        buf[i] = s * env;
    }

    free(dl);
}

/* 合成一段完整和弦琶音（立体声 int16，PSRAM 分配） */
static void synth_chord(int idx, int16_t *buf, size_t frames)
{
    if (idx < 0 || idx >= CHORD_DATA_COUNT) return;

    const float SR = (float)SAMPLE_RATE;
    const ChordData &chord = chord_data_get((uint8_t)idx);
    const float note_gain = 0.28f;       // 每音增益（5 音叠加后峰值约 0.85，留余量防削波）

    // 浮点工作缓冲（复用，逐音合成后混入输出）
    float *tmp = (float *)heap_caps_malloc(frames * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!tmp) { memset(buf, 0, frames * 2 * sizeof(int16_t)); return; }

    // 先清零输出
    memset(buf, 0, frames * 2 * sizeof(int16_t));

    for (int n = 0; n < CHORD_AUDIO_NOTE_COUNT; n++) {
        float freq = chord.audio_hz[n];
        size_t start = (size_t)n * NOTE_GAP;   // 第 n 音起始偏移
        if (start >= frames) break;

        ks_pluck(freq, tmp, frames, start,
                 (start + NOTE_RING <= frames) ? NOTE_RING : (frames - start));

        // 混入立体声输出（L=R，单声道源）
        for (size_t i = start; i < frames && i < start + NOTE_RING; i++) {
            float s = tmp[i] * note_gain;
            // 软限幅（soft clip），避免硬削波产生刺耳高频
            if (s > 1.0f)  s = 1.0f - expf(-(s - 1.0f));
            else if (s < -1.0f) s = -1.0f + expf(-((-s) - 1.0f));
            int16_t v = (int16_t)(s * 32000.0f);   // 略低于满量程，留动态余量
            buf[i * 2]     += v;   // L
            buf[i * 2 + 1] += v;   // R
        }
    }

    // 尾部淡出（最后 200 帧 ≈12ms，消除突然截断的 click）
    size_t fade_start = (frames > 200) ? frames - 200 : 0;
    for (size_t i = fade_start; i < frames; i++) {
        float k = (float)(frames - 1 - i) / 200.0f;
        buf[i * 2]     = (int16_t)(buf[i * 2]     * k);
        buf[i * 2 + 1] = (int16_t)(buf[i * 2 + 1] * k);
    }

    free(tmp);
}

/* ---------------- Beep 合成（会议提醒，保留不变） ---------------- */
static void synth_beep(int16_t *buf, size_t frames)
{
    const float SR = (float)SAMPLE_RATE;
    const int beepLen = 1600;   // 0.10s
    const int gap     = 1280;   // 0.08s
    const int total   = beepLen * 2 + gap;
    (void)frames;
    for (int i = 0; i < total; i++) {
        float s = 0.0f;
        for (int b = 0; b < 2; b++) {
            int start = b * (beepLen + gap);
            int rel = i - start;
            if (rel >= 0 && rel < beepLen) {
                float t = (float)rel / SR;
                float env = sinf(PI * (float)rel / (float)beepLen);  // 半正弦平滑起落
                s += sinf(2.0f * PI * 1046.0f * t) * env * 0.55f;
            }
        }
        if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
        int16_t v = (int16_t)(s * 32767 * 0.9f);
        buf[i * 2]     = v;
        buf[i * 2 + 1] = v;
    }
}

/* ---------------- 播放任务（非阻塞，支持连按打断） ---------------- */
typedef struct { const int16_t *data; size_t frames; } audio_req_t;

static void audio_task(void *arg)
{
    (void)arg;
    audio_req_t req;
    for (;;) {
        if (xQueueReceive(g_audio_q, &req, portMAX_DELAY) == pdTRUE) {
            size_t total = req.frames * 4;   // 字节数（立体声 int16）
            size_t written = 0;
            size_t chunk = 256 * 4;         // 每块 256 帧（16ms），分块写入以支持打断

            /* 分块写入，但每块用 portMAX_DELAY 阻塞至写完（w 必等于 blk）。
             * 绝不能用短超时（50ms）：当 DMA 缓冲满时 i2s_write 会返回 w=0，
             * 导致 written 停滞、while 死循环忙等，永久占用 core1 并阻塞雷达任务。 */
            while (written < total) {
                size_t remain = total - written;
                size_t blk = (remain < chunk) ? remain : chunk;
                size_t w = 0;
                i2s_write(I2S_PORT,
                          ((uint8_t *)req.data) + written, blk, &w, portMAX_DELAY);
                written += w;

                // 检查是否有新请求入队（连按打断当前播放）
                if (written < total && uxQueueMessagesWaiting(g_audio_q) > 0) {
                    i2s_zero_dma_buffer(I2S_PORT);
                    break;   // 退出当前播放，立即处理下一个请求
                }
            }
            i2s_zero_dma_buffer(I2S_PORT);
        }
    }
}

/* ---------------- 公开接口 ---------------- */
void audio_init(void)
{
    // 1) ES8311 I²C 配置（Wire 已由 I2cMasterBus 在 SDA=13/SCL=14 上 begin）
    es8311_init();

    // 2) I²S 安装（标准 I2S，主模式，仅 TX）
    i2s_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode                = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate         = SAMPLE_RATE;
    cfg.bits_per_sample     = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format      = I2S_CHANNEL_FMT_RIGHT_LEFT;          // 立体声
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags    = ESP_INTR_FLAG_LEVEL2 | ESP_INTR_FLAG_IRAM;
    cfg.dma_buf_count       = 4;
    cfg.dma_buf_len         = 256;
    cfg.use_apll            = true;
    cfg.tx_desc_auto_clear  = true;
    cfg.fixed_mclk          = 4096000;                          // 强制精确 MCLK=4.096MHz（APLL）
    cfg.mclk_multiple       = I2S_MCLK_MULTIPLE_256;            // fixed_mclk>0 时此值被忽略
    cfg.bits_per_chan       = I2S_BITS_PER_CHAN_16BIT;         // 必须合法枚举

    esp_err_t e = i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
    if (e != ESP_OK) {
        Serial.printf("[audio] i2s_driver_install fail: 0x%x\n", e);
        return;
    }

    i2s_pin_config_t pins = {
        .mck_io_num  = I2S_MCK,
        .bck_io_num  = I2S_BCK,
        .ws_io_num   = I2S_WS,
        .data_out_num = I2S_DO,
        .data_in_num  = I2S_DI,
    };
    i2s_set_pin(I2S_PORT, &pins);
    // 注意：legacy 驱动里 i2s_set_clk 会重配时钟并丢掉已使能的 MCK 脚 → 导致 MCLK 关闭、codec 无主时钟静音。
    // install 已按 16k/16bit/立体声配好时钟，此处不再调用 i2s_set_clk。
    i2s_start(I2S_PORT);

    // 3) 功放使能（GPIO46 拉高）
    pinMode(PA_PIN, OUTPUT);
    digitalWrite(PA_PIN, HIGH);

    // 4) 合成音效缓冲（全部分配于 PSRAM）
    Serial.println("[audio] synthesizing guitar chords...");
    unsigned long t0 = millis();
    for (int c = 0; c < CHORD_DATA_COUNT; c++) {
        g_chord[c] = (int16_t *)heap_caps_malloc(CHORD_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (g_chord[c]) {
            synth_chord(c, g_chord[c], CHORD_FRAMES);
            Serial.printf("  chord %s OK (%d KB)\n", chord_data_get(c).name,
                          (int)(CHORD_FRAMES * 4 / 1024));
        } else {
            Serial.printf("  chord %d alloc FAIL\n", c);
        }
    }
    g_beep = (int16_t *)heap_caps_malloc(BEEP_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (g_beep) synth_beep(g_beep, BEEP_FRAMES);
    Serial.printf("[audio] synth done in %lu ms\n", millis() - t0);

    // 5) 播放队列 + 任务
    g_audio_q = xQueueCreate(4, sizeof(audio_req_t));
    if (g_audio_q) {
        xTaskCreatePinnedToCore(audio_task, "audio", 4 * 1024, NULL, 3, NULL, 1);
        g_audio_ready = true;
        Serial.println("[audio] ES8311 + I2S ready (guitar mode)");
    }
}

void audio_play_chord(uint8_t idx)
{
    if (!g_audio_ready || idx >= CHORD_DATA_COUNT || !g_chord[idx]) return;
    audio_req_t r = { g_chord[idx], CHORD_FRAMES };
    xQueueSend(g_audio_q, &r, 0);
}

uint8_t audio_chord_count(void)
{
    return CHORD_DATA_COUNT;
}

void audio_play_beep(void)
{
    if (!g_audio_ready || !g_beep) return;
    audio_req_t r = { g_beep, BEEP_FRAMES };
    xQueueSend(g_audio_q, &r, 0);
}

void audio_set_volume(uint8_t reg)
{
    es_wr(R32, reg);
}

/* ---------------- 调试辅助（仅供临时音频测试固件使用） ---------------- */
bool     audio_diag_ready(void)        { return g_audio_ready; }
int      audio_diag_write_result(void) { return g_last_wr; }
int      audio_diag_reg_read(uint8_t reg) { return es_rd(reg); }
