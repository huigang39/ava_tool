#ifndef PARAMLIST_H
#define PARAMLIST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PARAM_LIST_RECORD_SIZE = 8, // 单条参数记录长度, 单位: byte
};

enum param_list_type {
    PARAM_LIST_U32 = 1,    // 无符号 32 位参数
    PARAM_LIST_F32 = 2,    // 单精度浮点参数
    PARAM_LIST_ENUM32 = 3, // 32 位枚举参数
};

enum param_list_result {
    PARAM_LIST_INVALID = -1,  // 参数模式或数据无效
    PARAM_LIST_MISMATCH = -2, // 参数类型或版本不兼容
};

struct param_list_descriptor {
    uint16_t id;      // 参数永久标识
    uint16_t offset;  // 字段在配置结构体内的字节偏移
    uint8_t type;     // 参数存储类型
    uint8_t revision; // 参数兼容性版本
    uint8_t shift;    // 位域起始位
    uint8_t bits;     // 位域宽度
};

struct param_list {
    const struct param_list_descriptor *descriptors;
    uint32_t cnt;    // 有效元素数量
    size_t cfg_size; // 配置结构体大小, 单位: byte
    const void *defaults;
};

/**
 * @brief           将配置编码为参数记录
 * @param[in]  list 参数模式和默认配置
 * @param[in]  cfg  配置对象
 * @param[out] wire 编码参数记录
 * @param[in]  cap  输出缓冲区容量, 单位: byte
 * @return          成功返回编码字节数, 失败返回 PARAM_LIST_INVALID
 */
int param_list_encode(const struct param_list *list,
                      const void *cfg,
                      uint8_t *wire,
                      size_t cap);

/**
 * @brief              将参数记录解码到配置
 * @param[in]     list 参数模式和默认配置
 * @param[in,out] cfg  配置对象
 * @param[in]     wire 编码参数记录
 * @param[in]     len  记录数据长度, 单位: byte
 * @return             成功返回 0, 无效数据返回 PARAM_LIST_INVALID, 类型或版本不兼容返回 PARAM_LIST_MISMATCH
 */
int param_list_decode(const struct param_list *list,
                      void *cfg,
                      const uint8_t *wire,
                      size_t len);

/**
 * @brief              结合默认值恢复参数配置
 * @param[in]     list 参数模式和默认配置
 * @param[in,out] cfg  配置对象
 * @param[in]     wire 编码参数记录
 * @param[in]     len  记录数据长度, 单位: byte
 * @return             成功返回 0, 无效数据返回 PARAM_LIST_INVALID, 类型或版本不兼容返回 PARAM_LIST_MISMATCH
 * @note               先恢复默认配置, 再应用兼容记录; 缺少的参数保留默认值
 */
int param_list_restore(const struct param_list *list,
                       void *cfg,
                       const uint8_t *wire,
                       size_t len);

struct param_list_invalid_field {
    uint16_t id;        // 参数永久标识
    uint16_t offset;    // 字段在配置结构体内的字节偏移
    uint8_t type;       // 参数存储类型
    uint8_t revision;   // 参数兼容性版本
    uint32_t raw_value; // 原始参数存储值
};

/**
 * @brief           查找配置中的首个无效字段
 * @param[in]  list 参数模式和默认配置
 * @param[in]  cfg  配置对象
 * @param[out] out  首个无效字段信息, 可为 NULL
 * @return          找到无效字段返回 1, 未找到或模式及参数无效返回 0
 */
int param_list_first_invalid(const struct param_list *list,
                             const void *cfg,
                             struct param_list_invalid_field *out);

#ifdef __cplusplus
}
#endif

#endif
