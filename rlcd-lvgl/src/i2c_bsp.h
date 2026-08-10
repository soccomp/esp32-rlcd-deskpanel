#ifndef I2C_BSP_H
#define I2C_BSP_H

#include <Arduino.h>
#include <Wire.h>

// 轻量 I2C 总线封装：基于 Arduino Wire（TwoWire）。
// 原因：本机 arduino-espressif32 3.2.0 捆绑的 ESP-IDF 仅提供旧版 driver/i2c.h，
// 厂商示例使用的 i2c_master_bus_* 新 API（ESP-IDF 5.x）在此 SDK 中并不存在，
// 直接用 Wire 可跨版本稳定工作，且 SHTC3 协议足够简单。
class I2cMasterBus {
private:
    TwoWire *wire_;
    uint8_t  port_;        // 0 -> Wire, 1 -> Wire1

public:
    // scl_pin / sda_pin: 实际 GPIO；i2c_port: 0=Wire, 1=Wire1
    I2cMasterBus(int scl_pin, int sda_pin, int i2c_port = 0);
    ~I2cMasterBus();

    TwoWire *GetWire(void) { return wire_; }

    // 发送数据：reg == -1 时无寄存器前缀，直接写 buf
    int i2c_write_buff(uint8_t dev_addr, int reg, uint8_t *buf, uint8_t len);

    // 先写 writeLen 字节，再读 readLen 字节（repeated-start，不释放总线）
    int i2c_master_write_read_dev(uint8_t dev_addr, uint8_t *writeBuf,
                                  uint8_t writeLen, uint8_t *readBuf, uint8_t readLen);

    // 读数据：reg == -1 时直接读 len 字节
    int i2c_read_buff(uint8_t dev_addr, int reg, uint8_t *buf, uint8_t len);
};

#endif
