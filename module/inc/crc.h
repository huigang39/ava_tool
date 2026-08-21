#ifndef CRC_H
#define CRC_H
#include <stddef.h>
#include <stdint.h>

#include "macrodef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

uint8_t crc8(const void *data, size_t size);

/**
 * @brief Modbus CRC16 计算
 */
uint16_t crc16_modbus(const void *data, size_t size);

/**
 * @brief CRC32 计算
 *
 * @param data 待计算的数据
 * @param size 待计算的字节数
 * @return     uint32_t 32bit CRC 值
 */
uint32_t crc32(const void *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif // !CRC_H
