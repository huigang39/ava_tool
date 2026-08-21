#include "../inc/ava_sdk.h"
#include "../inc/ava_ecat.h"
#include "../inc/elf_parser.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#error "ava_sdk currently supports Windows only"
#endif

#define AVA_SDK_PATH_MAX    1024
#define AVA_SDK_NAME_MAX    512
#define AVA_SDK_WAIT_CYCLES 1000U

struct ava_sdk {
    HANDLE              map_handle;
    struct shm_header  *shm;
    uint32_t            slave_index;
    char                elf_path[AVA_SDK_PATH_MAX];
    struct elf_parser  *elf;
    struct ava_ecat_pdo memory;
};

static int
copy_string(char *dst, size_t capacity, const char *src)
{
    size_t length;
    if (dst == NULL || src == NULL || capacity == 0)
        return 0;
    length = strlen(src);
    if (length >= capacity)
        return 0;
    memcpy(dst, src, length + 1);
    return 1;
}

static int
wait_pdo_cycle(void *user, uint32_t seq)
{
    const struct ava_sdk *sdk       = (const struct ava_sdk *)user;
    uint32_t              heartbeat = sdk->shm->heartbeat;
    uint32_t              spins;
    (void)seq;

    if (sdk->shm->magic != SHM_MAGIC || sdk->shm->version != 1U)
        return AVA_ECAT_ERR_TIMEOUT;

    for (spins = 0; spins < 20; ++spins) {
        Sleep(1);
        if (sdk->shm->heartbeat != heartbeat)
            return 0;
        if (sdk->shm->magic != SHM_MAGIC)
            return AVA_ECAT_ERR_TIMEOUT;
    }
    return AVA_ECAT_ERR_TIMEOUT;
}

int
ava_sdk_init(struct ava_sdk **out, const char *elf_path, uint32_t slave_index)
{
    struct ava_sdk       *sdk;
    struct shm_slave_pdo *slave;
    int                   ret;

    if (out == NULL || elf_path == NULL)
        return AVA_SDK_ERR_ARG;
    *out = NULL;
    sdk  = (struct ava_sdk *)calloc(1, sizeof(*sdk));
    if (sdk == NULL)
        return AVA_SDK_ERR_OPEN;

    if (!copy_string(sdk->elf_path, sizeof(sdk->elf_path), elf_path)) {
        free(sdk);
        return AVA_SDK_ERR_ARG;
    }
    if (!elf_parser_open(&sdk->elf, sdk->elf_path)) {
        free(sdk);
        return AVA_SDK_ERR_IMAGE;
    }

    sdk->map_handle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, false, SHM_NAME);
    if (sdk->map_handle == NULL) {
        free(sdk);
        return AVA_SDK_ERR_OPEN;
    }
    sdk->shm = (struct shm_header *)MapViewOfFile(
        sdk->map_handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(struct shm_header));
    if (sdk->shm == NULL || sdk->shm->magic != SHM_MAGIC || sdk->shm->version != 1U ||
        slave_index >= sdk->shm->slave_count) {
        ava_sdk_close(sdk);
        return AVA_SDK_ERR_OPEN;
    }

    sdk->slave_index = slave_index;
    {
        uint32_t heartbeat = sdk->shm->heartbeat;
        uint32_t wait_ms;
        for (wait_ms = 0; wait_ms < 100U && sdk->shm->heartbeat == heartbeat; ++wait_ms)
            Sleep(1);
        if (sdk->shm->heartbeat == heartbeat) {
            ava_sdk_close(sdk);
            return AVA_SDK_ERR_OPEN;
        }
    }
    slave = &sdk->shm->slaves[slave_index];
    ret   = ava_ecat_pdo_init(&sdk->memory,
                            slave->output_data,
                            slave->output_size,
                            slave->input_data,
                            slave->input_size,
                            wait_pdo_cycle,
                            sdk);
    if (ret != AVA_ECAT_OK) {
        ava_sdk_close(sdk);
        return AVA_SDK_ERR_OPEN;
    }

    *out = sdk;
    return AVA_SDK_OK;
}

void
ava_sdk_close(struct ava_sdk *sdk)
{
    if (sdk == NULL)
        return;
    elf_parser_close(sdk->elf);
    if (sdk->shm != NULL)
        UnmapViewOfFile(sdk->shm);
    if (sdk->map_handle != NULL)
        CloseHandle(sdk->map_handle);
    free(sdk);
}

int
ava_sdk_resolve(struct ava_sdk *sdk, const char *variable, uint32_t *address)
{
    if (sdk == NULL || variable == NULL || address == NULL)
        return AVA_SDK_ERR_ARG;
    return elf_parser_resolve(sdk->elf, variable, address) ? AVA_SDK_OK : AVA_SDK_ERR_SYMBOL;
}

int
ava_sdk_query(struct ava_sdk *sdk, const char *variable, uint32_t *address, uint32_t *size)
{
    if (sdk == NULL || variable == NULL || address == NULL || size == NULL)
        return AVA_SDK_ERR_ARG;
    return elf_parser_resolve_info(sdk->elf, variable, address, size) ? AVA_SDK_OK
                                                                      : AVA_SDK_ERR_SYMBOL;
}

int
ava_sdk_format(struct ava_sdk *sdk,
               const char     *variable,
               const void     *data,
               uint32_t        bytes,
               char           *output,
               size_t          output_capacity)
{
    if (sdk == NULL || variable == NULL || data == NULL || output == NULL || output_capacity == 0)
        return AVA_SDK_ERR_ARG;
    return elf_parser_format(sdk->elf, variable, data, bytes, output, output_capacity)
               ? AVA_SDK_OK
               : AVA_SDK_ERR_SYMBOL;
}
int
ava_sdk_encode(
    struct ava_sdk *sdk, const char *variable, const char *text, void *output, uint32_t output_size)
{
    if (sdk == NULL || variable == NULL || text == NULL || output == NULL)
        return AVA_SDK_ERR_ARG;
    return elf_parser_encode(sdk->elf, variable, text, output, output_size) ? AVA_SDK_OK
                                                                            : AVA_SDK_ERR_SYMBOL;
}

int
ava_sdk_read(struct ava_sdk *sdk, const char *variable, void *dst, uint32_t bytes)
{
    uint32_t address;
    int      ret = ava_sdk_resolve(sdk, variable, &address);
    if (ret != AVA_SDK_OK)
        return ret;
    return ava_ecat_pdo_read(&sdk->memory, address, dst, bytes, AVA_SDK_WAIT_CYCLES) == AVA_ECAT_OK
               ? AVA_SDK_OK
               : AVA_SDK_ERR_IO;
}

int
ava_sdk_write(struct ava_sdk *sdk, const char *variable, const void *src, uint32_t bytes)
{
    uint32_t address;
    int      ret = ava_sdk_resolve(sdk, variable, &address);
    if (ret != AVA_SDK_OK)
        return ret;
    return ava_ecat_pdo_write(&sdk->memory, address, src, bytes, AVA_SDK_WAIT_CYCLES) == AVA_ECAT_OK
               ? AVA_SDK_OK
               : AVA_SDK_ERR_IO;
}

int
ava_sdk_read_at(struct ava_sdk *sdk, uint32_t address, void *dst, uint32_t bytes)
{
    if (sdk == NULL || dst == NULL || bytes == 0)
        return AVA_SDK_ERR_ARG;
    return ava_ecat_pdo_read(&sdk->memory, address, dst, bytes, AVA_SDK_WAIT_CYCLES) == AVA_ECAT_OK
               ? AVA_SDK_OK
               : AVA_SDK_ERR_IO;
}

int
ava_sdk_write_at(struct ava_sdk *sdk, uint32_t address, const void *src, uint32_t bytes)
{
    if (sdk == NULL || src == NULL || bytes == 0)
        return AVA_SDK_ERR_ARG;
    return ava_ecat_pdo_write(&sdk->memory, address, src, bytes, AVA_SDK_WAIT_CYCLES) == AVA_ECAT_OK
               ? AVA_SDK_OK
               : AVA_SDK_ERR_IO;
}
int
ava_sdk_get_debug(struct ava_sdk *sdk, struct ava_sdk_debug *debug)
{
    if (sdk == NULL || debug == NULL)
        return AVA_SDK_ERR_ARG;
    debug->heartbeat   = sdk->shm->heartbeat;
    debug->req_seq     = sdk->memory.req->seq;
    debug->req_address = sdk->memory.req->address;
    debug->req_length  = sdk->memory.req->length;
    debug->req_command = sdk->memory.req->command;
    debug->res_ack     = sdk->memory.res->ack;
    debug->res_status  = sdk->memory.res->status;
    memcpy(debug->res_raw, (const uint8_t *)sdk->memory.res - 4, sizeof(debug->res_raw));
    return AVA_SDK_OK;
}
int
ava_sdk_read_f32(struct ava_sdk *sdk, const char *variable, float *value)
{
    return ava_sdk_read(sdk, variable, value, sizeof(*value));
}

int
ava_sdk_write_f32(struct ava_sdk *sdk, const char *variable, float value)
{
    return ava_sdk_write(sdk, variable, &value, sizeof(value));
}

const char *
ava_sdk_strerror(int code)
{
    switch (code) {
        case AVA_SDK_OK:
            return "success";
        case AVA_SDK_ERR_ARG:
            return "invalid argument";
        case AVA_SDK_ERR_OPEN:
            return "master/shared-memory open failed";
        case AVA_SDK_ERR_IMAGE:
            return "invalid AXF/ELF image";
        case AVA_SDK_ERR_SYMBOL:
            return "variable resolution failed";
        case AVA_SDK_ERR_IO:
            return "device communication failed";
        default:
            return "unknown error";
    }
}
