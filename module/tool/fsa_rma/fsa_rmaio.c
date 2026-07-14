#include <stdio.h>

#include "fsa_rmaio.h"

/*---------------------------------- PC -> FSA
 * ----------------------------------*/
void
rmaio_pc2fsa_generate_frame_head(rmaio_pc2fsa_t    *rmaio_pc2fsa,
                                 uint8_t            version,
                                 uint8_t            type,
                                 uint8_t            encrypt,
                                 atomic_wait_time_e atomic_wait_time,
                                 uint8_t            direct_write,
                                 uint8_t            no_response,
                                 uint16_t           cnt)
{
        rmaio_pc2fsa->generate_index          = sizeof(rmaio_pc2fsa_frame_head_t);
        rmaio_pc2fsa_frame_head_t *frame_head = (rmaio_pc2fsa_frame_head_t *)rmaio_pc2fsa->send_buf;
        frame_head->version                   = version;
        frame_head->type                      = type;
        frame_head->encrypt                   = encrypt;
        frame_head->atomic_wait_time          = atomic_wait_time;
        frame_head->direct_write              = direct_write;
        frame_head->no_response               = no_response;
        frame_head->cnt                       = cnt;
}

void
rmaio_pc2fsa_add_read(rmaio_pc2fsa_t *rmaio_pc2fsa, uint16_t addr, uint8_t len)
{
        rmaio_pc2fsa_data_head_t *data_head =
            (rmaio_pc2fsa_data_head_t *)(rmaio_pc2fsa->send_buf + rmaio_pc2fsa->generate_index);
        data_head->cmd                = RMAIO_PC2FSA_READ;
        data_head->len                = len - 1;
        data_head->addr               = addr;
        rmaio_pc2fsa->generate_index += sizeof(rmaio_pc2fsa_data_head_t);
}

void
rmaio_pc2fsa_add_write_only(rmaio_pc2fsa_t *rmaio_pc2fsa, uint16_t addr, uint32_t *data, uint8_t len)
{
        rmaio_pc2fsa_data_head_t *data_head =
            (rmaio_pc2fsa_data_head_t *)(rmaio_pc2fsa->send_buf + rmaio_pc2fsa->generate_index);
        data_head->cmd                = RMAIO_PC2FSA_WRITE_ONLY;
        data_head->len                = len - 1;
        data_head->addr               = addr;
        rmaio_pc2fsa->generate_index += sizeof(rmaio_pc2fsa_data_head_t);
        for (int i = 0; i < len; i++) {
                *(uint32_t *)(rmaio_pc2fsa->send_buf + rmaio_pc2fsa->generate_index)  = data[i];
                rmaio_pc2fsa->generate_index                                         += sizeof(uint32_t);
        }
}

void
rmaio_pc2fsa_add_write_feedback(rmaio_pc2fsa_t *rmaio_pc2fsa, uint16_t addr, uint32_t *data, uint8_t len)
{
        rmaio_pc2fsa_data_head_t *data_head =
            (rmaio_pc2fsa_data_head_t *)(rmaio_pc2fsa->send_buf + rmaio_pc2fsa->generate_index);
        data_head->cmd                = RMAIO_PC2FSA_WRITE_FEEDBACK;
        data_head->len                = len - 1;
        data_head->addr               = addr;
        rmaio_pc2fsa->generate_index += sizeof(rmaio_pc2fsa_data_head_t);
        for (int i = 0; i < len; i++) {
                *(uint32_t *)(rmaio_pc2fsa->send_buf + rmaio_pc2fsa->generate_index)  = data[i];
                rmaio_pc2fsa->generate_index                                         += sizeof(uint32_t);
        }
}

void
rmaio_pc2fsa_print(rmaio_pc2fsa_t *rmaio_pc2fsa)
{
        printf("send_data: ");
        for (int i = 0; i < rmaio_pc2fsa->generate_index; i++) {
                printf("%02X ", rmaio_pc2fsa->send_buf[i]);
        }
        printf("\nsend_len: %d\n", rmaio_pc2fsa->generate_index);
        rmaio_pc2fsa_frame_head_t *frame_head = (rmaio_pc2fsa_frame_head_t *)rmaio_pc2fsa->send_buf;
        printf("  frame_head: 0x%08X\n", *(uint32_t *)frame_head);
        printf("    frame_head->version: %d\n", frame_head->version);
        printf("    frame_head->type: %d\n", frame_head->type);
        printf("    frame_head->encrypt: %d\n", frame_head->encrypt);
        printf("    frame_head->atomic_wait_time: %d\n", frame_head->atomic_wait_time);
        printf("    frame_head->direct_write: %d\n", frame_head->direct_write);
        printf("    frame_head->no_response: %d\n", frame_head->no_response);
        printf("    frame_head->cnt: %d\n", frame_head->cnt);
        uint16_t parse_index = sizeof(rmaio_pc2fsa_frame_head_t);
        while (parse_index < rmaio_pc2fsa->generate_index) {
                if (rmaio_pc2fsa->generate_index - parse_index < (int)sizeof(rmaio_pc2fsa_data_head_t)) {
                        printf("Body error, %d\n", rmaio_pc2fsa->generate_index);
                        return;
                }
                rmaio_pc2fsa_data_head_t *data_head  = (rmaio_pc2fsa_data_head_t *)(rmaio_pc2fsa->send_buf + parse_index);
                parse_index                         += sizeof(rmaio_pc2fsa_data_head_t);
                uint16_t cmd                         = data_head->cmd;
                uint16_t len                         = data_head->len;
                uint16_t addr                        = data_head->addr;
                printf("  data_head: 0x%04X", *(uint16_t *)data_head);
                if (cmd != 0) {
                        for (int i = 0; i <= len; i++) {
                                printf(" 0x%08X", *(uint32_t *)(rmaio_pc2fsa->send_buf + parse_index + i * sizeof(uint32_t)));
                        }
                }
                printf("\n  data_head: cmd: %d, len: %d, addr: 0x%04X\n", cmd, len + 1, addr);
                switch (cmd) {
                        case RMAIO_PC2FSA_READ: // 读取内存
                        {
                                for (int i = 0; i <= len; i++) {
                                        printf("    read addr: 0x%04X\n", addr + i);
                                }
                        } break;
                        case RMAIO_PC2FSA_WRITE_ONLY: // 仅写入内存
                        {
                                for (int i = 0; i <= len; i++) {
                                        printf("    write only addr: 0x%04X, data: "
                                               "0x%8x\n",
                                               addr + i,
                                               *(uint32_t *)(rmaio_pc2fsa->send_buf + parse_index));
                                        parse_index += sizeof(uint32_t);
                                }
                        } break;
                        case RMAIO_PC2FSA_WRITE_FEEDBACK: // 写入内存+反馈
                        {
                                for (int i = 0; i <= len; i++) {
                                        printf("    write feedback addr: 0x%04X, data: "
                                               "0x%8x\n",
                                               addr + i,
                                               *(uint32_t *)(rmaio_pc2fsa->send_buf + parse_index));
                                        parse_index += sizeof(uint32_t);
                                }
                        } break;
                        case RMAIO_PC2FSA_ILLEGAL_ACCESS: // 非法访问，忽略
                                break;
                        default:
                                break;
                }
        }
        printf("pc2fsa send print end\n\n");
}

/*---------------------------------- FSA -> PC
 * ----------------------------------*/
void
rmaio_fsa2pc_parse(rmaio_fsa2pc_t *rmaio_fsa2pc, int recv_len)
{
        rmaio_fsa2pc->recv_len = recv_len;
        if (recv_len < sizeof(rmaio_fsa2pc_frame_head_t)) {
                printf("frame_head error, %d\n", recv_len);
                return;
        }
        rmaio_fsa2pc_frame_head_t *frame_head = (rmaio_fsa2pc_frame_head_t *)rmaio_fsa2pc->recv_buf;
        if (frame_head->version != 0) {
                printf("version error, %d\n", frame_head->version);
                return;
        }
        if (frame_head->type != 0) {
                printf("type error, %d\n", frame_head->type);
                return;
        }
        if (frame_head->encrypt != 0) {
                printf("encrypt error, %d\n", frame_head->encrypt);
                return;
        }
        if (frame_head->atomic_warning != 0) {
                printf("atomic_wait_time warning!\n");
        }
        rmaio_fsa2pc->parse_index = sizeof(rmaio_fsa2pc_frame_head_t);
        while (rmaio_fsa2pc->parse_index < recv_len) {
                if (recv_len - rmaio_fsa2pc->parse_index < (int)sizeof(rmaio_fsa2pc_data_head_t)) {
                        printf("data_head error, %d\n", recv_len);
                        return;
                }
                rmaio_fsa2pc_data_head_t *data_head =
                    (rmaio_fsa2pc_data_head_t *)(rmaio_fsa2pc->recv_buf + rmaio_fsa2pc->parse_index);
                rmaio_fsa2pc->parse_index += sizeof(rmaio_fsa2pc_data_head_t);
                uint16_t cmd               = data_head->cmd;
                uint16_t len               = data_head->len;
                uint16_t addr              = data_head->addr;
                switch (cmd) {
                        case RMAIO_PC2FSA_READ: // 读取内存
                        {
                                for (int i = 0; i <= len; i++) {
                                        rmaio_fsa2pc->fsa_mem_in_pc[addr + i] =
                                            *(uint32_t *)(rmaio_fsa2pc->recv_buf + rmaio_fsa2pc->parse_index);
                                        rmaio_fsa2pc->parse_index += sizeof(uint32_t);
                                }
                        } break;
                        case RMAIO_PC2FSA_WRITE_ONLY: // 仅写入内存
                        {
                                printf("    write only addr: 0x%04X\n", addr);
                        } break;
                        case RMAIO_PC2FSA_WRITE_FEEDBACK: // 写入内存+反馈
                        {
                                rmaio_fsa2pc->parse_index += sizeof(uint32_t);
                        } break;
                        case RMAIO_PC2FSA_ILLEGAL_ACCESS: // 非法访问
                        {
                                printf("    illegal access addr: 0x%04X\n", addr);
                                rmaio_fsa2pc->parse_index += sizeof(uint32_t);
                        } break;
                        default:
                                break;
                }
        }
}

void
rmaio_fsa2pc_print(rmaio_fsa2pc_t *rmaio_fsa2pc)
{
        printf("recv_data: ");
        for (int i = 0; i < rmaio_fsa2pc->parse_index; i++) {
                printf("%02X ", rmaio_fsa2pc->recv_buf[i]);
        }
        printf("\nrecv_len: %d\n", rmaio_fsa2pc->recv_len);
        rmaio_fsa2pc_frame_head_t *frame_head = (rmaio_fsa2pc_frame_head_t *)rmaio_fsa2pc->recv_buf;
        printf("  frame_head: 0x%08X\n", *(uint32_t *)frame_head);
        printf("    frame_head->version: %d\n", frame_head->version);
        printf("    frame_head->type: %d\n", frame_head->type);
        printf("    frame_head->encrypt: %d\n", frame_head->encrypt);
        printf("    frame_head->atomic_wait_time: %d\n", frame_head->atomic_wait_time);
        printf("    frame_head->direct_write: %d\n", frame_head->direct_write);
        printf("    frame_head->atomic_warning: %d\n", frame_head->atomic_warning);
        printf("    frame_head->cnt: %d\n", frame_head->cnt);
        rmaio_fsa2pc->parse_index = sizeof(rmaio_fsa2pc_frame_head_t);
        while (rmaio_fsa2pc->parse_index < rmaio_fsa2pc->recv_len) {
                if (rmaio_fsa2pc->recv_len - rmaio_fsa2pc->parse_index < sizeof(rmaio_fsa2pc_data_head_t)) {
                        printf("data_head error, %d\n", rmaio_fsa2pc->recv_len);
                        return;
                }
                rmaio_fsa2pc_data_head_t *data_head =
                    (rmaio_fsa2pc_data_head_t *)(rmaio_fsa2pc->recv_buf + rmaio_fsa2pc->parse_index);
                rmaio_fsa2pc->parse_index += sizeof(rmaio_fsa2pc_data_head_t);
                uint16_t cmd               = data_head->cmd;
                uint16_t len               = data_head->len;
                uint16_t addr              = data_head->addr;
                printf("  data_head: 0x%04X", *(uint16_t *)data_head);
                if (cmd != 0) {
                        for (int i = 0; i <= len; i++) {
                                printf(
                                    " 0x%08X",
                                    *(uint32_t *)(rmaio_fsa2pc->recv_buf + rmaio_fsa2pc->parse_index + i * sizeof(uint32_t)));
                        }
                }
                printf("\n  data_head: cmd: %d, len: %d, addr: 0x%04X\n", cmd, len + 1, addr);
                switch (cmd) {
                        case RMAIO_PC2FSA_READ: // 读取内存
                        {
                                for (int i = 0; i <= len; i++) {
                                        printf("    read addr: 0x%04X, data: 0x%8x, "
                                               "data_fp32:%f\n",
                                               addr + i,
                                               *(uint32_t *)(rmaio_fsa2pc->recv_buf + rmaio_fsa2pc->parse_index),
                                               u32_to_fp32(*(uint32_t *)(rmaio_fsa2pc->recv_buf + rmaio_fsa2pc->parse_index)));
                                        rmaio_fsa2pc->parse_index += sizeof(uint32_t);
                                }
                        } break;
                        case RMAIO_PC2FSA_WRITE_ONLY:     // 仅写入内存
                        case RMAIO_PC2FSA_WRITE_FEEDBACK: // 写入内存+反馈
                        case RMAIO_PC2FSA_ILLEGAL_ACCESS: // 非法访问
                        {
                                uint32_t feedback_data     = *(uint32_t *)(rmaio_fsa2pc->recv_buf + rmaio_fsa2pc->parse_index);
                                rmaio_fsa2pc->parse_index += sizeof(uint32_t);
                                for (int i = 0; i < 8; i++) {
                                        uint16_t feedback_addr   = addr + i;
                                        uint8_t  feedback_status = (feedback_data >> (i * 4)) & 0xF;
                                        if (feedback_status == 0) {
                                                printf("    write feedback addr: 0x%04X, "
                                                       "status: 0x%1x SUCCESS\n",
                                                       feedback_addr,
                                                       feedback_status);
                                        } else if (feedback_status == 0xF) {
                                                printf("    write feedback addr: 0x%04X, "
                                                       "status: 0x%1x PC NOT WRITE\n",
                                                       feedback_addr,
                                                       feedback_status);
                                        } else {
                                                printf("    write feedback addr: "
                                                       "0x%04X, status: 0x%1x ERROR\n",
                                                       feedback_addr,
                                                       feedback_status);
                                        }
                                }
                        }
                        default:
                                break;
                }
        }
        printf("fsa2pc recv print end\n\n");
}

uint32_t
rmaio_fsa2pc_read_mem(rmaio_fsa2pc_t *rmaio_fsa2pc, uint16_t addr)
{
        return rmaio_fsa2pc->fsa_mem_in_pc[addr];
}
uint16_t
rmaio_fsa2pc_get_read_data_head(uint16_t addr, uint8_t len)
{
        rmaio_fsa2pc_data_head_t data_head;
        data_head.cmd  = RMAIO_PC2FSA_READ;
        data_head.len  = len - 1;
        data_head.addr = addr;
        return *(uint16_t *)&data_head;
}
uint16_t
rmaio_fsa2pc_get_write_feedback_data_head(uint16_t addr)
{
        rmaio_fsa2pc_data_head_t data_head;
        data_head.cmd  = RMAIO_PC2FSA_WRITE_FEEDBACK;
        data_head.len  = 0;
        data_head.addr = addr;
        return *(uint16_t *)&data_head;
}
