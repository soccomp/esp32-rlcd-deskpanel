#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include "display_bsp.h"

DisplayPort::DisplayPort(int mosi, int scl, int dc, int cs, int rst, int width, int height, spi_host_device_t spihost)
  : mosi_(mosi),
    scl_(scl),
    dc_(dc),
    cs_(cs),
    rst_(rst),
    width_(width),
    height_(height) {
  esp_err_t ret;
  spi_bus_config_t buscfg = {};
  int transfer = width_ * height_;
  buscfg.miso_io_num = -1;
  buscfg.mosi_io_num = mosi;
  buscfg.sclk_io_num = scl;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;
  buscfg.max_transfer_sz = transfer;
  ret = spi_bus_initialize(spihost, &buscfg, SPI_DMA_CH_AUTO);
  ESP_ERROR_CHECK(ret);

  esp_lcd_panel_io_spi_config_t io_config = {};
  io_config.dc_gpio_num = dc_;
  io_config.cs_gpio_num = cs_;
  io_config.pclk_hz = 10 * 1000 * 1000;
  io_config.lcd_cmd_bits = 8;
  io_config.lcd_param_bits = 8;
  io_config.spi_mode = 0;
  /* 8-31：10 -> 16。每帧要发 8 个事务（3 组窗口命令 + 2 字节参数 + 15000B 帧数据），
   * 相机取帧与全屏刷新叠加时队列会瞬时排队，深度留足余量降低溢出概率。 */
  io_config.trans_queue_depth = 16;

  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)spihost, &io_config, &io_handle));

  gpio_config_t gpio_conf = {};
  gpio_conf.intr_type = GPIO_INTR_DISABLE;
  gpio_conf.mode = GPIO_MODE_OUTPUT;
  gpio_conf.pin_bit_mask = (0x1ULL << rst_);
  gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));

  Set_ResetIOLevel(1);

  DisplayLen = transfer >> 3;  //(1byte 8ipex)
  DispBuffer = (uint8_t *)heap_caps_malloc(DisplayLen, MALLOC_CAP_SPIRAM);
  assert(DispBuffer);

#if (AlgorithmOptimization == 3)
  PixelIndexLUT = (uint16_t(*)[300])heap_caps_malloc(transfer * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  PixelBitLUT = (uint8_t(*)[300])heap_caps_malloc(transfer * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
  assert(PixelIndexLUT);
  assert(PixelBitLUT);
  if (width_ == 400) {
    InitLandscapeLUT();
  } else {
    InitPortraitLUT();
  }
#endif
}

DisplayPort::~DisplayPort() {
}

void DisplayPort::RLCD_Init() {
  RLCD_Reset();

  RLCD_SendCommand(0xD6);  // NVM Load Control
  RLCD_SendData(0x17);
  RLCD_SendData(0x02);

  RLCD_SendCommand(0xD1);  //Booster Enable
  RLCD_SendData(0x01);

  RLCD_SendCommand(0xC0);  //Gate Voltage Control
  RLCD_SendData(0x11);
  RLCD_SendData(0x04);

  RLCD_SendCommand(0xC1);  //VSHP Setting
  RLCD_SendData(0x69);
  RLCD_SendData(0x69);
  RLCD_SendData(0x69);
  RLCD_SendData(0x69);

  RLCD_SendCommand(0xC2);
  RLCD_SendData(0x19);
  RLCD_SendData(0x19);
  RLCD_SendData(0x19);
  RLCD_SendData(0x19);

  RLCD_SendCommand(0xC4);
  RLCD_SendData(0x4B);
  RLCD_SendData(0x4B);
  RLCD_SendData(0x4B);
  RLCD_SendData(0x4B);

  RLCD_SendCommand(0xC5);
  RLCD_SendData(0x19);
  RLCD_SendData(0x19);
  RLCD_SendData(0x19);
  RLCD_SendData(0x19);

  RLCD_SendCommand(0xD8);
  RLCD_SendData(0x80);
  RLCD_SendData(0xE9);

  RLCD_SendCommand(0xB2);
  RLCD_SendData(0x02);

  RLCD_SendCommand(0xB3);
  RLCD_SendData(0xE5);
  RLCD_SendData(0xF6);
  RLCD_SendData(0x05);
  RLCD_SendData(0x46);
  RLCD_SendData(0x77);
  RLCD_SendData(0x77);
  RLCD_SendData(0x77);
  RLCD_SendData(0x77);
  RLCD_SendData(0x76);
  RLCD_SendData(0x45);

  RLCD_SendCommand(0xB4);
  RLCD_SendData(0x05);
  RLCD_SendData(0x46);
  RLCD_SendData(0x77);
  RLCD_SendData(0x77);
  RLCD_SendData(0x77);
  RLCD_SendData(0x77);
  RLCD_SendData(0x76);
  RLCD_SendData(0x45);

  RLCD_SendCommand(0x62);
  RLCD_SendData(0x32);
  RLCD_SendData(0x03);
  RLCD_SendData(0x1F);

  RLCD_SendCommand(0xB7);
  RLCD_SendData(0x13);

  RLCD_SendCommand(0xB0);
  RLCD_SendData(0x64);

  RLCD_SendCommand(0x11);
  vTaskDelay(pdMS_TO_TICKS(200));
  RLCD_SendCommand(0xC9);
  RLCD_SendData(0x00);

  RLCD_SendCommand(0x36);
  RLCD_SendData(0x48);

  RLCD_SendCommand(0x3A);
  RLCD_SendData(0x11);

  RLCD_SendCommand(0xB9);
  RLCD_SendData(0x20);

  RLCD_SendCommand(0xB8);
  RLCD_SendData(0x29);

  RLCD_SendCommand(0x21);

  RLCD_SendCommand(0x2A);
  RLCD_SendData(0x12);
  RLCD_SendData(0x2A);

  RLCD_SendCommand(0x2B);
  RLCD_SendData(0x00);
  RLCD_SendData(0xC7);

  RLCD_SendCommand(0x35);
  RLCD_SendData(0x00);

  RLCD_SendCommand(0xD0);
  RLCD_SendData(0xFF);

  RLCD_SendCommand(0x38);
  RLCD_SendCommand(0x29);

  RLCD_ColorClear(ColorWhite);
}

void DisplayPort::RLCD_ColorClear(uint8_t color) {
  memset(DispBuffer, color, DisplayLen);
}

bool DisplayPort::RLCD_Display() {
  RLCD_SendCommand(0x2A);  // Column Address Set
  RLCD_SendData(0x12);
  RLCD_SendData(0x2A);

  RLCD_SendCommand(0x2B);  // Page Address Set
  RLCD_SendData(0x00);
  RLCD_SendData(0xC7);

  RLCD_SendCommand(0x2c);  // Page Address Set

  return RLCD_Sendbuffera(DispBuffer, DisplayLen);
}

void DisplayPort::RLCD_Reset(void) {
  Set_ResetIOLevel(1);
  vTaskDelay(pdMS_TO_TICKS(50));
  Set_ResetIOLevel(0);
  vTaskDelay(pdMS_TO_TICKS(20));
  Set_ResetIOLevel(1);
  vTaskDelay(pdMS_TO_TICKS(50));
}

void DisplayPort::RLCD_SendCommand(uint8_t Reg) {
  esp_err_t err = esp_lcd_panel_io_tx_param(io_handle, Reg, NULL, 0);
  if (err != ESP_OK) {
    tx_fail_cnt++;
    uint32_t now = millis();
    if (now - last_tx_fail_ms > 5000) {
      last_tx_fail_ms = now;
      ESP_LOGE(TAG, "tx_param(cmd 0x%02X) fail x%u err=0x%x (%s) free=%u",
               Reg, (unsigned)tx_fail_cnt, err, esp_err_to_name(err),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      tx_fail_cnt = 0;
    }
  }
}

void DisplayPort::RLCD_SendData(uint8_t Data) {
  esp_err_t err = esp_lcd_panel_io_tx_param(io_handle, -1, &Data, 1);
  if (err != ESP_OK) {
    tx_fail_cnt++;
    uint32_t now = millis();
    if (now - last_tx_fail_ms > 5000) {
      last_tx_fail_ms = now;
      ESP_LOGE(TAG, "tx_param(data 0x%02X) fail x%u err=0x%x (%s) free=%u",
               Data, (unsigned)tx_fail_cnt, err, esp_err_to_name(err),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      tx_fail_cnt = 0;
    }
  }
}

/* 8-31 关键修复：原实现是 ESP_ERROR_CHECK(...)，一旦 esp_lcd_panel_io_tx_color
 * 返回 ESP_ERR_NO_MEM(0x101)（SPI 事务队列瞬时排满 / 临时 DMA 缓冲区分配失败）
 * 就会 abort() -> 整机重启。实测与"切页次数"无关：有一次连续正常运行 361 秒
 * 后才触发，另有几次上电后 28 秒即触发，说明是 SPI 背压的偶发事件，不是逻辑错误。
 * 对策：退避重试，彻底失败则丢弃这一帧（反射屏保留上一帧内容），绝不重启。 */
bool DisplayPort::RLCD_Sendbuffera(uint8_t *Data, int len) {
  for (int attempt = 0; attempt < 4; ++attempt) {
    esp_err_t err = esp_lcd_panel_io_tx_color(io_handle, -1, Data, len);
    if (err == ESP_OK) return true;
    if (attempt == 3) {
      tx_fail_cnt++;
      uint32_t now = millis();
      if (now - last_tx_fail_ms > 5000) {
        last_tx_fail_ms = now;
        ESP_LOGE(TAG, "tx_color DROPPED x%u err=0x%x (%s) len=%d free=%u largest_dma=%u",
                 (unsigned)tx_fail_cnt, err, esp_err_to_name(err), len,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        tx_fail_cnt = 0;
      }
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(2 + attempt * 3));   // 2ms / 5ms / 8ms 退避
  }
  return false;
}

void DisplayPort::Set_ResetIOLevel(uint8_t level) {
  gpio_set_level((gpio_num_t)rst_, level ? 1 : 0);
}
#if (AlgorithmOptimization != 3)

void DisplayPort::RLCD_SetPortraitPixel(uint16_t x, uint16_t y, uint8_t color) {
  if ((x >= width_) || (y >= height_)) {
    ESP_LOGE("Pixel", "Beyond the limit : (%d,%d)", x, y);
    return;
  }
#if (AlgorithmOptimization == 2)
  const uint16_t W4 = width_ >> 2;

  uint16_t byte_x = x >> 2;
  uint16_t byte_y = y >> 1;

  uint32_t index = byte_y * W4 + byte_x;

  uint8_t local_x = x & 0x03;
  uint8_t local_y = y & 0x01;

  uint8_t bit = 7 - ((local_x << 1) | local_y);

  uint8_t mask = 1 << bit;

  if (color)
    DispBuffer[index] |= mask;
  else
    DispBuffer[index] &= ~mask;
#else
  uint16_t byte_x = x / 4;
  uint16_t byte_y = y / 2;

  uint32_t index = byte_y * (width_ / 4) + byte_x;

  uint8_t local_x = x % 4;
  uint8_t local_y = y % 2;
  uint8_t bit = 7 - (local_x * 2 + local_y);
  if (color)
    DispBuffer[index] |= (1 << bit);
  else
    DispBuffer[index] &= ~(1 << bit);
#endif
}

void DisplayPort::RLCD_SetLandscapePixel(uint16_t x, uint16_t y, uint8_t color) {
  if (x >= width_ || y >= height_)
    return;
#if (AlgorithmOptimization == 2)

  uint16_t inv_y = (height_ - 1 - y);
  const uint16_t H4 = height_ >> 2;
  uint16_t byte_x = x >> 1;
  uint16_t block_y = inv_y >> 2;
  uint32_t index = byte_x * H4 + block_y;
  uint8_t local_x = x & 0x01;
  uint8_t local_y = inv_y & 0x03;
  uint8_t bit = 7 - ((local_y << 1) | local_x);
  uint8_t mask = 1 << bit;
  if (color)
    DispBuffer[index] |= mask;
  else
    DispBuffer[index] &= ~mask;
#else
  uint16_t inv_y = height_ - 1 - y;

  uint16_t byte_x = x / 2;       // 0..199
  uint16_t block_y = inv_y / 4;  // 0..74

  uint32_t index = byte_x * (height_ / 4) + block_y;

  uint8_t local_x = x % 2;      // 0 or 1
  uint8_t local_y = inv_y % 4;  // 0..3

  uint8_t bit = 7 - (local_y * 2 + local_x);

  if (color)
    DispBuffer[index] |= (1 << bit);
  else
    DispBuffer[index] &= ~(1 << bit);
#endif
}

#endif


#if (AlgorithmOptimization == 3)

void DisplayPort::InitPortraitLUT() {
  uint16_t W4 = width_ >> 2;
  for (uint16_t y = 0; y < height_; y++) {
    uint16_t byte_y = y >> 1;
    uint8_t local_y = y & 1;

    for (uint16_t x = 0; x < width_; x++) {
      uint16_t byte_x = x >> 2;
      uint8_t local_x = x & 3;

      uint32_t index = byte_y * W4 + byte_x;
      uint8_t bit = 7 - ((local_x << 1) | local_y);

      PixelIndexLUT[x][y] = index;
      PixelBitLUT[x][y] = (1 << bit);
    }
  }
}

void DisplayPort::InitLandscapeLUT() {
  uint16_t H4 = height_ >> 2;

  for (uint16_t y = 0; y < height_; y++) {
    uint16_t inv_y = height_ - 1 - y;
    uint16_t block_y = inv_y >> 2;
    uint8_t local_y = inv_y & 3;

    for (uint16_t x = 0; x < width_; x++) {
      uint16_t byte_x = x >> 1;
      uint8_t local_x = x & 1;

      uint32_t index = byte_x * H4 + block_y;
      uint8_t bit = 7 - ((local_y << 1) | local_x);

      PixelIndexLUT[x][y] = index;
      PixelBitLUT[x][y] = (1 << bit);
    }
  }
}

void DisplayPort::RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color) {
  /* 8-13 审核修复：查表法版本原无边界检查。LVGL full_refresh 异常时可能给出
   * 越界 area（x>=width_ 或 y>=height_），会读 LUT 越界得垃圾 idx 再写
   * DispBuffer 越界 -> heap corruption -> 后续 LVGL 刷新 LoadProhibited 崩溃。
   * 与 SetPortraitPixel/SetLandscapePixel 版本保持一致：越界直接忽略。 */
  if (x >= width_ || y >= height_) return;
  uint32_t idx = PixelIndexLUT[x][y];
  uint8_t mask = PixelBitLUT[x][y];

  uint8_t *p = &DispBuffer[idx];

  if (color)
    *p |= mask;
  else
    *p &= ~mask;
}

#endif
