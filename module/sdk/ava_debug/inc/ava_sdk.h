#ifndef AVA_SDK_H
#define AVA_SDK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ava_sdk;

struct ava_sdk_debug {
    uint32_t heartbeat;
    uint32_t req_seq;
    uint32_t req_address;
    uint16_t req_length;
    uint8_t  req_command;
    uint32_t res_ack;
    uint8_t  res_status;
    uint8_t  res_raw[32];
};

enum ava_sdk_result {
    AVA_SDK_OK         = 0,
    AVA_SDK_ERR_ARG    = -100,
    AVA_SDK_ERR_OPEN   = -101,
    AVA_SDK_ERR_IMAGE  = -102,
    AVA_SDK_ERR_SYMBOL = -103,
    AVA_SDK_ERR_IO     = -104,
};

/*
 * 打开共享内存, 并使用内置解析器加载 CM7 AXF/ELF 镜像.
 */
int  ava_sdk_init(struct ava_sdk **out, const char *elf_path, uint32_t slave_index);
void ava_sdk_close(struct ava_sdk *sdk);

/* 解析全局变量或点分隔的 C 成员, 如 g_foc.lo.vel_pi.cfg.kp. */
int ava_sdk_resolve(struct ava_sdk *sdk, const char *variable, uint32_t *address);
int ava_sdk_query(struct ava_sdk *sdk, const char *variable, uint32_t *address, uint32_t *size);
int ava_sdk_format(struct ava_sdk *sdk,
                   const char     *variable,
                   const void     *data,
                   uint32_t        bytes,
                   char           *output,
                   size_t          output_capacity);
int ava_sdk_encode(struct ava_sdk *sdk,
                   const char     *variable,
                   const char     *text,
                   void           *output,
                   uint32_t        output_size);

/* 通用对齐内存访问, 当前设备协议要求 4 字节对齐. */
int ava_sdk_read(struct ava_sdk *sdk, const char *variable, void *dst, uint32_t bytes);
int ava_sdk_write(struct ava_sdk *sdk, const char *variable, const void *src, uint32_t bytes);
int ava_sdk_read_at(struct ava_sdk *sdk, uint32_t address, void *dst, uint32_t bytes);
int ava_sdk_write_at(struct ava_sdk *sdk, uint32_t address, const void *src, uint32_t bytes);
int ava_sdk_get_debug(struct ava_sdk *sdk, struct ava_sdk_debug *debug);

int         ava_sdk_read_f32(struct ava_sdk *sdk, const char *variable, float *value);
int         ava_sdk_write_f32(struct ava_sdk *sdk, const char *variable, float value);
const char *ava_sdk_strerror(int code);

#ifdef __cplusplus
}
#endif

#endif
