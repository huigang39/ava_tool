#ifndef FSA_RMA_DBIO_H
#define FSA_RMA_DBIO_H

#include "fsa_rmaio.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FSA_RMA_DBIO_BUF_SIZE 1400

typedef enum {
        I8  = 0,
        U8  = 1,
        I16 = 2,
        U16 = 3,
        I32 = 4,
        U32 = 5,
        I64 = 6,
        U64 = 7,
        F32 = 8,
        F64 = 9,
} rma_dbio_data_type_e;

/*---------------------------------- PC -> FSA ----------------------------------*/
typedef struct {
        uint8_t send_buf[FSA_RMA_DBIO_BUF_SIZE]; // must be uint8_t[1400]
        int     parse_index;
        int     generate_index;
} rma_dbio_pc2fsa_t;

typedef enum {
        RMA_DBIO_PC2FSA_READ       = 0,
        RMA_DBIO_PC2FSA_WRITE_ONLY = 1,

        RMA_DBIO_PC2FSA_CMD_32BIT = INT32_MAX, // 确保返回值为32位，防止编译器自作主张
} rma_dbio_pc2fsa_cmd_e;

#pragma pack(push, 1)
typedef struct {
        uint32_t version          : 4;
        uint32_t type             : 4;
        uint32_t encrypt          : 3;
        uint32_t atomic_wait_time : 3;
        uint32_t direct_write     : 1;
        uint32_t no_response      : 1;
        uint32_t cnt              : 16;
} rma_dbio_pc2fsa_frame_head_t;

typedef struct {
        uint8_t  cmd       : 2;
        uint8_t  reserved  : 2;
        uint8_t  data_type : 4;
        uint32_t addr;
} rma_dbio_pc2fsa_data_head_t;
#pragma pack(pop)

void rma_dbio_pc2fsa_generate_frame_head(rma_dbio_pc2fsa_t *rma_dbio_pc2fsa,
                                         uint8_t            version,
                                         uint8_t            type,
                                         uint8_t            encrypt,
                                         atomic_wait_time_e atomic_wait_time,
                                         uint8_t            direct_write,
                                         uint8_t            no_response,
                                         uint16_t           cnt);

void rma_dbio_pc2fsa_add_read(rma_dbio_pc2fsa_t *rma_dbio_pc2fsa, uint32_t addr, rma_dbio_data_type_e type);
void rma_dbio_pc2fsa_add_write_only(rma_dbio_pc2fsa_t *rma_dbio_pc2fsa, uint32_t addr, void *data, rma_dbio_data_type_e type);
// void rma_dbio_pc2fsa_print( rma_dbio_pc2fsa_t* rma_dbio_pc2fsa );

/*---------------------------------- FSA -> PC ----------------------------------*/
typedef struct {
        uint8_t  recv_buf[FSA_RMA_DBIO_BUF_SIZE]; // must be uint8_t[1400]
        int      recv_len;
        int      parse_index;
        uint8_t *fsa_mem_in_pc; // 4GB 内存映射到PC端，指向MCU的内存空间
} rma_dbio_fsa2pc_t;

typedef enum {
        RMA_DBIO_FSA2PC_READ           = 0,
        RMA_DBIO_FSA2PC_ILLEGAL_ACCESS = 3,

        RMA_DBIO_FSA2PC_CMD_32BIT = INT32_MAX, // 确保返回值为32位，防止编译器自作主张
} rma_dbio_fsa2pc_cmd_e;

#pragma pack(push, 1)
typedef struct {
        uint32_t version          : 4;
        uint32_t type             : 4;
        uint32_t encrypt          : 3;
        uint32_t atomic_wait_time : 3;
        uint32_t direct_write     : 1;
        uint32_t atomic_warning   : 1;
        uint32_t cnt              : 16;
} rma_dbio_fsa2pc_frame_head_t;

typedef struct {
        uint8_t  cmd       : 2;
        uint8_t  reserved  : 2;
        uint8_t  data_type : 4;
        uint32_t addr;
} rma_dbio_fsa2pc_data_head_t;
#pragma pack(pop)

void  rma_dbio_fsa2pc_parse(rma_dbio_fsa2pc_t *rma_dbio_fsa2pc, int recv_len);
void *rma_dbio_fsa2pc_read_mem(rma_dbio_fsa2pc_t *rma_dbio_fsa2pc, uint32_t addr);
// void  rma_dbio_fsa2pc_print( rma_dbio_fsa2pc_t* rma_dbio_fsa2pc );

#ifdef __cplusplus
}
#endif

#endif // FSA_RMA_DBIO_H
