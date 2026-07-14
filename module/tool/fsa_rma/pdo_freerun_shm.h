/** \file
 * \brief Shared memory definitions for PDO freerun daemon
 *
 * Shared memory name: "Local\\SOEM_PDO_FREERUN"
 *
 * Debug PDO layout expects the master to assign both the default PDO and the debug PDO:
 *   RxPDO: 0x1600 (28 bytes) + 0x1603 (140 bytes) = 168 bytes
 *   TxPDO: 0x1A00 (64 bytes) + 0x1A03 (140 bytes) = 204 bytes
 *
 * Half-precision (IEEE-754 binary16) fields are stored as raw uint16_t
 * (float16_t). Use fsa_float16_to_float() to decode.
 */

#ifndef PDO_FREERUN_SHM_H
#define PDO_FREERUN_SHM_H

#include <stdint.h>

#define SHM_NAME                     "Local\\SOEM_PDO_FREERUN"
#define SHM_MAX_SLAVES               64
#define SHM_MAX_PDO_SIZE             256
#define SHM_MAGIC                    0x534F454D /* "SOEM" */

#define FSA_MEM_PDO_DATA_BYTES       128u

#define FSA_MEM_PDO_CMD_NONE         0u
#define FSA_MEM_PDO_CMD_READ         1u
#define FSA_MEM_PDO_CMD_WRITE        2u

#define FSA_MEM_PDO_STATUS_IDLE      0u
#define FSA_MEM_PDO_STATUS_OK        1u
#define FSA_MEM_PDO_STATUS_BAD_CMD   2u
#define FSA_MEM_PDO_STATUS_BAD_ADDR  3u
#define FSA_MEM_PDO_STATUS_BAD_LEN   4u
#define FSA_MEM_PDO_STATUS_UNALIGNED 5u

typedef uint16_t float16_t;

#if defined(_MSC_VER)
#pragma pack(push, 1)
#define FSA_PACKED
#elif defined(__GNUC__) || defined(__clang__)
#define FSA_PACKED __attribute__((packed))
#else
#define FSA_PACKED
#endif

typedef struct fsa_set_pvct {
        float     p;
        float16_t v;
        float16_t t_ff;
        float16_t c;
        float16_t t;
        float16_t v_ff;
        float16_t c_ff;
} FSA_PACKED fsa_set_pvct_t;

typedef struct fsa_get_pvct {
        float     p;
        float16_t v;
        float16_t c;
        float16_t t;
        float16_t te;
} FSA_PACKED fsa_get_pvct_t;

typedef struct fsa_telemetry {
        float16_t mos;
        float16_t coil;
        float16_t vbus;
} FSA_PACKED fsa_telemetry_t;

typedef struct fsa_err_code {
        uint32_t err_arr[8];
} FSA_PACKED fsa_err_code_t;

typedef struct fsa_mem_pdo_req {
        uint32_t seq;
        uint32_t address;
        uint16_t length;
        uint8_t  command;
        uint8_t  reserved;
        uint8_t  data[FSA_MEM_PDO_DATA_BYTES];
} FSA_PACKED fsa_mem_pdo_req_t;

typedef struct fsa_mem_pdo_res {
        uint32_t ack;
        uint8_t  status;
        uint8_t  reserved[3];
        uint8_t  data[FSA_MEM_PDO_DATA_BYTES];
        uint32_t write_count;
} FSA_PACKED fsa_mem_pdo_res_t;

typedef struct fsa_neo_rx_pdo {
        uint16_t       control_word;
        int32_t        target_position;
        int32_t        target_velocity;
        int8_t         mode_of_op;
        fsa_set_pvct_t set_pvct;
        uint8_t        special_cmd;
} FSA_PACKED fsa_neo_rx_pdo_t;

typedef struct fsa_neo_rx_pdo_ext {
        fsa_neo_rx_pdo_t  base;
        fsa_mem_pdo_req_t mem_debug;
} FSA_PACKED fsa_neo_rx_pdo_ext_t;

typedef struct fsa_neo_tx_pdo {
        uint16_t        status_word;
        int32_t         position_actual_value;
        int32_t         velocity_actual_value;
        int8_t          mode_of_op_display;
        fsa_get_pvct_t  get_pvct;
        fsa_telemetry_t telemetry;
        fsa_err_code_t  err_code;
} FSA_PACKED fsa_neo_tx_pdo_t;

typedef struct fsa_neo_tx_pdo_ext {
        fsa_neo_tx_pdo_t  base;
        uint8_t           base_padding[3];
        fsa_mem_pdo_res_t mem_debug;
} FSA_PACKED fsa_neo_tx_pdo_ext_t;

#if defined(_MSC_VER)
#pragma pack(pop)
#endif

#if defined(__cplusplus) && __cplusplus >= 201103L
static_assert(sizeof(fsa_set_pvct_t) == 16, "fsa_set_pvct_t size");
static_assert(sizeof(fsa_get_pvct_t) == 12, "fsa_get_pvct_t size");
static_assert(sizeof(fsa_telemetry_t) == 6, "fsa_telemetry_t size");
static_assert(sizeof(fsa_err_code_t) == 32, "fsa_err_code_t size");
static_assert(sizeof(fsa_mem_pdo_req_t) == 140, "fsa_mem_pdo_req_t size");
static_assert(sizeof(fsa_mem_pdo_res_t) == 140, "fsa_mem_pdo_res_t size");
static_assert(sizeof(fsa_neo_rx_pdo_t) == 28, "fsa_neo_rx_pdo_t size");
static_assert(sizeof(fsa_neo_tx_pdo_t) == 61, "fsa_neo_tx_pdo_t size");
static_assert(sizeof(fsa_neo_rx_pdo_ext_t) == 168, "fsa_neo_rx_pdo_ext_t size");
static_assert(sizeof(fsa_neo_tx_pdo_ext_t) == 204, "fsa_neo_tx_pdo_ext_t size");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(fsa_set_pvct_t) == 16, "fsa_set_pvct_t size");
_Static_assert(sizeof(fsa_get_pvct_t) == 12, "fsa_get_pvct_t size");
_Static_assert(sizeof(fsa_telemetry_t) == 6, "fsa_telemetry_t size");
_Static_assert(sizeof(fsa_err_code_t) == 32, "fsa_err_code_t size");
_Static_assert(sizeof(fsa_mem_pdo_req_t) == 140, "fsa_mem_pdo_req_t size");
_Static_assert(sizeof(fsa_mem_pdo_res_t) == 140, "fsa_mem_pdo_res_t size");
_Static_assert(sizeof(fsa_neo_rx_pdo_t) == 28, "fsa_neo_rx_pdo_t size");
_Static_assert(sizeof(fsa_neo_tx_pdo_t) == 61, "fsa_neo_tx_pdo_t size");
_Static_assert(sizeof(fsa_neo_rx_pdo_ext_t) == 168, "fsa_neo_rx_pdo_ext_t size");
_Static_assert(sizeof(fsa_neo_tx_pdo_ext_t) == 204, "fsa_neo_tx_pdo_ext_t size");
#endif

#define FSA_NEO_BASE_RX_PDO_SIZE 28u
#define FSA_NEO_BASE_TX_PDO_SIZE 64u
#define FSA_NEO_RX_PDO_SIZE      168u
#define FSA_NEO_TX_PDO_SIZE      204u

typedef struct shm_slave_pdo {
        uint32_t output_size;
        uint8_t  output_data[SHM_MAX_PDO_SIZE];

        uint32_t input_size;
        uint8_t  input_data[SHM_MAX_PDO_SIZE];
} shm_slave_pdo_t;

typedef struct shm_header {
        uint32_t magic;
        uint32_t version;
        uint32_t heartbeat;
        uint32_t slave_count;

        shm_slave_pdo_t slaves[SHM_MAX_SLAVES];
} shm_header_t;

#endif /* PDO_FREERUN_SHM_H */
