#ifndef DYN200_H
#define DYN200_H

#include <stddef.h>
#include <stdint.h>

#if defined(DYN200_STATIC)
#define DYN200_API
#elif defined(_WIN32)
#if defined(DYN200_BUILD)
#define DYN200_API __declspec(dllexport)
#else
#define DYN200_API __declspec(dllimport)
#endif
#else
#define DYN200_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum Dyn200Error {
        DYN200_OK = 0,
        DYN200_ERR_ARGUMENT = -1,
        DYN200_ERR_OPEN = -2,
        DYN200_ERR_CONFIGURE = -3,
        DYN200_ERR_NOT_OPEN = -4,
        DYN200_ERR_WRITE = -5,
        DYN200_ERR_TIMEOUT = -6,
        DYN200_ERR_READ = -7,
        DYN200_ERR_CRC = -8,
        DYN200_ERR_PROTOCOL = -9,
        DYN200_ERR_MODBUS = -10
};

/* Byte addresses from the DYN-200 manual. Each value occupies two registers. */
enum Dyn200Register {
        DYN200_REG_TORQUE = 0x0000,
        DYN200_REG_SPEED = 0x0002,
        DYN200_REG_POWER = 0x0004,
        DYN200_REG_FILTER = 0x0006,
        DYN200_REG_TORQUE_DECIMALS = 0x0008,
        DYN200_REG_ZERO_ON_BOOT = 0x000a,
        DYN200_REG_TRANSMIT_ZERO = 0x000c,
        DYN200_REG_TRANSMIT_FULL = 0x000e,
        DYN200_REG_RANGE = 0x0010,
        DYN200_REG_TORQUE_DIRECTION = 0x0012,
        DYN200_REG_BAUD_RATE = 0x0014,
        DYN200_REG_DEVICE_ADDRESS = 0x0016,
        DYN200_REG_STOP_BITS = 0x0018,
        DYN200_REG_COEFFICIENT = 0x001a,
        DYN200_REG_COMMUNICATION_MODE = 0x001c,
        DYN200_REG_SPEED_FILTER = 0x001e,
        DYN200_REG_SPEED_DECIMALS = 0x0020
};

typedef struct Dyn200Measurement {
        int32_t torque_raw;
        int32_t speed_raw;
        int32_t power_raw;
        double torque_nm;
        double speed_rpm;
        double power_kw;
} Dyn200Measurement;

/*
 * Open an RS-485 serial adapter for Modbus RTU (8N1). Typical arguments are
 * COM7/9600/1 on Windows or /dev/ttyUSB0/9600/1 on Unix. Modbus polling uses
 * device addresses 1..120; address zero is reserved for broadcast.
 */
DYN200_API int dyn200_open(const char *port, int baud_rate, int device_address, int timeout_ms);
/* Variant for sensors configured for 8N2 instead of 8N1. */
DYN200_API int dyn200_open_ex(const char *port, int baud_rate, int device_address, int timeout_ms, int stop_bits);
/* Open mode 0/3 active-upload streams; no Modbus device address is involved. */
DYN200_API int dyn200_open_active(const char *port, int baud_rate, int timeout_ms, int stop_bits);
DYN200_API int dyn200_open_active8(const char *port, int baud_rate, int timeout_ms, int stop_bits);
DYN200_API int dyn200_close(void);
DYN200_API int dyn200_is_open(void);

/* Fixed-point scaling defaults to torque=2, speed=0, power=0 decimals. */
DYN200_API int dyn200_set_decimals(int torque_decimals, int speed_decimals, int power_decimals);
DYN200_API int dyn200_read(Dyn200Measurement *measurement);

/* Generic Modbus access. Register count is limited to 12 by the device manual. */
DYN200_API int dyn200_read_registers(uint16_t address, uint16_t count, uint16_t *values);
DYN200_API int dyn200_read_i32(uint16_t address, int32_t *value);
DYN200_API int dyn200_write_i32(uint16_t address, int32_t value);

DYN200_API int dyn200_zero(void);
DYN200_API int dyn200_factory_reset(void);
DYN200_API int dyn200_set_filter(int value);              /* 1..100 */
DYN200_API int dyn200_set_zero_on_boot(int enabled);
DYN200_API int dyn200_set_torque_direction(int reversed);

/* Helpers for communication modes 0 and 3 (active upload). */
DYN200_API uint16_t dyn200_crc16(const uint8_t *data, size_t size);
DYN200_API int dyn200_decode_active6(const uint8_t frame[6], int32_t *torque, int32_t *speed);
DYN200_API int dyn200_decode_active8(const uint8_t frame[8], int32_t *torque, uint32_t *speed);
/* Read and CRC-resynchronize one frame from communication mode 0 or 3. */
DYN200_API int dyn200_read_active6(Dyn200Measurement *measurement);
DYN200_API int dyn200_read_active8(Dyn200Measurement *measurement);

/*
 * Scalar accessors for ava_tool function-bound LOCAL variables.
 * read_* acquires a new frame and returns NAN on failure. get_* only returns
 * the most recently acquired frame, so several channels can share one sample.
 */
DYN200_API int dyn200_update(void);
DYN200_API double dyn200_read_torque_nm(void);
DYN200_API double dyn200_read_speed_rpm(void);
DYN200_API double dyn200_read_power_kw(void);
DYN200_API double dyn200_get_torque_nm(void);
DYN200_API double dyn200_get_speed_rpm(void);
DYN200_API double dyn200_get_power_kw(void);
DYN200_API float dyn200_read_torque_f32(void);
DYN200_API float dyn200_read_speed_f32(void);
DYN200_API float dyn200_read_power_f32(void);
DYN200_API float dyn200_get_torque_f32(void);
DYN200_API float dyn200_get_speed_f32(void);
DYN200_API float dyn200_get_power_f32(void);
DYN200_API int32_t dyn200_read_torque_raw(void);
DYN200_API int32_t dyn200_read_speed_raw(void);
DYN200_API int32_t dyn200_read_power_raw(void);
DYN200_API int32_t dyn200_get_torque_raw(void);
DYN200_API int32_t dyn200_get_speed_raw(void);
DYN200_API int32_t dyn200_get_power_raw(void);

DYN200_API int dyn200_last_error_code(void);
DYN200_API const char *dyn200_last_error(void);

#ifdef __cplusplus
}
#endif
#endif
