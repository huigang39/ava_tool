#ifndef KPS6050D_H
#define KPS6050D_H

#include <stddef.h>
#include <stdint.h>

#if defined(KPS6050D_STATIC)
#define KPS_API
#elif defined(_WIN32)
#if defined(KPS6050D_BUILD)
#define KPS_API __declspec(dllexport)
#else
#define KPS_API __declspec(dllimport)
#endif
#else
#define KPS_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Kps6050dState {
        float voltage;
        float current;
        float set_voltage;
        float set_current;
        float max_voltage;
        float max_current;
        int   output_on;
        int   ocp_on;
        int   remote_locked;
        int   constant_current;
        int   alarm;
} Kps6050dState;

/* SDK caller tables. Search only enumerates ports; connect only reads the PSU.
 * Neither operation changes the output switch or voltage/current settings. */
typedef enum kps6050d_port_action {
        KPS6050D_PORT_NONE = 0,
        KPS6050D_PORT_CONNECT = 1,
        KPS6050D_PORT_DISCONNECT = 2,
        KPS6050D_PORT_RESCAN = 3
} kps6050d_port_action;

typedef struct kps6050d_port_param {
        uint32_t index;
        uint32_t count;
        char name[64];
        char desc[160];
        char port[64];
        uint32_t baud_rate;             // 0 defaults to 2400; supports 2400/4800/9600/19200
        uint32_t device_id;             // 0..31, must match the power supply
        kps6050d_port_action action;
        uint32_t connected;
        char active_port[64];
        uint32_t active_baud_rate;
        uint32_t active_device_id;
        int32_t last_error_code;
        char last_error[256];
} kps6050d_port_param;

// V, 0..60
typedef struct kps6050d_voltage_setting { float val;   } kps6050d_voltage_setting;
// A, 0..50
typedef struct kps6050d_current_setting { float val;   } kps6050d_current_setting;
// 0 or 1
typedef struct kps6050d_switch_setting { uint32_t enabled;   } kps6050d_switch_setting;

/* Editing a group in ava_tool sets its write_<group> gate automatically.
 * Only gated groups are written, in one read/modify/write operation. Gates
 * are consumed on each write attempt. Read refreshes all editable values. */
typedef struct kps6050d_param {
        kps6050d_voltage_setting voltage;
        kps6050d_current_setting current;
        kps6050d_switch_setting output;
        kps6050d_switch_setting ocp;
        kps6050d_switch_setting remote_lock;
        uint32_t write_voltage;
        uint32_t write_current;
        uint32_t write_output;
        uint32_t write_ocp;
        uint32_t write_remote_lock;
        Kps6050dState device;
        uint32_t connected;
        int32_t last_error_code;
        char last_error[256];
} kps6050d_param;

typedef struct kps6050d_rt {
        float voltage;    // V
        float current;    // A
        float power;      // W
        float set_voltage;
        float set_current;
        uint32_t output_on;
        uint32_t ocp_on;
        uint32_t remote_locked;
        uint32_t constant_current;
        uint32_t alarm;
        uint32_t sample_count;
        uint32_t connected;
        uint32_t fresh;
        int32_t last_error_code;
        char last_error[256];
} kps6050d_rt;

KPS_API uint32_t kps6050d_port_count(void);
KPS_API int kps6050d_port_name(uint32_t index, char *name, size_t name_size, char *desc, size_t desc_size);
KPS_API int kps6050d_read_port_param(kps6050d_port_param *param);
KPS_API int kps6050d_read_param(kps6050d_param *param);
KPS_API int kps6050d_write_param(kps6050d_param *param);
KPS_API int kps6050d_read_rt(kps6050d_rt *rt);

// Open an RS232 port in 8N1 mode. device_id is the PSU system-setting ID
KPS_API int kps6050d_open(const char *port, int baud_rate, int device_id);
KPS_API int kps6050d_close(void);
KPS_API int kps6050d_is_open(void);

// Read registers 0..7. Returns 0 on success, a negative error code on failure
KPS_API int   kps6050d_read(Kps6050dState *state);
KPS_API float kps6050d_get_voltage(void);
KPS_API float kps6050d_get_current(void);

// Values are range-checked against the 60 V / 50 A KPS6050D limits
KPS_API int kps6050d_set_voltage(float volts);
KPS_API int kps6050d_set_current(float amps);
KPS_API int kps6050d_set_output(int enabled);
KPS_API int kps6050d_set_ocp(int enabled);
KPS_API int kps6050d_set_remote_lock(int enabled);

KPS_API int         kps6050d_last_error_code(void);
KPS_API const char *kps6050d_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
