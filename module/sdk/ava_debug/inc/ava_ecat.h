/** \file
 * \brief PDO 自由运行守护进程的共享内存定义
 *
 * 共享内存名称: "Local\\SOEM_PDO_FREERUN"
 *
 * PDO 布局与 FSA Neo 从站代码(FSANeo.c)一致:
 *   RxPDO (主站->从站, 28 字节): control/target/mode + set_pvct + special_cmd
 *   TxPDO (从站->主站, 61 字节): status/actual/mode + get_pvct + telemetry + err_code
 *
 * 半精度(IEEE-754 binary16)字段以 uint16_t 原始值存储,
 * 使用 fsa_float16_to_float() 解码.
 */

#ifndef AVA_ECAT_H
#define AVA_ECAT_H

#include <stdint.h>

#define SHM_NAME         "Local\\SOEM_PDO_FREERUN"
#define SHM_MAX_SLAVES   64
#define SHM_MAX_PDO_SIZE 256
#define SHM_MAGIC        0x534F454D /* "SOEM" */

/* ==================== 半精度浮点数 (IEEE-754 binary16) ====================
 *
 * C 没有原生半精度浮点类型, 此处仅存储 16 位原始数据;
 * EtherCAT 传输和共享内存访问定义见下文.
 */
/* ==================== FSA Neo PDO 结构 ====================
 *
 * 以下类型对应 FSANeo.c 中的 APPL_OutputMapping/APPL_InputMapping,
 * 可用于解析 output_data/input_data 原始字节缓冲区.
 */

#if defined(_MSC_VER)
#pragma pack(push, 1)
#define FSA_PACKED
#elif defined(__GNUC__) || defined(__clang__)
#define FSA_PACKED __attribute__((packed))
#else
#define FSA_PACKED
#endif

/** p/v/c/t 指令块(16 字节, 在 rt_ctrl_data 中偏移 0x10). */
struct fsa_set_pvct {
    float    p;    /* 0x00 目标位置 (float32) */
    uint16_t v;    /* 0x04 目标速度 (binary16) */
    uint16_t t_ff; /* 0x06 力矩前馈 (binary16) */
    uint16_t c;    /* 0x08 目标电流 (binary16) */
    uint16_t t;    /* 0x0A 目标力矩 (binary16) */
    uint16_t v_ff; /* 0x0C 速度前馈 (binary16) */
    uint16_t c_ff; /* 0x0E 电流前馈 (binary16) */
} FSA_PACKED;

/** p/v/c/t 反馈块(12 字节, 在 rt_ctrl_data 中偏移 0x00). */
struct fsa_get_pvct {
    float    p;  /* 0x00 实际位置 (float32) */
    uint16_t v;  /* 0x04 实际速度 (binary16) */
    uint16_t c;  /* 0x06 实际电流 (binary16) */
    uint16_t t;  /* 0x08 实际力矩 (binary16) */
    uint16_t te; /* 0x0A 电磁力矩 (binary16) */
} FSA_PACKED;

/** 遥测数据块(6 字节, 在 rt_ctrl_data 中偏移 0x2A). */
struct fsa_telemetry {
    uint16_t mos;  /* MOSFET 温度 (binary16) */
    uint16_t coil; /* 线圈温度 (binary16) */
    uint16_t vbus; /* 母线电压 (binary16) */
} FSA_PACKED;

/** 错误码块(32 字节, 在 rt_ctrl_data 中偏移 0x30). */
struct fsa_err_code {
    uint32_t err_arr[8];
} FSA_PACKED;

/** RxPDO = 主站 -> 从站(28 字节), 对应 APPL_OutputMapping. */
struct fsa_neo_rx_pdo {
    uint16_t            control_word;    /* 0x6040    */
    int32_t             target_position; /* 0x607A    */
    int32_t             target_velocity; /* 0x60FF    */
    int8_t              mode_of_op;      /* 0x6060    */
    struct fsa_set_pvct set_pvct;        /* 0x2010..0x201E */
    uint8_t             special_cmd;     /* 0x2080    */
} FSA_PACKED;

/** TxPDO = 从站 -> 主站(61 字节), 对应 APPL_InputMapping. */
struct fsa_neo_tx_pdo {
    uint16_t             status_word;           /* 0x6041 */
    int32_t              position_actual_value; /* 0x6064 */
    int32_t              velocity_actual_value; /* 0x606C */
    int8_t               mode_of_op_display;    /* 0x6061 */
    struct fsa_get_pvct  get_pvct;              /* 0x2000..0x200A */
    struct fsa_telemetry telemetry;             /* 0x202A..0x202E */
    struct fsa_err_code  err_code;              /* 0x2030..0x204C */
} FSA_PACKED;

#if defined(_MSC_VER)
#pragma pack(pop)
#endif

/* 编译期大小检查(半精度字段要求全部紧凑排列). */
#if defined(__cplusplus) && __cplusplus >= 201103L
static_assert(sizeof(struct fsa_set_pvct) == 16, "struct fsa_set_pvct size");
static_assert(sizeof(struct fsa_get_pvct) == 12, "struct fsa_get_pvct size");
static_assert(sizeof(struct fsa_telemetry) == 6, "struct fsa_telemetry size");
static_assert(sizeof(struct fsa_err_code) == 32, "struct fsa_err_code size");
static_assert(sizeof(struct fsa_neo_rx_pdo) == 28, "struct fsa_neo_rx_pdo size");
static_assert(sizeof(struct fsa_neo_tx_pdo) == 61, "struct fsa_neo_tx_pdo size");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(struct fsa_set_pvct) == 16, "struct fsa_set_pvct size");
_Static_assert(sizeof(struct fsa_get_pvct) == 12, "struct fsa_get_pvct size");
_Static_assert(sizeof(struct fsa_telemetry) == 6, "struct fsa_telemetry size");
_Static_assert(sizeof(struct fsa_err_code) == 32, "struct fsa_err_code size");
_Static_assert(sizeof(struct fsa_neo_rx_pdo) == 28, "struct fsa_neo_rx_pdo size");
_Static_assert(sizeof(struct fsa_neo_tx_pdo) == 61, "struct fsa_neo_tx_pdo size");
#endif

#define FSA_NEO_BASE_RX_PDO_SIZE      28U
#define FSA_NEO_BASE_TX_PDO_SIZE      64U
#define FSA_NEO_RX_DEBUG_OFFSET       FSA_NEO_BASE_RX_PDO_SIZE

#define AVA_ECAT_PDO_DATA_BYTES       128U
#define AVA_ECAT_PDO_CMD_NONE         0U
#define AVA_ECAT_PDO_CMD_READ         1U
#define AVA_ECAT_PDO_CMD_WRITE        2U
#define AVA_ECAT_PDO_STATUS_IDLE      0U
#define AVA_ECAT_PDO_STATUS_OK        1U
#define AVA_ECAT_PDO_STATUS_BAD_CMD   2U
#define AVA_ECAT_PDO_STATUS_BAD_ADDR  3U
#define AVA_ECAT_PDO_STATUS_BAD_LEN   4U
#define AVA_ECAT_PDO_STATUS_UNALIGNED 5U

struct ava_ecat_pdo_req {
    uint32_t seq;
    uint32_t address;
    uint16_t length;
    uint8_t  command;
    uint8_t  reserved;
    uint8_t  data[AVA_ECAT_PDO_DATA_BYTES];
} FSA_PACKED;

struct ava_ecat_pdo_res {
    uint32_t ack;
    uint8_t  status;
    uint8_t  reserved[3];
    uint8_t  data[AVA_ECAT_PDO_DATA_BYTES];
    uint32_t write_count;
} FSA_PACKED;

struct fsa_neo_rx_pdo_ext {
    struct fsa_neo_rx_pdo   base;
    struct ava_ecat_pdo_req mem_debug;
} FSA_PACKED;

struct fsa_neo_tx_pdo_ext {
    struct fsa_neo_tx_pdo   base;
    uint8_t                 base_padding[FSA_NEO_BASE_TX_PDO_SIZE - sizeof(struct fsa_neo_tx_pdo)];
    struct ava_ecat_pdo_res mem_debug;
} FSA_PACKED;

#define FSA_NEO_RX_PDO_SIZE (FSA_NEO_BASE_RX_PDO_SIZE + sizeof(struct ava_ecat_pdo_req))
#define FSA_NEO_TX_PDO_SIZE (FSA_NEO_BASE_TX_PDO_SIZE + sizeof(struct ava_ecat_pdo_res))

/** 共享内存中的单从站 PDO 数据. */
struct shm_slave_pdo {
    uint32_t output_size; /* 输出 PDO 字节数. */
    uint8_t  output_data[SHM_MAX_PDO_SIZE];

    uint32_t input_size; /* 输入 PDO 字节数. */
    uint8_t  input_data[SHM_MAX_PDO_SIZE];
};

/** 共享内存布局. */
struct shm_header {
    uint32_t magic;     /* SHM_MAGIC */
    uint32_t version;   /* 结构版本, 当前为 1. */
    uint32_t heartbeat; /* 每周期递增. */
    uint32_t slave_count;

    struct shm_slave_pdo slaves[SHM_MAX_SLAVES];
};

#ifdef __cplusplus
extern "C" {
#endif

#define AVA_ECAT_COE_INDEX        0x3000U
#define AVA_ECAT_COE_SUB_ADDRESS  0x01U
#define AVA_ECAT_COE_SUB_LENGTH   0x02U
#define AVA_ECAT_COE_SUB_COMMAND  0x03U
#define AVA_ECAT_COE_SUB_STATUS   0x04U
#define AVA_ECAT_COE_SUB_DATA     0x05U
#define AVA_ECAT_COE_SUB_WR_COUNT 0x06U

#define AVA_ECAT_CHUNK_BYTES      128U

#define AVA_ECAT_CMD_NONE         0U
#define AVA_ECAT_CMD_READ         1U
#define AVA_ECAT_CMD_WRITE        2U

#define AVA_ECAT_STATUS_IDLE      0U
#define AVA_ECAT_STATUS_OK        1U
#define AVA_ECAT_STATUS_BAD_CMD   2U
#define AVA_ECAT_STATUS_BAD_ADDR  3U
#define AVA_ECAT_STATUS_BAD_LEN   4U
#define AVA_ECAT_STATUS_UNALIGNED 5U
#define AVA_ECAT_STATUS_TIMEOUT   6U

typedef int (*ava_ecat_sdo_read_fn)(
    void *user, uint16_t index, uint8_t subindex, void *data, uint32_t size);
typedef int (*ava_ecat_sdo_write_fn)(
    void *user, uint16_t index, uint8_t subindex, const void *data, uint32_t size);

struct ava_ecat_sdo {
    ava_ecat_sdo_read_fn  read;
    ava_ecat_sdo_write_fn write;
    void                 *user;
};

enum ava_ecat_result {
    AVA_ECAT_OK          = 0,
    AVA_ECAT_ERR_ARG     = -1,
    AVA_ECAT_ERR_ALIGN   = -2,
    AVA_ECAT_ERR_SDO     = -3,
    AVA_ECAT_ERR_DEVICE  = -4,
    AVA_ECAT_ERR_TIMEOUT = -5,
};

int ava_ecat_sdo_read_memory(struct ava_ecat_sdo *ecat, uint32_t addr, void *dst, uint32_t bytes);
int ava_ecat_sdo_write_memory(struct ava_ecat_sdo *ecat,
                              uint32_t             addr,
                              const void          *src,
                              uint32_t             bytes);
int ava_ecat_sdo_read_u32(struct ava_ecat_sdo *ecat, uint32_t addr, uint32_t *value);
int ava_ecat_sdo_write_u32(struct ava_ecat_sdo *ecat, uint32_t addr, uint32_t value);
typedef int (*ava_ecat_pdo_wait_fn)(void *user, uint32_t seq);

struct ava_ecat_pdo {
    volatile struct ava_ecat_pdo_req       *req;
    volatile const struct ava_ecat_pdo_res *res;
    uint32_t                                seq;
    ava_ecat_pdo_wait_fn                    wait;
    void                                   *wait_user;
};

int ava_ecat_pdo_init(struct ava_ecat_pdo *pdo,
                      void                *output_pdo,
                      uint32_t             output_size,
                      const void          *input_pdo,
                      uint32_t             input_size,
                      ava_ecat_pdo_wait_fn wait,
                      void                *wait_user);
int ava_ecat_pdo_read(
    struct ava_ecat_pdo *pdo, uint32_t addr, void *dst, uint32_t bytes, uint32_t max_cycles);
int ava_ecat_pdo_write(
    struct ava_ecat_pdo *pdo, uint32_t addr, const void *src, uint32_t bytes, uint32_t max_cycles);
int ava_ecat_pdo_read_u32(struct ava_ecat_pdo *pdo,
                          uint32_t             addr,
                          uint32_t            *value,
                          uint32_t             max_cycles);
int ava_ecat_pdo_write_u32(struct ava_ecat_pdo *pdo,
                           uint32_t             addr,
                           uint32_t             value,
                           uint32_t             max_cycles);

#ifdef __cplusplus
}
#endif

#endif /* AVA_ECAT_H */
