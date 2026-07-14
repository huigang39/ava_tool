#include "fsa_rma_subs.h"

#include <stdio.h>
#include <string.h>

/*---------------------------------- FSA -> PC
 * ----------------------------------*/
void
rma_subs_fsa2pc_parse(rma_subs_fsa2pc_t *rma_subs_fsa2pc, int recv_len)
{
        rma_subs_fsa2pc->recv_len = recv_len;
        if (recv_len < sizeof(rma_subs_fsa2pc_frame_head_t)) {
                printf("frame_head error, %d\n", recv_len);
                return;
        }
        // 解析帧头
        rma_subs_fsa2pc_frame_head_t *frame_head = (rma_subs_fsa2pc_frame_head_t *)rma_subs_fsa2pc->recv_buf;
        if (frame_head->version != 0) {
                printf("version error, %d\n", frame_head->version);
                return;
        }
        if (frame_head->type != 3) {
                printf("type error, %d\n", frame_head->type);
                return;
        }
        if (frame_head->encrypt != 0) {
                printf("encrypt error, %d\n", frame_head->encrypt);
                return;
        }
        rma_subs_fsa2pc->cnt         = frame_head->cnt;
        rma_subs_fsa2pc->parse_index = sizeof(rma_subs_fsa2pc_frame_head_t);
        // 解析数据
        while (rma_subs_fsa2pc->parse_index < recv_len) {
                if (recv_len - rma_subs_fsa2pc->parse_index < (int)sizeof(rma_subs_fsa2pc_data_head_t)) {
                        printf("data_head error, %d\n", recv_len);
                        return;
                }
                rma_subs_fsa2pc_data_head_t *data_head =
                    (rma_subs_fsa2pc_data_head_t *)(rma_subs_fsa2pc->recv_buf + rma_subs_fsa2pc->parse_index);
                rma_subs_fsa2pc->parse_index += sizeof(rma_subs_fsa2pc_data_head_t);
                uint8_t  ret                  = data_head->ret;
                uint8_t  len                  = data_head->len;
                uint16_t addr                 = data_head->addr;
                for (int i = 0; i <= len; i++) {
                        rma_subs_fsa2pc->fsa_mem_in_pc[addr + i] =
                            *(uint32_t *)(rma_subs_fsa2pc->recv_buf + rma_subs_fsa2pc->parse_index);
                        rma_subs_fsa2pc->parse_index += sizeof(uint32_t);
                }
        }
        return;
}

uint32_t
rma_subs_fsa2pc_read_mem(rma_subs_fsa2pc_t *rma_subs_fsa2pc, uint16_t addr)
{
        return rma_subs_fsa2pc->fsa_mem_in_pc[addr];
}
