#include <Arduino.h>
#include <stdio.h>
#include <esp_log.h>
#include "shtc3.h"

Shtc3Port::Shtc3Port(I2cMasterBus &i2cbus)
  : i2cbus_(i2cbus) {
  Shtc3_Wakeup();
  Shtc3_SoftReset();
  delay(20);   // 20ms
  Shtc3_GetId();
  ESP_LOGI(TAG, "ID:%04x", shtc3_id);
}

Shtc3Port::~Shtc3Port() {
}

etError Shtc3Port::Shtc3_GetId() {
  uint8_t senBuf[2] = { (uint8_t)(READ_ID >> 8), (uint8_t)(READ_ID & 0xff) };
  uint8_t readBuf[3] = { 0, 0, 0 };
  int err = i2cbus_.i2c_master_write_read_dev(Shtc3Address, senBuf, 2, readBuf, 3);
  etError error = (err == 0) ? NO_ERROR : ACK_ERROR;
  if (error != NO_ERROR) {
    ESP_LOGE("shtc3", "GetId WRITE Failure");
    return error;
  }
  error = Shtc3_CheckCrc(readBuf, 2, readBuf[2]);
  if (error != NO_ERROR) {
    ESP_LOGE("shtc3", "GetId CRC Failure");
    return error;
  }
  shtc3_id = ((readBuf[0] << 8) | readBuf[1]);
  return error;
}

uint16_t Shtc3Port::Shtc3_GetShtc3Id() {
  return shtc3_id;
}

// wake up the sensor from sleep mode
etError Shtc3Port::Shtc3_Wakeup() {
  uint8_t senBuf[2] = { (uint8_t)(WAKEUP >> 8), (uint8_t)(WAKEUP & 0xff) };
  int err = i2cbus_.i2c_write_buff(Shtc3Address, -1, senBuf, 2);
  etError error = (err == 0) ? NO_ERROR : ACK_ERROR;
  delay(50);  // 50ms 唤醒稳定
  if (error != NO_ERROR)
    ESP_LOGE("shtc3", "Wakeup Failure");
  return error;
}

etError Shtc3Port::Shtc3_SoftReset() {
  uint8_t senBuf[2] = { (uint8_t)(SOFT_RESET >> 8), (uint8_t)(SOFT_RESET & 0xff) };
  int err = i2cbus_.i2c_write_buff(Shtc3Address, -1, senBuf, 2);
  etError error = (err == 0) ? NO_ERROR : ACK_ERROR;
  if (error != NO_ERROR)
    ESP_LOGE("shtc3", "SoftReset Failure");
  return error;
}

etError Shtc3Port::Shtc3_CheckCrc(uint8_t data[], uint8_t nbrOfBytes, uint8_t checksum) {
  uint8_t bit;         // bit mask
  uint8_t crc = 0xFF;  // calculated checksum
  uint8_t byteCtr;     // byte counter

  // calculates 8-Bit checksum with given polynomial
  for (byteCtr = 0; byteCtr < nbrOfBytes; byteCtr++) {
    crc ^= (data[byteCtr]);
    for (bit = 8; bit > 0; --bit) {
      if (crc & 0x80) {
        crc = (crc << 1) ^ CRC_POLYNOMIAL;
      } else {
        crc = (crc << 1);
      }
    }
  }

  // verify checksum
  if (crc != checksum) {
    return CHECKSUM_ERROR;
  } else {
    return NO_ERROR;
  }
}

float Shtc3Port::Shtc3_CalcTemperature(uint16_t rawValue) {
  // calculate temperature [°C]
  // T = -45 + 175 * rawValue / 2^16, 再减去补偿偏移
  return 175 * (float)rawValue / 65536.0f - 45.0f - SHTC3_PETP_VOL;
}

float Shtc3Port::Shtc3_CalcHumidity(uint16_t rawValue) {
  // calculate relative humidity [%RH]
  // RH = rawValue / 2^16 * 100
  return 100 * (float)rawValue / 65536.0f;
}

etError Shtc3Port::Shtc3_GetTempAndHumiPolling(float *temp, float *humi) {
  int err = 0;
  etError error;          // error code
  uint16_t rawValueTemp;  // temperature raw value from sensor
  uint16_t rawValueHumi;  // humidity raw value from sensor
  uint8_t bytes[6] = { 0 };
  uint8_t senBuf[2] = { (uint8_t)(MEAS_T_RH_POLLING >> 8), (uint8_t)(MEAS_T_RH_POLLING & 0xff) };
  err = i2cbus_.i2c_write_buff(Shtc3Address, -1, senBuf, 2);
  error = (err == 0) ? NO_ERROR : ACK_ERROR;
  if (error != NO_ERROR) {
    ESP_LOGE("shtc3", "GetTempAndHumi WRITE Failure");
    return error;
  }

  delay(20);  // 测量完成等待（normal mode ~12-14ms）

  // if no error, read temperature and humidity raw values
  err = i2cbus_.i2c_read_buff(Shtc3Address, -1, bytes, 6);
  error = (err == 0) ? NO_ERROR : ACK_ERROR;
  if (error != NO_ERROR) {
    ESP_LOGE("shtc3", "GetTempAndHumi READ Failure");
    return error;
  }
  error = Shtc3_CheckCrc(bytes, 2, bytes[2]);
  if (error != NO_ERROR) {
    ESP_LOGE("shtc3", "GetTempAndHumi TempCRC Failure");
    return error;
  }
  error = Shtc3_CheckCrc(&bytes[3], 2, bytes[5]);
  if (error != NO_ERROR) {
    ESP_LOGE("shtc3", "GetTempAndHumi humidityCRC Failure");
    return error;
  }
  // if no error, calculate temperature in °C and humidity in %RH
  rawValueTemp = (bytes[0] << 8) | bytes[1];
  rawValueHumi = (bytes[3] << 8) | bytes[4];
  *temp = Shtc3_CalcTemperature(rawValueTemp);
  *humi = Shtc3_CalcHumidity(rawValueHumi);
  return error;
}

etError Shtc3Port::Shtc3_Sleep() {
  uint8_t senBuf[2] = { (uint8_t)(SLEEP >> 8), (uint8_t)(SLEEP & 0xff) };
  int err = i2cbus_.i2c_write_buff(Shtc3Address, -1, senBuf, 2);
  etError error = (err == 0) ? NO_ERROR : ACK_ERROR;
  if (error != NO_ERROR)
    ESP_LOGE("shtc3", "Sleep Failure");
  return error;
}

// 返回 0=成功, 1=失败（与厂商示例一致）
uint8_t Shtc3Port::Shtc3_ReadTempHumi(float *t, float *h) {
  etError error;
  Shtc3_Wakeup();
  error = Shtc3_GetTempAndHumiPolling(t, h);
  if (error != NO_ERROR) {
    ESP_LOGW("shtc3", "error:%d", error);
    Shtc3_Sleep();
    return 1;
  }
  Shtc3_Sleep();
  return 0;
}
