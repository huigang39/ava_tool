#ifndef JLINK_PORT_H
#define JLINK_PORT_H
#include <stddef.h>
#include <stdint.h>

#include "errdef.h"
#include "macrodef.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief               初始化并连接 J-Link
 * @param[in] dll_path  J-Link DLL 路径传 NULL 则在默认路径中查找
 * @param[in] dev       目标芯片名称 (例如 "STM32H745II")
 * @param[in] vel_khz   SWD 通信速率 (传 0 默认使用 4000)
 * @param[in] serial_no J-Link 序列号
 * @param[in] use_sn    是否根据序列号连接特定的 J-Link 仿真器
 * @return              int      0 成功,<0 失败
 */
int jlink_port_init(const char *dll_path,
                    const char *dev,
                    uint32_t vel_khz,
                    uint32_t serial_no,
                    bool use_sn);

/**
 * @brief  断开 J-Link 连接,释放 DLL 资源
 * @return 操作结果, 失败时返回对应错误码
 */
int jlink_port_deinit(void);

/**
 * @brief  重置 J-Link 端口
 * @return 操作结果, 失败时返回对应错误码
 */
int jlink_port_reset(void);

/**
 * @brief            向目标内存写入数据
 * @param[in] addr   目标起始地址
 * @param[in] length 写入字节数
 * @param[in] data   待写入的数据指针
 * @return           int 返回实际写入的字节数,<0 表示失败
 * @param[in] len    数组元素数量或缓冲区长度
 */
int jlink_port_write_mem(uint32_t addr, uint32_t len, const void *data);

/**
 * @brief             从目标内存读取数据 (预留扩展,方便以后读取数据)
 * @param[in]  addr   目标起始地址
 * @param[in]  length 读取字节数
 * @param[out] data   接收数据的缓冲区指针
 * @return            int 返回实际读取的字节数,<0 表示失败
 * @param[in]  len    数组元素数量或缓冲区长度
 */
int jlink_port_read_mem(uint32_t addr, uint32_t len, void *data);

#ifdef __cplusplus
}
#endif

#endif
