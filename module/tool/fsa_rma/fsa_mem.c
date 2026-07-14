#include "fsa_mem.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static int
fsa_mem_check_args(fsa_mem_ecat_t *ecat, const void *buf, uint32_t addr, uint32_t bytes)
{
        if ((ecat == NULL) || (ecat->read == NULL) || (ecat->write == NULL)) {
                return FSA_MEM_ERR_ARG;
        }

        if ((buf == NULL) && (bytes != 0u)) {
                return FSA_MEM_ERR_ARG;
        }

        if (((addr | bytes) & 0x3u) != 0u) {
                return FSA_MEM_ERR_ALIGN;
        }

        return FSA_MEM_OK;
}

static int
fsa_mem_sdo_read(fsa_mem_ecat_t *ecat, uint8_t subindex, void *data, uint32_t size)
{
        return ecat->read(ecat->user, FSA_MEM_COE_INDEX, subindex, data, size) == 0 ? FSA_MEM_OK : FSA_MEM_ERR_SDO;
}

static int
fsa_mem_sdo_write(fsa_mem_ecat_t *ecat, uint8_t subindex, const void *data, uint32_t size)
{
        return ecat->write(ecat->user, FSA_MEM_COE_INDEX, subindex, data, size) == 0 ? FSA_MEM_OK : FSA_MEM_ERR_SDO;
}

static int
fsa_mem_set_window(fsa_mem_ecat_t *ecat, uint32_t addr, uint16_t bytes)
{
        int ret;

        ret = fsa_mem_sdo_write(ecat, FSA_MEM_COE_SUB_ADDRESS, &addr, sizeof(addr));
        if (ret != FSA_MEM_OK) {
                return ret;
        }

        ret = fsa_mem_sdo_write(ecat, FSA_MEM_COE_SUB_LENGTH, &bytes, sizeof(bytes));
        if (ret != FSA_MEM_OK) {
                return ret;
        }

        return FSA_MEM_OK;
}

static int
fsa_mem_command(fsa_mem_ecat_t *ecat, uint8_t command)
{
        uint8_t status = FSA_MEM_STATUS_IDLE;
        int     ret;

        ret = fsa_mem_sdo_write(ecat, FSA_MEM_COE_SUB_COMMAND, &command, sizeof(command));
        if (ret != FSA_MEM_OK) {
                return ret;
        }

        ret = fsa_mem_sdo_read(ecat, FSA_MEM_COE_SUB_STATUS, &status, sizeof(status));
        if (ret != FSA_MEM_OK) {
                return ret;
        }

        return status == FSA_MEM_STATUS_OK ? FSA_MEM_OK : FSA_MEM_ERR_DEVICE;
}

int
fsa_mem_ecat_read(fsa_mem_ecat_t *ecat, uint32_t addr, void *dst, uint32_t bytes)
{
        uint8_t *out = (uint8_t *)dst;
        int      ret;

        ret = fsa_mem_check_args(ecat, dst, addr, bytes);
        if (ret != FSA_MEM_OK) {
                return ret;
        }

        while (bytes > 0u) {
                uint16_t chunk = bytes > FSA_MEM_CHUNK_BYTES ? FSA_MEM_CHUNK_BYTES : (uint16_t)bytes;

                ret = fsa_mem_set_window(ecat, addr, chunk);
                if (ret != FSA_MEM_OK) {
                        return ret;
                }

                ret = fsa_mem_command(ecat, FSA_MEM_CMD_READ);
                if (ret != FSA_MEM_OK) {
                        return ret;
                }

                ret = fsa_mem_sdo_read(ecat, FSA_MEM_COE_SUB_DATA, out, chunk);
                if (ret != FSA_MEM_OK) {
                        return ret;
                }

                out   += chunk;
                addr  += chunk;
                bytes -= chunk;
        }

        return FSA_MEM_OK;
}

int
fsa_mem_ecat_write(fsa_mem_ecat_t *ecat, uint32_t addr, const void *src, uint32_t bytes)
{
        const uint8_t *in = (const uint8_t *)src;
        int            ret;

        ret = fsa_mem_check_args(ecat, src, addr, bytes);
        if (ret != FSA_MEM_OK) {
                return ret;
        }

        while (bytes > 0u) {
                uint16_t chunk = bytes > FSA_MEM_CHUNK_BYTES ? FSA_MEM_CHUNK_BYTES : (uint16_t)bytes;

                ret = fsa_mem_set_window(ecat, addr, chunk);
                if (ret != FSA_MEM_OK) {
                        return ret;
                }

                ret = fsa_mem_sdo_write(ecat, FSA_MEM_COE_SUB_DATA, in, chunk);
                if (ret != FSA_MEM_OK) {
                        return ret;
                }

                ret = fsa_mem_command(ecat, FSA_MEM_CMD_WRITE);
                if (ret != FSA_MEM_OK) {
                        return ret;
                }

                in    += chunk;
                addr  += chunk;
                bytes -= chunk;
        }

        return FSA_MEM_OK;
}

int
fsa_mem_ecat_read_u32(fsa_mem_ecat_t *ecat, uint32_t addr, uint32_t *value)
{
        return fsa_mem_ecat_read(ecat, addr, value, sizeof(*value));
}

int
fsa_mem_ecat_write_u32(fsa_mem_ecat_t *ecat, uint32_t addr, uint32_t value)
{
        return fsa_mem_ecat_write(ecat, addr, &value, sizeof(value));
}

static int
fsa_mem_pdo_check_args(fsa_mem_pdo_t *pdo, const void *buf, uint32_t addr, uint32_t bytes)
{
        if ((pdo == NULL) || (pdo->req == NULL) || (pdo->res == NULL)) {
                return FSA_MEM_ERR_ARG;
        }

        if ((buf == NULL) && (bytes != 0u)) {
                return FSA_MEM_ERR_ARG;
        }

        if (((addr | bytes) & 0x3u) != 0u) {
                return FSA_MEM_ERR_ALIGN;
        }

        return FSA_MEM_OK;
}

static int
fsa_mem_pdo_wait_ack(fsa_mem_pdo_t *pdo, uint32_t seq, uint32_t max_cycles)
{
        uint32_t i;

        for (i = 0u; i <= max_cycles; i++) {
                if (pdo->res->ack == seq) {
                        return pdo->res->status == FSA_MEM_STATUS_OK ? FSA_MEM_OK : FSA_MEM_ERR_DEVICE;
                }

                if (pdo->wait != NULL) {
                        int ret = pdo->wait(pdo->wait_user, seq);
                        if (ret != 0) {
                                return ret;
                        }
                }
        }

        return FSA_MEM_ERR_TIMEOUT;
}

int
fsa_mem_pdo_init(fsa_mem_pdo_t      *pdo,
                 void               *output_pdo,
                 uint32_t            output_size,
                 const void         *input_pdo,
                 uint32_t            input_size,
                 fsa_mem_pdo_wait_fn wait,
                 void               *wait_user)
{
        fsa_neo_rx_pdo_ext_t       *rx;
        const fsa_neo_tx_pdo_ext_t *tx;

        if ((pdo == NULL) || (output_pdo == NULL) || (input_pdo == NULL)) {
                return FSA_MEM_ERR_ARG;
        }

        if ((output_size < FSA_NEO_RX_PDO_SIZE) || (input_size < FSA_NEO_TX_PDO_SIZE)) {
                return FSA_MEM_ERR_ARG;
        }

        rx = (fsa_neo_rx_pdo_ext_t *)output_pdo;
        tx = (const fsa_neo_tx_pdo_ext_t *)input_pdo;

        pdo->req       = &rx->mem_debug;
        pdo->res       = &tx->mem_debug;
        pdo->seq       = tx->mem_debug.ack;
        pdo->wait      = wait;
        pdo->wait_user = wait_user;

        return FSA_MEM_OK;
}

int
fsa_mem_pdo_read(fsa_mem_pdo_t *pdo, uint32_t addr, void *dst, uint32_t bytes, uint32_t max_cycles)
{
        uint8_t *out = (uint8_t *)dst;
        int      ret;

        ret = fsa_mem_pdo_check_args(pdo, dst, addr, bytes);
        if (ret != FSA_MEM_OK) {
                return ret;
        }

        while (bytes > 0u) {
                uint16_t chunk = bytes > FSA_MEM_CHUNK_BYTES ? FSA_MEM_CHUNK_BYTES : (uint16_t)bytes;
                uint32_t seq   = ++pdo->seq;

                pdo->req->address  = addr;
                pdo->req->length   = chunk;
                pdo->req->command  = FSA_MEM_CMD_READ;
                pdo->req->reserved = 0u;
                pdo->req->seq      = seq;

                ret = fsa_mem_pdo_wait_ack(pdo, seq, max_cycles);
                if (ret != FSA_MEM_OK) {
                        return ret;
                }

                memcpy(out, (const void *)pdo->res->data, chunk);
                out   += chunk;
                addr  += chunk;
                bytes -= chunk;
        }

        return FSA_MEM_OK;
}

int
fsa_mem_pdo_write(fsa_mem_pdo_t *pdo, uint32_t addr, const void *src, uint32_t bytes, uint32_t max_cycles)
{
        const uint8_t *in = (const uint8_t *)src;
        int            ret;

        ret = fsa_mem_pdo_check_args(pdo, src, addr, bytes);
        if (ret != FSA_MEM_OK) {
                return ret;
        }

        while (bytes > 0u) {
                uint16_t chunk = bytes > FSA_MEM_CHUNK_BYTES ? FSA_MEM_CHUNK_BYTES : (uint16_t)bytes;
                uint32_t seq   = ++pdo->seq;

                pdo->req->address  = addr;
                pdo->req->length   = chunk;
                pdo->req->command  = FSA_MEM_CMD_WRITE;
                pdo->req->reserved = 0u;
                memcpy((void *)pdo->req->data, in, chunk);
                pdo->req->seq = seq;

                ret = fsa_mem_pdo_wait_ack(pdo, seq, max_cycles);
                if (ret != FSA_MEM_OK) {
                        return ret;
                }

                in    += chunk;
                addr  += chunk;
                bytes -= chunk;
        }

        return FSA_MEM_OK;
}

int
fsa_mem_pdo_read_u32(fsa_mem_pdo_t *pdo, uint32_t addr, uint32_t *value, uint32_t max_cycles)
{
        return fsa_mem_pdo_read(pdo, addr, value, sizeof(*value), max_cycles);
}

int
fsa_mem_pdo_write_u32(fsa_mem_pdo_t *pdo, uint32_t addr, uint32_t value, uint32_t max_cycles)
{
        return fsa_mem_pdo_write(pdo, addr, &value, sizeof(value), max_cycles);
}
