#include <Arduino.h>
#include "i2c_bsp.h"

I2cMasterBus::I2cMasterBus(int scl_pin, int sda_pin, int i2c_port)
{
    port_ = (uint8_t)i2c_port;
    wire_ = (port_ == 1) ? &Wire1 : &Wire;
    wire_->begin(sda_pin, scl_pin);
    wire_->setClock(400000);   // SHTC3 支持到 400kHz
}

I2cMasterBus::~I2cMasterBus()
{
}

// 发送数据：reg == -1 时无寄存器前缀，直接写 buf
int I2cMasterBus::i2c_write_buff(uint8_t dev_addr, int reg, uint8_t *buf, uint8_t len)
{
    wire_->beginTransmission(dev_addr);
    if (reg != -1) {
        wire_->write((uint8_t)reg);
    }
    for (uint8_t i = 0; i < len; i++) {
        wire_->write(buf[i]);
    }
    return (wire_->endTransmission() == 0) ? 0 : -1;
}

// 先写 writeLen 字节，再读 readLen 字节（repeated-start）
int I2cMasterBus::i2c_master_write_read_dev(uint8_t dev_addr, uint8_t *writeBuf,
                                            uint8_t writeLen, uint8_t *readBuf, uint8_t readLen)
{
    wire_->beginTransmission(dev_addr);
    for (uint8_t i = 0; i < writeLen; i++) {
        wire_->write(writeBuf[i]);
    }
    if (wire_->endTransmission(false) != 0) {   // false => repeated-start，保持总线
        return -1;
    }
    uint8_t got = wire_->requestFrom((int)dev_addr, (int)readLen, (int)true);
    if (got != readLen) {
        return -1;
    }
    for (uint8_t i = 0; i < readLen; i++) {
        readBuf[i] = wire_->read();
    }
    return 0;
}

// 读数据：reg == -1 时直接读 len 字节
int I2cMasterBus::i2c_read_buff(uint8_t dev_addr, int reg, uint8_t *buf, uint8_t len)
{
    if (reg != -1) {
        wire_->beginTransmission(dev_addr);
        wire_->write((uint8_t)reg);
        if (wire_->endTransmission(false) != 0) {
            return -1;
        }
    }
    uint8_t got = wire_->requestFrom((int)dev_addr, (int)len, (int)true);
    if (got != len) {
        return -1;
    }
    for (uint8_t i = 0; i < len; i++) {
        buf[i] = wire_->read();
    }
    return 0;
}
