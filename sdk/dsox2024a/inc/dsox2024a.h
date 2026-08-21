#ifndef DSOX2024A_H
#define DSOX2024A_H

#include <stddef.h>

#if defined(_WIN32)
#if defined(DSOX2024A_BUILD)
#define DSOX2024A_API __declspec(dllexport)
#else
#define DSOX2024A_API __declspec(dllimport)
#endif
#else
#define DSOX2024A_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum Dsox2024aError {
        DSOX2024A_OK                   = 0,
        DSOX2024A_ERR_ARGUMENT         = -1,
        DSOX2024A_ERR_NOT_OPEN         = -2,
        DSOX2024A_ERR_NETWORK          = -3,
        DSOX2024A_ERR_TIMEOUT          = -4,
        DSOX2024A_ERR_PROTOCOL         = -5,
        DSOX2024A_ERR_BUFFER_TOO_SMALL = -6,
        DSOX2024A_ERR_INSTRUMENT       = -7,
        DSOX2024A_ERR_VISA             = -8
};

typedef struct Dsox2024aWaveformInfo {
        size_t points;
        double x_increment;
        double x_origin;
        double x_reference;
        double y_increment;
        double y_origin;
        double y_reference;
} Dsox2024aWaveformInfo;

/* Connect to the oscilloscope's raw SCPI socket (normally TCP port 5025). */
DSOX2024A_API int dsox2024a_open(const char *host, unsigned short port, int timeout_ms);
/* Connect via USBTMC/VISA. NULL resource automatically finds a DSOX2024A. */
DSOX2024A_API int dsox2024a_open_usb(const char *resource, int timeout_ms);
DSOX2024A_API int dsox2024a_close(void);
DSOX2024A_API int dsox2024a_is_open(void);

DSOX2024A_API int dsox2024a_identify(char *response, size_t capacity);
DSOX2024A_API int dsox2024a_reset(void);
DSOX2024A_API int dsox2024a_autoscale(void);
DSOX2024A_API int dsox2024a_run(void);
DSOX2024A_API int dsox2024a_stop(void);
DSOX2024A_API int dsox2024a_single(void);
DSOX2024A_API int dsox2024a_digitize(int channel); /* channel: 1..4 */

DSOX2024A_API int dsox2024a_set_channel_enabled(int channel, int enabled);
DSOX2024A_API int dsox2024a_set_channel_scale(int channel, double volts_per_div);
DSOX2024A_API int dsox2024a_set_channel_offset(int channel, double volts);
DSOX2024A_API int dsox2024a_set_channel_coupling(int channel, const char *coupling); /* AC/DC */
DSOX2024A_API int dsox2024a_set_probe_ratio(int channel, double ratio);
DSOX2024A_API int dsox2024a_set_timebase_scale(double seconds_per_div);
DSOX2024A_API int dsox2024a_set_timebase_position(double seconds);
DSOX2024A_API int dsox2024a_set_edge_trigger(int channel, double level_volts, const char *slope);

DSOX2024A_API int dsox2024a_measure_frequency(int channel, double *hz);
DSOX2024A_API int dsox2024a_measure_vpp(int channel, double *volts);
DSOX2024A_API int dsox2024a_measure_rms(int channel, double *volts);

/*
 * Read calibrated samples. Pass samples=NULL/capacity=0 first to obtain the
 * required point count in info->points. The returned time for sample i is:
 * x_origin + (i - x_reference) * x_increment.
 */
DSOX2024A_API int
dsox2024a_read_waveform(int channel, size_t requested_points, double *samples, size_t capacity, Dsox2024aWaveformInfo *info);

/* Low-level SCPI escape hatch. query reads one newline-terminated response. */
DSOX2024A_API int dsox2024a_write(const char *command);
DSOX2024A_API int dsox2024a_query(const char *command, char *response, size_t capacity);
DSOX2024A_API int dsox2024a_check_error(char *response, size_t capacity);

DSOX2024A_API int         dsox2024a_last_error_code(void);
DSOX2024A_API const char *dsox2024a_last_error(void);

#ifdef __cplusplus
}
#endif
#endif
