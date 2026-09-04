#pragma once

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>


#define AlgorithmOptimization 3  //1:原始算法 2:采用移位算法 3:查表法   来优化CPU

enum ColorSelection {
  ColorBlack = 0,
  ColorWhite = 0xff
};

class DisplayPort {
private:
  esp_lcd_panel_io_handle_t io_handle = NULL;
  uint32_t i2c_data_pdMS_TICKS = 0;
  uint32_t i2c_done_pdMS_TICKS = 0;
  const char *TAG = "Display";
  int mosi_;
  int scl_;
  int dc_;
  int cs_;
  int rst_;
  int width_;
  int height_;
  uint8_t *DispBuffer = NULL;
  int DisplayLen;
#if (AlgorithmOptimization == 3)
  uint16_t (*PixelIndexLUT)[300];
  uint8_t (*PixelBitLUT)[300];
  void InitPortraitLUT();
  void InitLandscapeLUT();
#endif

  void Set_ResetIOLevel(uint8_t level);
  void RLCD_SendCommand(uint8_t Reg);
  void RLCD_SendData(uint8_t Data);
  /* 8-31：改为返回成功与否。SPI 事务队列偶发背压时不再 abort（见 .cpp 注释） */
  bool RLCD_Sendbuffera(uint8_t *Data, int len);
  void RLCD_Reset(void);
  uint32_t tx_fail_cnt = 0;      // 累计发送失败次数（日志限流用）
  uint32_t last_tx_fail_ms = 0;  // 上次打印失败日志的时刻

public:
  DisplayPort(int mosi, int scl, int dc, int cs, int rst, int width, int height, spi_host_device_t spihost = SPI3_HOST);
  ~DisplayPort();
  void RLCD_Init();
  void RLCD_ColorClear(uint8_t color);
  bool RLCD_Display();   // 8-31：返回帧是否真正送出（false=本次刷新被丢弃）
#if (AlgorithmOptimization != 3)
  void RLCD_SetPortraitPixel(uint16_t x, uint16_t y, uint8_t color);   //竖屏显示
  void RLCD_SetLandscapePixel(uint16_t x, uint16_t y, uint8_t color);  //横屏显示
#endif
#if (AlgorithmOptimization == 3)
  void RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color);
#endif
};
