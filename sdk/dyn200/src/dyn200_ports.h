/*
 * dyn200_ports.h — 串口枚举的内部接口，只给 dyn200.c 使用。
 *
 * 单独一个编译单元：只用扁平 API 的静态库使用者不会拉进这段代码，因此也就
 * 不需要在链接时加上 Windows 的 SetupAPI（见 README“构建”一节）。
 */
#ifndef DYN200_PORTS_H
#define DYN200_PORTS_H

#include <stdint.h>

#define DYN200_PORT_NAME_MAX 64
#define DYN200_PORT_DESC_MAX 160
#define DYN200_PORT_LIST_MAX 32

/**
 * @brief 枚举本机串口，最多写 capacity 条
 *        names[i] 例如 "COM7"、"/dev/ttyUSB0"；descs[i] 是设备描述（可能为空串）
 *        返回实际条数。只写调用方提供的缓冲，可重入
 */
uint32_t dyn200_ports_enumerate(char (*names)[DYN200_PORT_NAME_MAX],
                                char (*descs)[DYN200_PORT_DESC_MAX],
                                uint32_t capacity);

#endif // DYN200_PORTS_H
