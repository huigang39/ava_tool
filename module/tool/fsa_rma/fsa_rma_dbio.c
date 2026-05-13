#include "fsa_rma_dbio.h"

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int
type2size(rma_dbio_data_type_e type)
{
        switch (type) {
                case I8:
                case U8:
                        return 1;
                case I16:
                case U16:
                        return 2;
                case I32:
                case U32:
                        return 4;
                case I64:
                case U64:
                        return 8;
                case F32:
                        return 4;
                case F64:
                        return 8;
                default:
                        return -1; // 错误类型
        }
}

static uint8_t *
lazy_malloc_4GB()
{
        size_t size = 4ULL * 1024 * 1024 * 1024; // 4GB
        void  *mem  = mmap(NULL,
                           size,
                           PROT_READ | PROT_WRITE, // 读写权限
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                           -1,
                           0);

        if (mem == MAP_FAILED) {
                perror("mmap failed");
                return 0;
        }
}

/*---------------------------------- PC -> FSA
 * ----------------------------------*/
void
rma_dbio_pc2fsa_generate_frame_head(rma_dbio_pc2fsa_t *rma_dbio_pc2fsa,
                                    uint8_t            version,
                                    uint8_t            type,
                                    uint8_t            encrypt,
                                    atomic_wait_time_e atomic_wait_time,
                                    uint8_t            direct_write,
                                    uint8_t            no_response,
                                    uint16_t           cnt)
{
        rma_dbio_pc2fsa->generate_index          = sizeof(rma_dbio_pc2fsa_frame_head_t);
        rma_dbio_pc2fsa_frame_head_t *frame_head = (rma_dbio_pc2fsa_frame_head_t *)rma_dbio_pc2fsa->send_buf;
        frame_head->version                      = version;
        frame_head->type                         = type;
        frame_head->encrypt                      = encrypt;
        frame_head->atomic_wait_time             = atomic_wait_time;
        frame_head->direct_write                 = direct_write;
        frame_head->no_response                  = no_response;
        frame_head->cnt                          = cnt;
}

void
rma_dbio_pc2fsa_add_read(rma_dbio_pc2fsa_t *rma_dbio_pc2fsa, uint32_t addr, rma_dbio_data_type_e type)
{
        rma_dbio_pc2fsa_data_head_t *data_head =
            (rma_dbio_pc2fsa_data_head_t *)(rma_dbio_pc2fsa->send_buf + rma_dbio_pc2fsa->generate_index);
        data_head->cmd                   = RMA_DBIO_PC2FSA_READ;
        data_head->data_type             = type;
        data_head->addr                  = addr;
        rma_dbio_pc2fsa->generate_index += sizeof(rma_dbio_pc2fsa_data_head_t);
}

void
rma_dbio_pc2fsa_add_write_only(rma_dbio_pc2fsa_t *rma_dbio_pc2fsa, uint32_t addr, void *data, rma_dbio_data_type_e type)
{
        rma_dbio_pc2fsa_data_head_t *data_head =
            (rma_dbio_pc2fsa_data_head_t *)(rma_dbio_pc2fsa->send_buf + rma_dbio_pc2fsa->generate_index);
        data_head->cmd                   = RMA_DBIO_PC2FSA_WRITE_ONLY;
        data_head->data_type             = type;
        data_head->addr                  = addr;
        rma_dbio_pc2fsa->generate_index += sizeof(rma_dbio_pc2fsa_data_head_t);
        int size                         = type2size(type);
        if (size < 0) {
                printf("type2size error, %d\n", type);
                return;
        }
        memcpy(rma_dbio_pc2fsa->send_buf + rma_dbio_pc2fsa->generate_index, data, size);
        rma_dbio_pc2fsa->generate_index += size;
}

/*---------------------------------- FSA -> PC
 * ----------------------------------*/
void
rma_dbio_fsa2pc_parse(rma_dbio_fsa2pc_t *rma_dbio_fsa2pc, int recv_len)
{
        if (rma_dbio_fsa2pc->fsa_mem_in_pc == NULL) {
                rma_dbio_fsa2pc->fsa_mem_in_pc = lazy_malloc_4GB();
        }
        rma_dbio_fsa2pc->recv_len = recv_len;
        if (recv_len < sizeof(rma_dbio_fsa2pc_frame_head_t)) {
                printf("frame_head error, %d\n", recv_len);
                return;
        }
        rma_dbio_fsa2pc_frame_head_t *frame_head = (rma_dbio_fsa2pc_frame_head_t *)rma_dbio_fsa2pc->recv_buf;
        if (frame_head->version != 0) {
                printf("version error, %d\n", frame_head->version);
                return;
        }
        if (frame_head->type != 1) {
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
        rma_dbio_fsa2pc->parse_index = sizeof(rma_dbio_fsa2pc_frame_head_t);
        while (rma_dbio_fsa2pc->parse_index < recv_len) {
                if (recv_len - rma_dbio_fsa2pc->parse_index < (int)sizeof(rma_dbio_fsa2pc_data_head_t)) {
                        printf("data_head error, %d\n", recv_len);
                        return;
                }
                rma_dbio_fsa2pc_data_head_t *data_head =
                    (rma_dbio_fsa2pc_data_head_t *)(rma_dbio_fsa2pc->recv_buf + rma_dbio_fsa2pc->parse_index);
                rma_dbio_fsa2pc->parse_index += sizeof(rma_dbio_fsa2pc_data_head_t);
                rma_dbio_fsa2pc_cmd_e cmd     = data_head->cmd;
                rma_dbio_data_type_e  type    = data_head->data_type;
                uint32_t              addr    = data_head->addr;
                switch (cmd) {
                        case RMA_DBIO_FSA2PC_READ: // 读取内存
                        {
                                int size = type2size(type);
                                if (size < 0) {
                                        printf("type2size error, %d\n", type);
                                        return;
                                }
                                memcpy(rma_dbio_fsa2pc->fsa_mem_in_pc + addr,
                                       rma_dbio_fsa2pc->recv_buf + rma_dbio_fsa2pc->parse_index,
                                       size);
                                rma_dbio_fsa2pc->parse_index += size;
                        } break;
                        case RMA_DBIO_FSA2PC_ILLEGAL_ACCESS: // 非法访问
                                printf("illegal access, addr: 0x%04X\n", addr);
                                break;
                        default:
                                break;
                }
        }
}

void *
rma_dbio_fsa2pc_read_mem(rma_dbio_fsa2pc_t *rma_dbio_fsa2pc, uint32_t addr)
{
        if (rma_dbio_fsa2pc->fsa_mem_in_pc == NULL) {
                rma_dbio_fsa2pc->fsa_mem_in_pc = lazy_malloc_4GB();
        }
        return rma_dbio_fsa2pc->fsa_mem_in_pc + addr;
}
