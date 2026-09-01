#ifndef MODULE_SAFETY_H
#define MODULE_SAFETY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 采样值类型; AUTO 仅用于解析定义
enum safety_value_type {
    SAFETY_AUTO = 0,
    SAFETY_UNSIGNED = 1,
    SAFETY_SIGNED = 2,
    SAFETY_BOOLEAN = 3,
    SAFETY_FLOATING = 4,
};

// 数值显示,位图或枚举解析
enum safety_kind { SAFETY_NUMBER = 0, SAFETY_BITS = 1, SAFETY_ENUM = 2 };

// 接口返回码, 与被解析的设备错误码无关
enum safety_error {
    SAFETY_OK = 0,
    SAFETY_INVALID = -1,  // 定义或输入无效
    SAFETY_MISMATCH = -2, // 数值类型或位宽与定义不匹配
    SAFETY_CAPACITY = -3, // 索引缓冲区不足; result.matched 给出所需数量
};

// 一个采样值; 整数使用原始位, 不转成浮点数
struct safety_value {
    uint64_t bits;               // 有效低 width 位; 有符号整数使用补码
    double real;                 // 浮点数值; 解析器不改写采样值
    unsigned width;              // 有效位宽, 1..64
    enum safety_value_type type; // 采样类型, 不能为 AUTO
};

// 一条匹配定义, 解释文字由调用方按索引查表
struct safety_entry {
    uint64_t val; // 位图为非零掩码, 枚举为 width 位原始值
};

// 解析规则, 不依赖 JSON 格式或设备变量名
struct safety_decoder {
    enum safety_kind kind;              // 解析方式
    enum safety_value_type type;        // AUTO 不限制数值显示类型
    unsigned width;                     // 1..64; 浮点为 16/32/64
    const struct safety_entry *entries; // 匹配定义数组, count 为 0 时可为 NULL
    size_t count;                       // 匹配定义数量
};

// 解析结果; 未定义的位和值保留给调用方显示
struct safety_result {
    size_t matched;        // 命中项总数, 不受输出缓冲区容量影响
    uint64_t unknown_mask; // 位图中未被命中定义覆盖的位
    int unknown_value;     // 枚举无匹配项时为 1
};

/**
 * @brief                     将十进制或 0x 整数文本转为原始位
 * @param[in]  text           以 NUL 结尾的文本, 允许首尾 ASCII 空白
 * @param[in]  width          位宽, 1..64
 * @param[in]  allow_negative 非零表示允许有符号范围内的负数
 * @param[out] out            解析结果; 失败时清零
 * @return                    成功返回 SAFETY_OK, 格式错误或溢出返回 SAFETY_INVALID
 */
int safety_parse_integer(const char *text,
                         unsigned width,
                         int allow_negative,
                         uint64_t *out);

/**
 * @brief             检查位宽,类型,重复定义及掩码范围
 * @param[in] decoder 解析规则
 * @return            有效返回 SAFETY_OK, 否则返回 SAFETY_INVALID
 */
int safety_validate(const struct safety_decoder *decoder);

/**
 * @brief               按定义解析采样值并返回命中项索引, 不分配内存
 * @param[in]  decoder  解析规则
 * @param[in]  val      采样值
 * @param[out] indices  命中项索引数组; capacity 为 0 时可为 NULL
 * @param[in]  capacity indices 的元素容量
 * @param[out] result   命中数量及未定义内容
 * @return              成功返回 SAFETY_OK; 错误返回 safety_error 中的负值
 * @note                位图要求掩码内所有位都置位; 枚举按完整值匹配
 *                      数值模式仅检查类型, 非零数值不视作故障
 *                      无效输入或类型不匹配时清零 result, 不写 indices
 *                      容量不足返回 SAFETY_CAPACITY, 保留已写索引并报告所需数量
 */
int safety_decode(const struct safety_decoder *decoder,
                  const struct safety_value *val,
                  size_t *indices,
                  size_t capacity,
                  struct safety_result *result);

/**
 * @brief              用原始整数位匹配条件
 * @param[in] val      采样值, 不支持浮点数
 * @param[in] expected 期望的掩码内数值
 * @param[in] mask     非零掩码, 不能超出采样位宽
 * @return             匹配返回 1, 不匹配返回 0, 无效输入返回 SAFETY_INVALID
 */
int safety_matches(const struct safety_value *val,
                   uint64_t expected,
                   uint64_t mask);

#ifdef __cplusplus
}
#endif
#endif
