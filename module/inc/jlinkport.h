#ifndef JLINK_PORT_H
#define JLINK_PORT_H

#include "typedef.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化并连接 J-Link
 * @param dll_path  J-Link DLL 路径。传 NULL 则在默认路径中查找。
 * @param device    目标芯片名称 (例如 "STM32H745II")
 * @param speed_khz SWD 通信速率 (传 0 默认使用 4000)
 * @param serial_no J-Link 序列号
 * @param use_sn    是否根据序列号连接特定的 J-Link 仿真器
 * @return int      0 成功，<0 失败
 */
int jlink_port_init(const char *dll_path, const char *device, u32 speed_khz, u32 serial_no, bool use_sn);

/**
 * @brief 断开 J-Link 连接，释放 DLL 资源
 */
void jlink_port_deinit(void);

int jlink_port_reset(void);

/**
 * @brief 向目标内存写入数据
 * @param addr 目标起始地址
 * @param len  写入字节数
 * @param data 待写入的数据指针
 * @return int 返回实际写入的字节数，<0 表示失败
 */
int jlink_port_write_mem(u32 addr, u32 len, const void *data);

/**
 * @brief 从目标内存读取数据 (预留扩展，方便以后读取数据)
 * @param addr 目标起始地址
 * @param len  读取字节数
 * @param data 接收数据的缓冲区指针
 * @return int 返回实际读取的字节数，<0 表示失败
 */
int jlink_port_read_mem(u32 addr, u32 len, void *data);

#ifdef __cplusplus
}
#endif

#endif // !JLINK_PORT_H
