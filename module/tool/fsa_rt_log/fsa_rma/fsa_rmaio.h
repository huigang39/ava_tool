#ifndef FSA_RMAIO_H
#define FSA_RMAIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FSA_RMAIO_BUF_SIZE 1400

typedef union {
    uint32_t  uint32_value;
    int32_t   int32_value;
    uint16_t  uint16_value[2];
    int16_t   int16_value[2];
    uint8_t   uint8_value[4];
    int8_t    int8_value[4];
    float fp32;
} word_u;

enum atomic_wait_time {
    US_0    = 0,
    US_10   = 1,
    US_20   = 2,
    US_50   = 3,
    US_100  = 4,
    US_200  = 5,
    US_500  = 6,
    US_1000 = 7,

    ATOMIC_WAIT_TIME_32BIT = INT32_MAX, // 确保返回值为32位,防止编译器自作主张
};

static float
u32_to_fp32(uint32_t data)
{
    word_u res;
    res.uint32_value = data;
    return res.fp32;
}

static inline uint32_t
fp32_to_u32(float data)
{
    word_u res;
    res.fp32 = data;
    return res.uint32_value;
}

/*---------------------------------- PC -> FSA ----------------------------------*/
struct rmaio_pc2fsa {
    uint8_t send_buf[FSA_RMAIO_BUF_SIZE]; // 必须为 uint8_t[1400]
    int     parse_index;
    int     generate_index;
};

enum rmaio_pc2fsa_cmd {
    RMAIO_PC2FSA_READ           = 0,
    RMAIO_PC2FSA_WRITE_ONLY     = 1,
    RMAIO_PC2FSA_WRITE_FEEDBACK = 2,
    RMAIO_PC2FSA_ILLEGAL_ACCESS = 3,

    RMAIO_PC2FSA_CMD_32BIT = INT32_MAX, // 确保返回值为32位,防止编译器自作主张
};

#pragma pack(push, 1)
struct rmaio_pc2fsa_frame_head {
    uint32_t version          : 4;
    uint32_t type             : 4;
    uint32_t encrypt          : 3;
    uint32_t atomic_wait_time : 3;
    uint32_t direct_write     : 1;
    uint32_t no_response      : 1;
    uint32_t cnt              : 16;
};

struct rmaio_pc2fsa_data_head {
    uint16_t cmd  : 2;
    uint16_t len  : 3;
    uint16_t addr : 11;
};
#pragma pack(pop)

void rmaio_pc2fsa_generate_frame_head(struct rmaio_pc2fsa  *rmaio_pc2fsa,
                                      uint8_t               version,
                                      uint8_t               type,
                                      uint8_t               encrypt,
                                      enum atomic_wait_time atomic_wait_time,
                                      uint8_t               direct_write,
                                      uint8_t               no_response,
                                      uint16_t              cnt);

void rmaio_pc2fsa_add_read(struct rmaio_pc2fsa *rmaio_pc2fsa, uint16_t addr, uint8_t len);
void rmaio_pc2fsa_add_write_only(struct rmaio_pc2fsa *rmaio_pc2fsa,
                                 uint16_t             addr,
                                 uint32_t            *data,
                                 uint8_t              len);
void rmaio_pc2fsa_add_write_feedback(struct rmaio_pc2fsa *rmaio_pc2fsa,
                                     uint16_t             addr,
                                     uint32_t            *data,
                                     uint8_t              len);
void rmaio_pc2fsa_print(struct rmaio_pc2fsa *rmaio_pc2fsa);

/*---------------------------------- FSA -> PC ----------------------------------*/
struct rmaio_fsa2pc {
    uint8_t  recv_buf[FSA_RMAIO_BUF_SIZE]; // 必须为 uint8_t[1400]
    int      recv_len;
    int      parse_index;
    uint32_t fsa_mem_in_pc[2048]; // 必须为 uint32_t[2048]
};

enum rmaio_fsa2pc_cmd {
    RMAIO_FSA2PC_READ           = 0,
    RMAIO_FSA2PC_WRITE_ONLY     = 1,
    RMAIO_FSA2PC_WRITE_FEEDBACK = 2,
    RMAIO_FSA2PC_ILLEGAL_ACCESS = 3,

    RMAIO_FSA2PC_CMD_32BIT = INT32_MAX, // 确保返回值为32位,防止编译器自作主张
};

#pragma pack(push, 1)
struct rmaio_fsa2pc_frame_head {
    uint32_t version          : 4;
    uint32_t type             : 4;
    uint32_t encrypt          : 3;
    uint32_t atomic_wait_time : 3;
    uint32_t direct_write     : 1;
    uint32_t atomic_warning   : 1;
    uint32_t cnt              : 16;
};

struct rmaio_fsa2pc_data_head {
    uint16_t cmd  : 2;
    uint16_t len  : 3;
    uint16_t addr : 11;
};
#pragma pack(pop)

void     rmaio_fsa2pc_parse(struct rmaio_fsa2pc *rmaio_fsa2pc, int recv_len);
void     rmaio_fsa2pc_print(struct rmaio_fsa2pc *rmaio_fsa2pc);
uint32_t rmaio_fsa2pc_read_mem(struct rmaio_fsa2pc *rmaio_fsa2pc, uint16_t addr);
uint16_t rmaio_fsa2pc_get_read_data_head(uint16_t addr, uint8_t len);
uint16_t rmaio_fsa2pc_get_write_feedback_data_head(uint16_t addr);

#ifdef __cplusplus
}
#endif

#endif // FSA_RMAIO_H
