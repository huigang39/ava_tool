#ifndef FSA_RMA_SUBS_H
#define FSA_RMA_SUBS_H

#include "fsa_rmaio.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FSA_RMA_SUBS_BUF_SIZE 1400
/*---------------------------------- PC -> FSA ----------------------------------*/
typedef struct {
        uint64_t keepalive_time_ns;      // 保持订阅时间，单位纳秒
        uint64_t last_keepalive_time_ns; // 上次保持订阅时间，单位纳秒
        uint8_t  enable_mem_map[2048];   // must be uint8_t[2048]
} rma_subs_pc2fsa_t;

typedef struct {
        volatile uint16_t reserved : 2;  // 保留位
        volatile uint16_t len      : 3;  // 订阅长度
        volatile uint16_t addr     : 11; // RMA地址
        volatile uint16_t freq_Hz;       // 订阅频率，单位Hz
} rma_subs_item_t;

/*---------------------------------- FSA -> PC ----------------------------------*/
typedef struct {
        uint8_t  recv_buf[FSA_RMA_SUBS_BUF_SIZE]; // must be uint8_t[1400]
        uint16_t cnt;                             // 订阅计数
        uint16_t cnt_last;                        // 上次订阅计数
        int      recv_len;
        int      parse_index;
        uint32_t fsa_mem_in_pc[2048]; // must be uint32_t[2048]
} rma_subs_fsa2pc_t;

#pragma pack(push, 1)
typedef struct {
        uint32_t version  : 4;
        uint32_t type     : 4;
        uint32_t encrypt  : 3;
        uint32_t reserved : 5;
        uint32_t cnt      : 16;
} rma_subs_fsa2pc_frame_head_t;

typedef struct {
        uint16_t ret  : 2;
        uint16_t len  : 3;
        uint16_t addr : 11;
} rma_subs_fsa2pc_data_head_t;
#pragma pack(pop)

void     rma_subs_fsa2pc_parse(rma_subs_fsa2pc_t *rma_subs_fsa2pc, int recv_len);
uint32_t rma_subs_fsa2pc_read_mem(rma_subs_fsa2pc_t *rma_subs_fsa2pc, uint16_t addr);
// void  rma_subs_fsa2pc_print( rma_subs_fsa2pc_t* rma_subs_fsa2pc );

#ifdef __cplusplus
}
#endif

#endif // FSA_RMA_SUBS_H
