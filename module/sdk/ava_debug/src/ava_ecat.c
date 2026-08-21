#include "../inc/ava_ecat.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static int
ava_ecat_check_args(struct ava_ecat_sdo *ecat, const void *buf, uint32_t addr, uint32_t bytes)
{
    if ((ecat == NULL) || (ecat->read == NULL) || (ecat->write == NULL)) {
        return AVA_ECAT_ERR_ARG;
    }

    if ((buf == NULL) && (bytes != 0U)) {
        return AVA_ECAT_ERR_ARG;
    }

    if (((addr | bytes) & 0x3U) != 0U) {
        return AVA_ECAT_ERR_ALIGN;
    }

    return AVA_ECAT_OK;
}

static int
ava_ecat_sdo_read(struct ava_ecat_sdo *ecat, uint8_t subindex, void *data, uint32_t size)
{
    return ecat->read(ecat->user, AVA_ECAT_COE_INDEX, subindex, data, size) == 0 ? AVA_ECAT_OK
                                                                                 : AVA_ECAT_ERR_SDO;
}

static int
ava_ecat_sdo_write(struct ava_ecat_sdo *ecat, uint8_t subindex, const void *data, uint32_t size)
{
    return ecat->write(ecat->user, AVA_ECAT_COE_INDEX, subindex, data, size) == 0
               ? AVA_ECAT_OK
               : AVA_ECAT_ERR_SDO;
}

static int
ava_ecat_set_window(struct ava_ecat_sdo *ecat, uint32_t addr, uint16_t bytes)
{
    int ret;

    ret = ava_ecat_sdo_write(ecat, AVA_ECAT_COE_SUB_ADDRESS, &addr, sizeof(addr));
    if (ret != AVA_ECAT_OK) {
        return ret;
    }

    ret = ava_ecat_sdo_write(ecat, AVA_ECAT_COE_SUB_LENGTH, &bytes, sizeof(bytes));
    if (ret != AVA_ECAT_OK) {
        return ret;
    }

    return AVA_ECAT_OK;
}

static int
ava_ecat_command(struct ava_ecat_sdo *ecat, uint8_t command)
{
    uint8_t status = AVA_ECAT_STATUS_IDLE;
    int     ret;

    ret = ava_ecat_sdo_write(ecat, AVA_ECAT_COE_SUB_COMMAND, &command, sizeof(command));
    if (ret != AVA_ECAT_OK) {
        return ret;
    }

    ret = ava_ecat_sdo_read(ecat, AVA_ECAT_COE_SUB_STATUS, &status, sizeof(status));
    if (ret != AVA_ECAT_OK) {
        return ret;
    }

    return status == AVA_ECAT_STATUS_OK ? AVA_ECAT_OK : AVA_ECAT_ERR_DEVICE;
}

int
ava_ecat_sdo_read_memory(struct ava_ecat_sdo *ecat, uint32_t addr, void *dst, uint32_t bytes)
{
    uint8_t *out = (uint8_t *)dst;
    int      ret;

    ret = ava_ecat_check_args(ecat, dst, addr, bytes);
    if (ret != AVA_ECAT_OK) {
        return ret;
    }

    while (bytes > 0U) {
        uint16_t chunk = bytes > AVA_ECAT_CHUNK_BYTES ? AVA_ECAT_CHUNK_BYTES : (uint16_t)bytes;

        ret = ava_ecat_set_window(ecat, addr, chunk);
        if (ret != AVA_ECAT_OK) {
            return ret;
        }

        ret = ava_ecat_command(ecat, AVA_ECAT_CMD_READ);
        if (ret != AVA_ECAT_OK) {
            return ret;
        }

        ret = ava_ecat_sdo_read(ecat, AVA_ECAT_COE_SUB_DATA, out, chunk);
        if (ret != AVA_ECAT_OK) {
            return ret;
        }

        out   += chunk;
        addr  += chunk;
        bytes -= chunk;
    }

    return AVA_ECAT_OK;
}

int
ava_ecat_sdo_write_memory(struct ava_ecat_sdo *ecat, uint32_t addr, const void *src, uint32_t bytes)
{
    const uint8_t *in = (const uint8_t *)src;
    int            ret;

    ret = ava_ecat_check_args(ecat, src, addr, bytes);
    if (ret != AVA_ECAT_OK) {
        return ret;
    }

    while (bytes > 0U) {
        uint16_t chunk = bytes > AVA_ECAT_CHUNK_BYTES ? AVA_ECAT_CHUNK_BYTES : (uint16_t)bytes;

        ret = ava_ecat_set_window(ecat, addr, chunk);
        if (ret != AVA_ECAT_OK) {
            return ret;
        }

        ret = ava_ecat_sdo_write(ecat, AVA_ECAT_COE_SUB_DATA, in, chunk);
        if (ret != AVA_ECAT_OK) {
            return ret;
        }

        ret = ava_ecat_command(ecat, AVA_ECAT_CMD_WRITE);
        if (ret != AVA_ECAT_OK) {
            return ret;
        }

        in    += chunk;
        addr  += chunk;
        bytes -= chunk;
    }

    return AVA_ECAT_OK;
}

int
ava_ecat_sdo_read_u32(struct ava_ecat_sdo *ecat, uint32_t addr, uint32_t *value)
{
    return ava_ecat_sdo_read_memory(ecat, addr, value, sizeof(*value));
}

int
ava_ecat_sdo_write_u32(struct ava_ecat_sdo *ecat, uint32_t addr, uint32_t value)
{
    return ava_ecat_sdo_write_memory(ecat, addr, &value, sizeof(value));
}

static int
ava_ecat_pdo_check_args(struct ava_ecat_pdo *pdo, const void *buf, uint32_t addr, uint32_t bytes)
{
    if ((pdo == NULL) || (pdo->req == NULL) || (pdo->res == NULL)) {
        return AVA_ECAT_ERR_ARG;
    }

    if ((buf == NULL) && (bytes != 0U)) {
        return AVA_ECAT_ERR_ARG;
    }

    if (((addr | bytes) & 0x3U) != 0U) {
        return AVA_ECAT_ERR_ALIGN;
    }

    return AVA_ECAT_OK;
}

static int
ava_ecat_pdo_wait_ack(struct ava_ecat_pdo *pdo, uint32_t seq, uint32_t max_cycles)
{
    uint32_t i;

    for (i = 0U; i <= max_cycles; i++) {
        if (pdo->res->ack == seq) {
            return pdo->res->status == AVA_ECAT_STATUS_OK ? AVA_ECAT_OK : AVA_ECAT_ERR_DEVICE;
        }

        if (pdo->wait != NULL) {
            int ret = pdo->wait(pdo->wait_user, seq);
            if (ret != 0) {
                return ret;
            }
        }
    }

    return AVA_ECAT_ERR_TIMEOUT;
}

int
ava_ecat_pdo_init(struct ava_ecat_pdo *pdo,
                  void                *output_pdo,
                  uint32_t             output_size,
                  const void          *input_pdo,
                  uint32_t             input_size,
                  ava_ecat_pdo_wait_fn wait,
                  void                *wait_user)
{
    struct fsa_neo_rx_pdo_ext       *rx;
    const struct fsa_neo_tx_pdo_ext *tx;

    if ((pdo == NULL) || (output_pdo == NULL) || (input_pdo == NULL)) {
        return AVA_ECAT_ERR_ARG;
    }

    if ((output_size < FSA_NEO_RX_PDO_SIZE) || (input_size < FSA_NEO_TX_PDO_SIZE)) {
        return AVA_ECAT_ERR_ARG;
    }

    rx = (struct fsa_neo_rx_pdo_ext *)output_pdo;
    tx = (const struct fsa_neo_tx_pdo_ext *)input_pdo;

    pdo->req       = &rx->mem_debug;
    pdo->res       = &tx->mem_debug;
    pdo->seq       = tx->mem_debug.ack;
    pdo->wait      = wait;
    pdo->wait_user = wait_user;

    return AVA_ECAT_OK;
}

int
ava_ecat_pdo_read(
    struct ava_ecat_pdo *pdo, uint32_t addr, void *dst, uint32_t bytes, uint32_t max_cycles)
{
    uint8_t *out = (uint8_t *)dst;
    int      ret;

    ret = ava_ecat_pdo_check_args(pdo, dst, addr, bytes);
    if (ret != AVA_ECAT_OK) {
        return ret;
    }

    while (bytes > 0U) {
        uint16_t chunk = bytes > AVA_ECAT_CHUNK_BYTES ? AVA_ECAT_CHUNK_BYTES : (uint16_t)bytes;
        uint32_t seq   = ++pdo->seq;

        pdo->req->address  = addr;
        pdo->req->length   = chunk;
        pdo->req->command  = AVA_ECAT_CMD_READ;
        pdo->req->reserved = 0U;
        pdo->req->seq      = seq;

        ret = ava_ecat_pdo_wait_ack(pdo, seq, max_cycles);
        if (ret != AVA_ECAT_OK) {
            return ret;
        }

        memcpy(out, (const void *)pdo->res->data, chunk);
        out   += chunk;
        addr  += chunk;
        bytes -= chunk;
    }

    return AVA_ECAT_OK;
}

int
ava_ecat_pdo_write(
    struct ava_ecat_pdo *pdo, uint32_t addr, const void *src, uint32_t bytes, uint32_t max_cycles)
{
    const uint8_t *in = (const uint8_t *)src;
    int            ret;

    ret = ava_ecat_pdo_check_args(pdo, src, addr, bytes);
    if (ret != AVA_ECAT_OK) {
        return ret;
    }

    while (bytes > 0U) {
        uint16_t chunk = bytes > AVA_ECAT_CHUNK_BYTES ? AVA_ECAT_CHUNK_BYTES : (uint16_t)bytes;
        uint32_t seq   = ++pdo->seq;

        pdo->req->address  = addr;
        pdo->req->length   = chunk;
        pdo->req->command  = AVA_ECAT_CMD_WRITE;
        pdo->req->reserved = 0U;
        memcpy((void *)pdo->req->data, in, chunk);
        pdo->req->seq = seq;

        ret = ava_ecat_pdo_wait_ack(pdo, seq, max_cycles);
        if (ret != AVA_ECAT_OK) {
            return ret;
        }

        in    += chunk;
        addr  += chunk;
        bytes -= chunk;
    }

    return AVA_ECAT_OK;
}

int
ava_ecat_pdo_read_u32(struct ava_ecat_pdo *pdo, uint32_t addr, uint32_t *value, uint32_t max_cycles)
{
    return ava_ecat_pdo_read(pdo, addr, value, sizeof(*value), max_cycles);
}

int
ava_ecat_pdo_write_u32(struct ava_ecat_pdo *pdo, uint32_t addr, uint32_t value, uint32_t max_cycles)
{
    return ava_ecat_pdo_write(pdo, addr, &value, sizeof(value), max_cycles);
}
