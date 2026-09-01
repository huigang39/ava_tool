#ifndef CRC_H
#define CRC_H

#include <stddef.h>
#include <stdint.h>

#include "macrodef.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief          计算 CRC8 校验值
 * @param[in] data 待校验数据
 * @param[in] size 数据长度, 单位: byte
 * @return         CRC8 校验值
 */
uint8_t crc8(const void *data, size_t size);

/**
 * @brief          计算 Modbus CRC16 校验值
 * @param[in] data 待校验数据
 * @param[in] size 数据长度, 单位: byte
 * @return         CRC16 校验值
 */
uint16_t crc16_modbus(const void *data, size_t size);

/**
 * @brief          CRC32 计算
 * @param[in] data 待计算的数据
 * @param[in] size 待计算的字节数
 * @return         uint32_t 32bit CRC 值
 */
uint32_t crc32(const void *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif
