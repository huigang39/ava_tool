#ifndef CRC_H
#define CRC_H

#include "typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                  接口声明                                  */
/* -------------------------------------------------------------------------- */

u8 crc8(const void *data, usize size);

/**
 * @brief CRC32 计算
 *
 * @param data 待计算的数据
 * @param size 待计算的字节数
 * @return     u32 32bit CRC 值
 */
u32 crc32(const void *data, usize size);

#ifdef __cplusplus
}
#endif

#endif // !CRC_H
