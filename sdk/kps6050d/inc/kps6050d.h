#ifndef KPS6050D_H
#define KPS6050D_H

#if defined(_WIN32)
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

/* Open an RS232 port in 8N1 mode. device_id is the PSU system-setting ID. */
KPS_API int kps6050d_open(const char *port, int baud_rate, int device_id);
KPS_API int kps6050d_close(void);
KPS_API int kps6050d_is_open(void);

/* Read registers 0..7. Returns 0 on success, a negative error code on failure. */
KPS_API int   kps6050d_read(Kps6050dState *state);
KPS_API float kps6050d_get_voltage(void);
KPS_API float kps6050d_get_current(void);

/* Values are range-checked against the 60 V / 50 A KPS6050D limits. */
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
