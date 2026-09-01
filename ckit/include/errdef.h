#ifndef ERRDEF_H
#define ERRDEF_H

#include "platdef.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 统一错误码, 枚举保存正值, 函数失败返回对应负值
 * 按功能分组连续编号, 调用方使用枚举名称判断错误
 */
enum errdef {
    // 成功与参数
    MOK = 0,       // 操作成功
    MEINVAL = 1,   // 参数取值或组合无效
    MENULLPTR = 2, // 必需的指针为空
    MERANGE = 3,   // 数值超出允许范围

    // 资源与运行状态
    MEBUSY = 4,      // 资源被占用或传输尚未完成
    MEACCES = 5,     // 当前操作无访问权限
    MECREATE = 6,    // 创建资源失败
    MEALLOC = 7,     // 分配内存失败
    MEXIST = 8,      // 资源已存在
    MENOTFOUND = 9,  // 未找到目标资源
    MENOTSUP = 10,   // 不支持该操作
    MEOVERFLOW = 11, // 缓冲区或容量超限
    MESTATE = 12,    // 状态不符合操作要求
    MEAGAIN = 13,    // 暂无结果或事件, 可稍后再试
    MENOTINIT = 14,  // 对象尚未完成初始化

    // 配置与通信
    METIMEOUT = 15, // 操作超过等待期限
    MESYSERR = 16,  // 底层接口调用失败, 细节由底层状态记录
    MEIO = 17,      // 无法进一步分类的输入输出错误
    MECFG = 18,     // 外设或工作模式配置不符合要求
    MEDMACFG = 19,  // DMA 句柄缺失或 DMA 配置不符合要求

    // 数据校验与设备故障
    MECHECKSUM = 20, // 数据完整性校验失败, 如 CRC, 校验和或 MD5 摘要不匹配
    MEVERIFY = 21,   // 数据校验或寄存器读回比对失败
    MESENSOR = 22,   // 传感器报告自身故障

    MECOUNT = 23, // 错误码数量, 不是有效错误码
};

// 公共 API 成功返回 0, 失败返回负错误码
#define MERR(code) (-(int)(code))

/**
 * @brief            取得错误码的正值
 * @param[in] result 正错误码或函数返回值, 不得为 INT_MIN
 * @return           对应非负错误码
 */
HAPI int merr_code(const int result)
{
    return result < 0 ? -result : result;
}

/**
 * @brief            判断函数返回值是否表示失败
 * @param[in] result 函数返回值
 * @return           负值返回 1, 否则返回 0
 */
HAPI int is_merr_error(const int result)
{
    return result < 0;
}

/**
 * @brief            判断错误码是否已定义
 * @param[in] result 正错误码或函数返回值, 不得为 INT_MIN
 * @return           已定义返回 1, 否则返回 0
 */
HAPI int is_merr_known(const int result)
{
    const int code = merr_code(result);
    return code >= MOK && code < MECOUNT;
}

/**
 * @brief            获取错误码名称
 * @param[in] result 正错误码或函数返回值, 不得为 INT_MIN
 * @return           静态名称字符串, 未定义时返回 MEUNKNOWN, 始终非 NULL
 */
const char *merr_name(int result);

/**
 * @brief            获取错误说明
 * @param[in] result 正错误码或函数返回值, 不得为 INT_MIN
 * @return           静态说明字符串, 始终非 NULL
 */
const char *merr_msg(int result);

#ifdef __cplusplus
}
#endif

#endif
