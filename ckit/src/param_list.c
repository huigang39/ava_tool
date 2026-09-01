#include "param_list.h"

#include <limits.h>
#include <string.h>

/**
 * @brief            读取参数记录标识
 * @param[in] record 参数记录
 */
static uint16_t record_id(const uint8_t *record);

/**
 * @brief       读取小端 32 位数据
 * @param[in] p 输入数据地址
 */
static uint32_t get32(const uint8_t *p);

/**
 * @brief             写入小端 32 位数据
 * @param[in,out] p   输入数据地址
 * @param[in]     val 参数原始值
 */
static void put32(uint8_t *p, uint32_t val);

/**
 * @brief          计算参数位域掩码
 * @param[in] bits 字段位数
 */
static uint32_t field_mask(uint8_t bits);

/**
 * @brief          检查参数模式定义是否有效
 * @param[in] list 参数模式
 */
static int is_schema_valid(const struct param_list *list);

/**
 * @brief         检查参数原始值是否有效
 * @param[in] d   参数字段描述
 * @param[in] val 参数原始值
 */
static int is_value_valid(const struct param_list_descriptor *d, uint32_t val);

/**
 * @brief         提取配置中的参数值
 * @param[in] cfg 配置数据
 * @param[in] d   参数字段描述
 */
static uint32_t field_val(const void *cfg,
                          const struct param_list_descriptor *d);

/**
 * @brief          检查配置参数值是否有效
 * @param[in] list 参数模式
 * @param[in] cfg  配置数据
 */
static int is_config_valid(const struct param_list *list, const void *cfg);

/**
 * @brief          检查编码记录是否有效
 * @param[in] list 参数模式
 * @param[in] wire 参数记录数据
 * @param[in] len  数据长度
 */
static int wire_valid(const struct param_list *list,
                      const uint8_t *wire,
                      size_t len);

/**
 * @brief              将参数记录写入配置
 * @param[in]     list 参数模式
 * @param[in,out] cfg  配置数据
 * @param[in]     wire 参数记录数据
 * @param[in]     len  数据长度
 */
static void apply_records(const struct param_list *list,
                          void *cfg,
                          const uint8_t *wire,
                          size_t len);

/**
 * @brief              解码已检查的参数记录
 * @param[in]     list 参数模式
 * @param[in,out] cfg  配置数据
 * @param[in]     wire 参数记录数据
 * @param[in]     len  数据长度
 */
static int decode_in_valid(const struct param_list *list,
                           void *cfg,
                           const uint8_t *wire,
                           size_t len);

int param_list_first_invalid(const struct param_list *list,
                             const void *cfg,
                             struct param_list_invalid_field *out)
{
    if (!is_schema_valid(list) || !cfg || !out) {
        return 0;
    }
    for (size_t i = 0; i < list->cnt; ++i) {
        const struct param_list_descriptor *d = &list->descriptors[i];
        const uint32_t val = field_val(cfg, d);
        if (!is_value_valid(d, val)) {
            out->id = d->id;
            out->offset = d->offset;
            out->type = d->type;
            out->revision = d->revision;
            out->raw_value = val;
            return 1;
        }
    }
    return 0;
}

static const struct param_list_descriptor *find_descriptor(
    const struct param_list *list, uint16_t id)
{
    for (size_t i = 0; i < list->cnt; ++i) {
        if (list->descriptors[i].id == id) {
            return &list->descriptors[i];
        }
    }
    return NULL;
}

int param_list_encode(const struct param_list *list,
                      const void *cfg,
                      uint8_t *wire,
                      size_t cap)
{
    if (!is_schema_valid(list) || list->cnt > cap / PARAM_LIST_RECORD_SIZE ||
        (list->cnt && !wire) || !is_config_valid(list, cfg)) {
        return PARAM_LIST_INVALID;
    }
    for (size_t i = 0; i < list->cnt; ++i) {
        const struct param_list_descriptor *d = &list->descriptors[i];
        uint8_t *record = wire + i * PARAM_LIST_RECORD_SIZE;
        record[0] = (uint8_t)d->id;
        record[1] = (uint8_t)(d->id >> 8);
        record[2] = d->type;
        record[3] = d->revision;
        put32(record + 4, field_val(cfg, d));
    }
    return (int)(list->cnt * PARAM_LIST_RECORD_SIZE);
}

int param_list_decode(const struct param_list *list,
                      void *cfg,
                      const uint8_t *wire,
                      size_t len)
{
    const int result = decode_in_valid(list, cfg, wire, len);
    if (result) {
        return result;
    }
    apply_records(list, cfg, wire, len);
    return 0;
}

int param_list_restore(const struct param_list *list,
                       void *cfg,
                       const uint8_t *wire,
                       size_t len)
{
    const int result = decode_in_valid(list, cfg, wire, len);
    if (result) {
        return result;
    }
    if (!is_config_valid(list, list->defaults)) {
        return PARAM_LIST_INVALID;
    }
    memcpy(cfg, list->defaults, list->cfg_size);
    apply_records(list, cfg, wire, len);
    return 0;
}

static uint16_t record_id(const uint8_t *record)
{
    return (uint16_t)((uint16_t)record[0] | (uint16_t)record[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t val)
{
    for (unsigned i = 0; i < 4; ++i) {
        p[i] = (uint8_t)(val >> (8 * i));
    }
}

static uint32_t field_mask(uint8_t bits)
{
    return UINT32_MAX >> (32 - bits);
}

static int is_schema_valid(const struct param_list *list)
{
    if (!list || !list->cfg_size || list->cnt > UINT16_MAX ||
        list->cnt > INT_MAX / PARAM_LIST_RECORD_SIZE ||
        (list->cnt && !list->descriptors)) {
        return 0;
    }
    for (size_t i = 0; i < list->cnt; ++i) {
        const struct param_list_descriptor *d = &list->descriptors[i];
        if (!d->id || !d->revision || !d->bits || d->bits > 32 ||
            d->shift + d->bits > 32 || d->offset > list->cfg_size ||
            list->cfg_size - d->offset < 4 || d->type < PARAM_LIST_U32 ||
            d->type > PARAM_LIST_ENUM32 ||
            (d->type == PARAM_LIST_F32 && (d->shift || d->bits != 32))) {
            return 0;
        }
        for (size_t j = 0; j < i; ++j) {
            if (list->descriptors[j].id == d->id) {
                return 0;
            }
        }
    }
    return 1;
}

static int is_value_valid(const struct param_list_descriptor *d, uint32_t val)
{
    return !(val & ~field_mask(d->bits)) &&
           (d->type != PARAM_LIST_F32 ||
            (val & UINT32_C(0x7f800000)) != UINT32_C(0x7f800000));
}

static uint32_t field_val(const void *cfg,
                          const struct param_list_descriptor *d)
{
    uint32_t val;
    memcpy(&val, (const uint8_t *)cfg + d->offset, sizeof(val));
    return (val >> d->shift) & field_mask(d->bits);
}

static int is_config_valid(const struct param_list *list, const void *cfg)
{
    if (!cfg) {
        return 0;
    }
    for (size_t i = 0; i < list->cnt; ++i) {
        if (!is_value_valid(&list->descriptors[i],
                            field_val(cfg, &list->descriptors[i]))) {
            return 0;
        }
    }
    return 1;
}

static int wire_valid(const struct param_list *list,
                      const uint8_t *wire,
                      size_t len)
{
    if (len % PARAM_LIST_RECORD_SIZE ||
        len / PARAM_LIST_RECORD_SIZE > UINT16_MAX || (len && !wire)) {
        return PARAM_LIST_INVALID;
    }
    for (size_t n = 0; n < len; n += PARAM_LIST_RECORD_SIZE) {
        const uint8_t *record = wire + n;
        const uint16_t id = record_id(record);
        if (!id || !record[3]) {
            return PARAM_LIST_INVALID;
        }
        for (size_t prev = 0; prev < n; prev += PARAM_LIST_RECORD_SIZE) {
            if (record_id(wire + prev) == id) {
                return PARAM_LIST_INVALID;
            }
        }
        const struct param_list_descriptor *d = find_descriptor(list, id);
        if (!d) {
            continue;
        }
        if (record[2] != d->type || record[3] != d->revision) {
            return PARAM_LIST_MISMATCH;
        }
        if (!is_value_valid(d, get32(record + 4))) {
            return PARAM_LIST_INVALID;
        }
    }
    return 0;
}

static void apply_records(const struct param_list *list,
                          void *cfg,
                          const uint8_t *wire,
                          size_t len)
{
    for (size_t n = 0; n < len; n += PARAM_LIST_RECORD_SIZE) {
        const uint8_t *record = wire + n;
        const struct param_list_descriptor *d =
            find_descriptor(list, record_id(record));
        if (d) {
            uint32_t val;
            memcpy(&val, (uint8_t *)cfg + d->offset, sizeof(val));
            val = (val & ~(field_mask(d->bits) << d->shift)) |
                  (get32(record + 4) << d->shift);
            memcpy((uint8_t *)cfg + d->offset, &val, sizeof(val));
        }
    }
}

static int decode_in_valid(const struct param_list *list,
                           void *cfg,
                           const uint8_t *wire,
                           size_t len)
{
    if (!is_schema_valid(list) || !cfg) {
        return PARAM_LIST_INVALID;
    }
    return wire_valid(list, wire, len);
}
