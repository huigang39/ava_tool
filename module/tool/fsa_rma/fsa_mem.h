#ifndef FSA_MEM_H
#define FSA_MEM_H

#include <stdint.h>

#include "pdo_freerun_shm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FSA_MEM_COE_INDEX        0x3000u
#define FSA_MEM_COE_SUB_ADDRESS  0x01u
#define FSA_MEM_COE_SUB_LENGTH   0x02u
#define FSA_MEM_COE_SUB_COMMAND  0x03u
#define FSA_MEM_COE_SUB_STATUS   0x04u
#define FSA_MEM_COE_SUB_DATA     0x05u
#define FSA_MEM_COE_SUB_WR_COUNT 0x06u

#define FSA_MEM_CHUNK_BYTES      128u

#define FSA_MEM_CMD_NONE         0u
#define FSA_MEM_CMD_READ         1u
#define FSA_MEM_CMD_WRITE        2u

#define FSA_MEM_STATUS_IDLE      0u
#define FSA_MEM_STATUS_OK        1u
#define FSA_MEM_STATUS_BAD_CMD   2u
#define FSA_MEM_STATUS_BAD_ADDR  3u
#define FSA_MEM_STATUS_BAD_LEN   4u
#define FSA_MEM_STATUS_UNALIGNED 5u
#define FSA_MEM_STATUS_TIMEOUT   6u

typedef int (*fsa_mem_sdo_read_fn)(void *user, uint16_t index, uint8_t subindex, void *data, uint32_t size);
typedef int (*fsa_mem_sdo_write_fn)(void *user, uint16_t index, uint8_t subindex, const void *data, uint32_t size);

typedef struct {
        fsa_mem_sdo_read_fn  read;
        fsa_mem_sdo_write_fn write;
        void                *user;
} fsa_mem_ecat_t;

typedef enum {
        FSA_MEM_OK          = 0,
        FSA_MEM_ERR_ARG     = -1,
        FSA_MEM_ERR_ALIGN   = -2,
        FSA_MEM_ERR_SDO     = -3,
        FSA_MEM_ERR_DEVICE  = -4,
        FSA_MEM_ERR_TIMEOUT = -5,
} fsa_mem_result_e;

int fsa_mem_ecat_read(fsa_mem_ecat_t *ecat, uint32_t addr, void *dst, uint32_t bytes);
int fsa_mem_ecat_write(fsa_mem_ecat_t *ecat, uint32_t addr, const void *src, uint32_t bytes);
int fsa_mem_ecat_read_u32(fsa_mem_ecat_t *ecat, uint32_t addr, uint32_t *value);
int fsa_mem_ecat_write_u32(fsa_mem_ecat_t *ecat, uint32_t addr, uint32_t value);
typedef int (*fsa_mem_pdo_wait_fn)(void *user, uint32_t seq);

typedef struct {
        volatile fsa_mem_pdo_req_t       *req;
        volatile const fsa_mem_pdo_res_t *res;
        uint32_t                          seq;
        fsa_mem_pdo_wait_fn               wait;
        void                             *wait_user;
} fsa_mem_pdo_t;

int fsa_mem_pdo_init(fsa_mem_pdo_t      *pdo,
                     void               *output_pdo,
                     uint32_t            output_size,
                     const void         *input_pdo,
                     uint32_t            input_size,
                     fsa_mem_pdo_wait_fn wait,
                     void               *wait_user);
int fsa_mem_pdo_read(fsa_mem_pdo_t *pdo, uint32_t addr, void *dst, uint32_t bytes, uint32_t max_cycles);
int fsa_mem_pdo_write(fsa_mem_pdo_t *pdo, uint32_t addr, const void *src, uint32_t bytes, uint32_t max_cycles);
int fsa_mem_pdo_read_u32(fsa_mem_pdo_t *pdo, uint32_t addr, uint32_t *value, uint32_t max_cycles);
int fsa_mem_pdo_write_u32(fsa_mem_pdo_t *pdo, uint32_t addr, uint32_t value, uint32_t max_cycles);

#ifdef __cplusplus
}
#endif

#endif /* FSA_MEM_H */
