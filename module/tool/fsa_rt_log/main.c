#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fsa_rma/fsa_rma_addr.h"
#include "fsa_rma/fsa_rmaio.h"
#include "module.h"
#include "version.h"

#if OS(WIN)
#include <direct.h>
#include <windows.h>
#elif OS(POSIX)
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define TOOL_NAME                  "fsa_rt_log"
#define DEFAULT_PORT               (2340U)
#define DEFAULT_FREQUENCY_HZ       (1000U)
#define DEFAULT_ERROR_FREQUENCY_HZ (100U)
#define DEFAULT_TIMEOUT_US         (1000U)
#define DEFAULT_ERROR_STOP_DELAY_S (3U)
#define DEFAULT_OUTPUT_DIR         "fsa_rt_logs"
#define DEFAULT_IP_PREFIX          "192.168.137"
#define DEFAULT_ROTATE_SIZE        (16U * SIZE_1MB)
#define DEFAULT_MAX_FILES          (10U)
#define MAX_FSA_NUM                (64U)
#define MAX_IPV4_LENGTH            (16U)
#define MAX_PATH_LENGTH            (512U)
#define LOG_HEADER_LENGTH          (512U)
#define LOG_RECORD_LENGTH          (1024U)
#define LOG_MEMPOOL_SIZE           (SIZE_64KB)
#define LOG_FLUSH_BUF_SIZE         (SIZE_2KB)
#define LOG_CHUNK_SIZE             (SIZE_4KB)
#define FSA_ERROR_CODE_COUNT       (8U)
#define ERROR2_TYPE_MASK           (0x03U)

struct cli_options {
    const char *ips[MAX_FSA_NUM];
    char        ip_storage[MAX_FSA_NUM][MAX_IPV4_LENGTH];
    size_t      ip_count;
    const char *output_dir;
    uint16_t    port;
    uint32_t    frequency_hz;
    uint32_t    error_frequency_hz;
    uint32_t    timeout_us;
    uint64_t    error_stop_delay_s;
    uint32_t    ignored_error_masks[FSA_ERROR_CODE_COUNT];
    uint64_t    sample_count;
    uint64_t    rotate_size;
    uint32_t    max_files;
    bool        include_pvct;
    bool        include_control;
    bool        include_pid;
    bool        include_power_status;
    bool        quiet;
};

struct optional_sample {
    uint32_t  control_mode;
    uint32_t  control_word;
    uint32_t  work_mode;
    float p_kp;
    float v_kp;
    float v_ki;
    float pd_kp;
    float pd_kd;
    float temp_mos;
    float temp_coil;
    float vbus;
};

struct fsa {
    struct net_ch       ch;
    uint32_t            cnt;
    struct rmaio_fsa2pc rmaio_fsa2pc;
    struct rmaio_pc2fsa rmaio_pc2fsa;
    const char         *ip;
    uint64_t            attempts;
    uint64_t            records;
    uint64_t            errors;
    uint64_t            error_read_errors;
    bool                fsa_error_detected;
    pthread_t           thread;
    bool                thread_started;
    struct log          log;
    struct mempool      log_mempool;
    uint8_t            *log_mempool_buf;
    char                log_dir[MAX_PATH_LENGTH];
    bool                log_initialized;
};

static struct net            g_net;
static struct fsa           *g_fsas;
static struct cli_options    g_options;
static volatile sig_atomic_t g_stop_requested;
static ATOMIC(uint64_t) g_error_stop_deadline_us;
static ATOMIC(uint8_t) g_error_stop_announced;

static const struct net_cfg g_net_cfg = {
    .e_type   = NET_TYPE_UDP,
    .f_get_ts = get_real_ts_us,
};

static char g_log_file_header[LOG_HEADER_LENGTH];

static void
print_usage(FILE *stream)
{
    fprintf(
        stream,
        "Usage: %s [options] [IP ...]\n\n"
        "Collect FSA reference and feedback values into rotating log files.\n"
        "At least one device IP is required.\n\n"
        "Options:\n"
        "  -i, --ip IP          Add full IP, last octet, or range; may be repeated\n"
        "  -p, --port PORT      UDP port (default: %u)\n"
        "  -o, --output-dir DIR Log directory (default: %s)\n"
        "  -f, --frequency HZ   Sampling frequency (default: %u Hz)\n"
        "      --error-frequency HZ\n"
        "                         Error-code polling frequency (default: %u Hz)\n"
        "      --timeout-us US  Receive timeout (default: %u us)\n"
        "      --ignore-error ERRORn:MASK\n"
        "                         Ignore an error bit mask; may be repeated\n"
        "      --error-stop-delay-s S\n"
        "                         Stop delay after an FSA error (default: %u s)\n"
        "  -n, --count N        Stop after N attempts per device\n"
        "      --rotate-size N  Bytes per log file (default: %u)\n"
        "      --max-files N    Files retained per device (default: %u)\n"
        "      --pvct BOOL      Include PVCT data (default: true)\n"
        "      --control BOOL   Include control mode/word and work mode (default: false)\n"
        "      --pid BOOL       Include real-time PID/PD parameters (default: false)\n"
        "      --power-status BOOL\n"
        "                         Include MOS/coil temperatures and bus voltage (default: false)\n"
        "  -q, --quiet          Suppress normal status messages\n"
        "  -V, --version        Show version\n"
        "  -h, --help           Show this help\n\n"
        "Examples:\n"
        "  %s 192.168.137.101 192.168.137.102\n"
        "  %s 101 110~116\n"
        "  Short IP forms use prefix %s\n",
        TOOL_NAME,
        DEFAULT_PORT,
        DEFAULT_OUTPUT_DIR,
        DEFAULT_FREQUENCY_HZ,
        DEFAULT_ERROR_FREQUENCY_HZ,
        DEFAULT_TIMEOUT_US,
        DEFAULT_ERROR_STOP_DELAY_S,
        DEFAULT_ROTATE_SIZE,
        DEFAULT_MAX_FILES,
        TOOL_NAME,
        TOOL_NAME,
        DEFAULT_IP_PREFIX);
}

static bool
append_format(char *buffer, size_t capacity, size_t *length, const char *format, ...)
{
    va_list arguments;
    int     written;

    if (*length >= capacity) {
        return false;
    }
    va_start(arguments, format);
    written = vsnprintf(buffer + *length, capacity - *length, format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= capacity - *length) {
        return false;
    }
    *length += (size_t)written;
    return true;
}

static bool
build_log_file_header(void)
{
    size_t length = 0U;

    g_log_file_header[0] = '\0';
    if (!append_format(
            g_log_file_header, sizeof(g_log_file_header), &length, "timestamp_us,producer_id")) {
        return false;
    }
    if (g_options.include_pvct &&
        !append_format(g_log_file_header,
                       sizeof(g_log_file_header),
                       &length,
                       ",ref_pos,fdb_pos,ref_vel,fdb_vel,ref_cur,fdb_cur,ref_tor,fdb_elec_tor")) {
        return false;
    }
    if (g_options.include_control && !append_format(g_log_file_header,
                                                    sizeof(g_log_file_header),
                                                    &length,
                                                    ",control_mode,control_word,work_mode")) {
        return false;
    }
    if (g_options.include_pid &&
        !append_format(
            g_log_file_header, sizeof(g_log_file_header), &length, ",p_kp,v_kp,v_ki,pd_kp,pd_kd")) {
        return false;
    }
    if (g_options.include_power_status &&
        !append_format(
            g_log_file_header, sizeof(g_log_file_header), &length, ",temp_mos,temp_coil,vbus")) {
        return false;
    }
    return append_format(g_log_file_header, sizeof(g_log_file_header), &length, ",latency_us\n");
}

static bool
parse_u64(const char *text, uint64_t minimum, uint64_t maximum, uint64_t *value)
{
    char              *end = NULL;
    unsigned long long parsed;

    if (text == NULL || *text == '\0' || *text == '-') {
        return false;
    }
    errno  = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static bool
parse_bool(const char *text, bool *value)
{
    if (text != NULL && strcmp(text, "true") == 0) {
        *value = true;
        return true;
    }
    if (text != NULL && strcmp(text, "false") == 0) {
        *value = false;
        return true;
    }
    return false;
}

static bool
add_ignored_error(struct cli_options *options, const char *text)
{
    char              *end = NULL;
    unsigned long long parsed;

    if (text == NULL || strlen(text) < 8U || toupper((unsigned char)text[0]) != 'E' ||
        toupper((unsigned char)text[1]) != 'R' || toupper((unsigned char)text[2]) != 'R' ||
        toupper((unsigned char)text[3]) != 'O' || toupper((unsigned char)text[4]) != 'R' ||
        text[5] < '1' || text[5] > '8' || text[6] != ':' || text[7] == '\0' || text[7] == '-') {
        print_error(true, " ignored error must use ERROR1:0x10 format");
        return false;
    }

    errno  = 0;
    parsed = strtoull(text + 7U, &end, 0);
    if (errno != 0 || *end != '\0' || parsed == 0U || parsed > UINT32_MAX) {
        print_error(true, " ignored error mask must be in the range 1..0xFFFFFFFF");
        return false;
    }

    const size_t error_index                   = (size_t)(text[5] - '1');
    options->ignored_error_masks[error_index] |= (uint32_t)parsed;
    return true;
}

static bool
is_valid_ipv4(const char *ip)
{
    const char *cursor = ip;

    if (ip == NULL || *ip == '\0') {
        return false;
    }
    for (unsigned int part = 0U; part < 4U; part++) {
        unsigned int value  = 0U;
        unsigned int digits = 0U;

        while (isdigit((unsigned char)*cursor)) {
            value = value * 10U + (unsigned int)(*cursor - '0');
            digits++;
            if (digits > 3U || value > 255U) {
                return false;
            }
            cursor++;
        }
        if (digits == 0U) {
            return false;
        }
        if (part < 3U) {
            if (*cursor != '.') {
                return false;
            }
            cursor++;
        } else if (*cursor != '\0') {
            return false;
        }
    }
    return true;
}

static bool
add_ip(struct cli_options *options, const char *ip)
{
    if (!is_valid_ipv4(ip)) {
        print_error(true, " invalid IPv4 address: %s", ip != NULL ? ip : "(null)");
        return false;
    }
    for (size_t i = 0U; i < options->ip_count; i++) {
        if (strcmp(options->ips[i], ip) == 0) {
            return true;
        }
    }
    if (options->ip_count >= MAX_FSA_NUM) {
        print_error(true, " at most %u IP addresses are supported", MAX_FSA_NUM);
        return false;
    }
    snprintf(options->ip_storage[options->ip_count], MAX_IPV4_LENGTH, "%s", ip);
    options->ips[options->ip_count] = options->ip_storage[options->ip_count];
    options->ip_count++;
    return true;
}

static bool
add_ip_suffix(struct cli_options *options, uint64_t suffix)
{
    char ip[MAX_IPV4_LENGTH];

    snprintf(ip, sizeof(ip), "%s.%u", DEFAULT_IP_PREFIX, (unsigned int)suffix);
    return add_ip(options, ip);
}

static bool
add_ip_spec(struct cli_options *options, const char *spec)
{
    const char *separator;
    uint64_t    first;
    uint64_t    last;

    if (is_valid_ipv4(spec)) {
        return add_ip(options, spec);
    }
    if (spec == NULL) {
        print_error(true, " invalid IP specification: (null)");
        return false;
    }

    separator = strchr(spec, '~');
    if (separator == NULL) {
        if (!parse_u64(spec, 0U, 255U, &first)) {
            print_error(true, " invalid IP specification: %s", spec);
            return false;
        }
        return add_ip_suffix(options, first);
    }
    if (strchr(separator + 1U, '~') != NULL || separator == spec || separator[1] == '\0') {
        print_error(true, " invalid IP range: %s", spec);
        return false;
    }

    const size_t first_length = (size_t)(separator - spec);
    if (first_length > 3U || strlen(separator + 1U) > 3U) {
        print_error(true, " invalid IP range: %s", spec);
        return false;
    }
    char first_text[4U];
    memcpy(first_text, spec, first_length);
    first_text[first_length] = '\0';

    if (!parse_u64(first_text, 0U, 255U, &first) || !parse_u64(separator + 1U, 0U, 255U, &last) ||
        first > last) {
        print_error(true, " invalid IP range: %s", spec);
        return false;
    }
    for (uint64_t suffix = first; suffix <= last; suffix++) {
        if (!add_ip_suffix(options, suffix)) {
            return false;
        }
    }
    return true;
}

static const char *
option_value(int argc, char **argv, int *index, const char *argument, const char *long_name)
{
    const size_t name_length = strlen(long_name);

    if (strncmp(argument, long_name, name_length) == 0 && argument[name_length] == '=') {
        return argument + name_length + 1U;
    }
    if (*index + 1 >= argc) {
        print_error(true, " option %s requires a value", argument);
        return NULL;
    }
    (*index)++;
    return argv[*index];
}

static int
parse_options(int argc, char **argv, struct cli_options *options)
{
    bool positional_only = false;

    *options = (struct cli_options){
        .output_dir          = DEFAULT_OUTPUT_DIR,
        .port                = DEFAULT_PORT,
        .frequency_hz        = DEFAULT_FREQUENCY_HZ,
        .error_frequency_hz  = DEFAULT_ERROR_FREQUENCY_HZ,
        .timeout_us          = DEFAULT_TIMEOUT_US,
        .error_stop_delay_s  = DEFAULT_ERROR_STOP_DELAY_S,
        .ignored_error_masks = {[1] = ERROR2_TYPE_MASK},
        .rotate_size         = DEFAULT_ROTATE_SIZE,
        .max_files           = DEFAULT_MAX_FILES,
        .include_pvct        = true,
    };

    for (int i = 1; i < argc; i++) {
        const char *argument = argv[i];
        const char *value;
        uint64_t    parsed;

        if (!positional_only && strcmp(argument, "--") == 0) {
            positional_only = true;
            continue;
        }
        if (!positional_only && (strcmp(argument, "-h") == 0 || strcmp(argument, "--help") == 0)) {
            print_usage(stdout);
            return 1;
        }
        if (!positional_only &&
            (strcmp(argument, "-V") == 0 || strcmp(argument, "--version") == 0)) {
            println("%s %s", TOOL_NAME, FSA_RT_LOG_VER_STRING);
            return 1;
        }
        if (!positional_only && (strcmp(argument, "-q") == 0 || strcmp(argument, "--quiet") == 0)) {
            options->quiet = true;
            continue;
        }
        if (!positional_only && strcmp(argument, "--pvct") == 0) {
            value = option_value(argc, argv, &i, argument, "--pvct");
            if (value == NULL || !parse_bool(value, &options->include_pvct)) {
                print_error(true, " --pvct must be followed by true or false");
                return -1;
            }
            continue;
        }
        if (!positional_only && strcmp(argument, "--control") == 0) {
            value = option_value(argc, argv, &i, argument, "--control");
            if (value == NULL || !parse_bool(value, &options->include_control)) {
                print_error(true, " --control must be followed by true or false");
                return -1;
            }
            continue;
        }
        if (!positional_only && strcmp(argument, "--pid") == 0) {
            value = option_value(argc, argv, &i, argument, "--pid");
            if (value == NULL || !parse_bool(value, &options->include_pid)) {
                print_error(true, " --pid must be followed by true or false");
                return -1;
            }
            continue;
        }
        if (!positional_only && strcmp(argument, "--power-status") == 0) {
            value = option_value(argc, argv, &i, argument, "--power-status");
            if (value == NULL || !parse_bool(value, &options->include_power_status)) {
                print_error(true, " --power-status must be followed by true or false");
                return -1;
            }
            continue;
        }
        if (!positional_only && (strcmp(argument, "-i") == 0 || strcmp(argument, "--ip") == 0 ||
                                 strncmp(argument, "--ip=", 5U) == 0)) {
            value = option_value(argc, argv, &i, argument, "--ip");
            if (value == NULL || !add_ip_spec(options, value)) {
                return -1;
            }
            continue;
        }
        if (!positional_only &&
            (strcmp(argument, "-o") == 0 || strcmp(argument, "--output-dir") == 0 ||
             strncmp(argument, "--output-dir=", 13U) == 0)) {
            value = option_value(argc, argv, &i, argument, "--output-dir");
            if (value == NULL || *value == '\0') {
                return -1;
            }
            options->output_dir = value;
            continue;
        }
        if (!positional_only && (strcmp(argument, "-p") == 0 || strcmp(argument, "--port") == 0 ||
                                 strncmp(argument, "--port=", 7U) == 0)) {
            value = option_value(argc, argv, &i, argument, "--port");
            if (value == NULL || !parse_u64(value, 1U, UINT16_MAX, &parsed)) {
                print_error(true, " port must be in the range 1..%u", UINT16_MAX);
                return -1;
            }
            options->port = (uint16_t)parsed;
            continue;
        }
        if (!positional_only &&
            (strcmp(argument, "-f") == 0 || strcmp(argument, "--frequency") == 0 ||
             strncmp(argument, "--frequency=", 12U) == 0)) {
            value = option_value(argc, argv, &i, argument, "--frequency");
            if (value == NULL || !parse_u64(value, 1U, 1000000U, &parsed)) {
                print_error(true, " frequency must be in the range 1..1000000 Hz");
                return -1;
            }
            options->frequency_hz = (uint32_t)parsed;
            continue;
        }
        if (!positional_only && (strcmp(argument, "--error-frequency") == 0 ||
                                 strncmp(argument, "--error-frequency=", 18U) == 0)) {
            value = option_value(argc, argv, &i, argument, "--error-frequency");
            if (value == NULL || !parse_u64(value, 1U, 1000000U, &parsed)) {
                print_error(true, " error frequency must be in the range 1..1000000 Hz");
                return -1;
            }
            options->error_frequency_hz = (uint32_t)parsed;
            continue;
        }
        if (!positional_only && (strcmp(argument, "--timeout-us") == 0 ||
                                 strncmp(argument, "--timeout-us=", 13U) == 0)) {
            value = option_value(argc, argv, &i, argument, "--timeout-us");
            if (value == NULL || !parse_u64(value, 1U, UINT32_MAX, &parsed)) {
                print_error(true, " timeout must be a positive microsecond value");
                return -1;
            }
            options->timeout_us = (uint32_t)parsed;
            continue;
        }
        if (!positional_only && (strcmp(argument, "--ignore-error") == 0 ||
                                 strncmp(argument, "--ignore-error=", 15U) == 0)) {
            value = option_value(argc, argv, &i, argument, "--ignore-error");
            if (value == NULL || !add_ignored_error(options, value)) {
                return -1;
            }
            continue;
        }
        if (!positional_only && (strcmp(argument, "--error-stop-delay-s") == 0 ||
                                 strncmp(argument, "--error-stop-delay-s=", 21U) == 0)) {
            value = option_value(argc, argv, &i, argument, "--error-stop-delay-s");
            if (value == NULL || !parse_u64(value, 0U, UINT64_MAX / 1000000U, &parsed)) {
                print_error(true, " error stop delay must be a non-negative second value");
                return -1;
            }
            options->error_stop_delay_s = parsed;
            continue;
        }
        if (!positional_only && (strcmp(argument, "--rotate-size") == 0 ||
                                 strncmp(argument, "--rotate-size=", 14U) == 0)) {
            value = option_value(argc, argv, &i, argument, "--rotate-size");
            if (value == NULL || !parse_u64(value, 1024U, UINT64_MAX, &parsed)) {
                print_error(true, " rotate size must be at least 1024 bytes");
                return -1;
            }
            options->rotate_size = parsed;
            continue;
        }
        if (!positional_only &&
            (strcmp(argument, "--max-files") == 0 || strncmp(argument, "--max-files=", 12U) == 0)) {
            value = option_value(argc, argv, &i, argument, "--max-files");
            if (value == NULL || !parse_u64(value, 1U, 1000U, &parsed)) {
                print_error(true, " max files must be in the range 1..1000");
                return -1;
            }
            options->max_files = (uint32_t)parsed;
            continue;
        }
        if (!positional_only && (strcmp(argument, "-n") == 0 || strcmp(argument, "--count") == 0 ||
                                 strncmp(argument, "--count=", 8U) == 0)) {
            value = option_value(argc, argv, &i, argument, "--count");
            if (value == NULL || !parse_u64(value, 1U, UINT64_MAX, &parsed)) {
                print_error(true, " count must be a positive integer");
                return -1;
            }
            options->sample_count = parsed;
            continue;
        }
        if (!positional_only && argument[0] == '-') {
            print_error(true, " unknown option: %s", argument);
            return -1;
        }
        if (!add_ip_spec(options, argument)) {
            return -1;
        }
    }
    if (options->ip_count == 0U) {
        print_error(true, " at least one device IP is required");
        return -1;
    }
    if (!options->include_pvct && !options->include_control && !options->include_pid &&
        !options->include_power_status) {
        print_error(true, " at least one CSV data group must be enabled");
        return -1;
    }
    return 0;
}

static void
signal_handler(int signal_number)
{
    ARG_UNUSED(signal_number);
    g_stop_requested = 1;
}

static int
fsa_init(struct fsa *fsa, const char *ip, uint16_t port)
{
    fsa->ip = ip;
    fsa->ch = net_cfg_ch(IP_STR_TO_U32(ip), port, NET_MODE_SYNC_YIELD);
    return net_add_ch(&g_net, &fsa->ch);
}

static int
fsa_get_ref_pvct(struct fsa *fsa, struct foc_ref_pvct *ref_pvct)
{
    rmaio_pc2fsa_generate_frame_head(&fsa->rmaio_pc2fsa, 0, 0, 0, US_50, 0, 0, fsa->cnt++);
    rmaio_pc2fsa_add_read(&fsa->rmaio_pc2fsa, RMAIO_ADDR_REAL_TIME_SET_PVCT_FFD_BASE, 7);
    const int ret = (int)net_send_recv(&g_net,
                                       &fsa->ch,
                                       fsa->rmaio_pc2fsa.send_buf,
                                       fsa->rmaio_pc2fsa.generate_index,
                                       fsa->rmaio_fsa2pc.recv_buf,
                                       sizeof(fsa->rmaio_fsa2pc.recv_buf),
                                       g_options.timeout_us);
    if (ret < 0) {
        return ret;
    }
    rmaio_fsa2pc_parse(&fsa->rmaio_fsa2pc, ret);
    ref_pvct->pos =
        u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_SET_PVCT_FFD_P));
    ref_pvct->vel =
        u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_SET_PVCT_FFD_V));
    ref_pvct->cur =
        u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_SET_PVCT_FFD_C));
    ref_pvct->tor =
        u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_SET_PVCT_FFD_T));
    ref_pvct->ffd_vel = u32_to_fp32(
        rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_SET_PVCT_FFD_V_FF));
    ref_pvct->ffd_cur = u32_to_fp32(
        rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_SET_PVCT_FFD_C_FF));
    ref_pvct->ffd_tor = u32_to_fp32(
        rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_SET_PVCT_FFD_T_FF));
    return ret;
}

static int
fsa_get_sample_data(struct fsa             *fsa,
                    struct foc_fdb_pvct    *fdb_pvct,
                    struct optional_sample *optional)
{
    rmaio_pc2fsa_generate_frame_head(&fsa->rmaio_pc2fsa, 0, 0, 0, US_50, 0, 0, fsa->cnt++);
    if (g_options.include_pvct) {
        rmaio_pc2fsa_add_read(&fsa->rmaio_pc2fsa, RMAIO_ADDR_REAL_TIME_GET_PVCT_BASE, 5);
    }
    if (g_options.include_control) {
        rmaio_pc2fsa_add_read(&fsa->rmaio_pc2fsa, RMAIO_ADDR_REAL_TIME_CONTROL_BASE, 3);
    }
    if (g_options.include_pid) {
        rmaio_pc2fsa_add_read(&fsa->rmaio_pc2fsa, RMAIO_ADDR_REAL_TIME_PIDPD_PARAM_BASE, 5);
    }
    if (g_options.include_power_status) {
        rmaio_pc2fsa_add_read(&fsa->rmaio_pc2fsa, RMAIO_ADDR_REAL_TIME_POWER_STATUS_BASE, 3);
    }
    const int ret = (int)net_send_recv(&g_net,
                                       &fsa->ch,
                                       fsa->rmaio_pc2fsa.send_buf,
                                       fsa->rmaio_pc2fsa.generate_index,
                                       fsa->rmaio_fsa2pc.recv_buf,
                                       sizeof(fsa->rmaio_fsa2pc.recv_buf),
                                       g_options.timeout_us);
    if (ret < 0) {
        return ret;
    }
    rmaio_fsa2pc_parse(&fsa->rmaio_fsa2pc, ret);
    if (g_options.include_pvct) {
        fdb_pvct->pos =
            u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_GET_PVCT_P));
        fdb_pvct->vel =
            u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_GET_PVCT_V));
        fdb_pvct->cur =
            u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_GET_PVCT_C));
        fdb_pvct->load_tor =
            u32_to_fp32(rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_GET_PVCT_T));
        fdb_pvct->elec_tor = u32_to_fp32(
            rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_GET_PVCT_TE));
    }
    if (g_options.include_control) {
        optional->control_mode =
            rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_CONTROL_MODE);
        optional->control_word =
            rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_CONTROL_WORD);
        optional->work_mode =
            rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_WORK_MODE);
    }
    if (g_options.include_pid) {
        optional->p_kp = u32_to_fp32(
            rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_PIDPD_PARAM_P_KP));
        optional->v_kp = u32_to_fp32(
            rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_PIDPD_PARAM_V_KP));
        optional->v_ki = u32_to_fp32(
            rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_PIDPD_PARAM_V_KI));
        optional->pd_kp = u32_to_fp32(
            rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_PIDPD_PARAM_PD_KP));
        optional->pd_kd = u32_to_fp32(
            rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_PIDPD_PARAM_PD_KD));
    }
    if (g_options.include_power_status) {
        optional->temp_mos = u32_to_fp32(
            rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_POWER_STATUS_TEMP_MOS));
        optional->temp_coil = u32_to_fp32(
            rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_POWER_STATUS_TEMP_COIL));
        optional->vbus = u32_to_fp32(
            rmaio_fsa2pc_read_mem(&fsa->rmaio_fsa2pc, RMAIO_ADDR_REAL_TIME_POWER_STATUS_VBUS));
    }
    return ret;
}

static int
fsa_get_error_codes(struct fsa *fsa, uint32_t *error_codes)
{
    rmaio_pc2fsa_generate_frame_head(&fsa->rmaio_pc2fsa, 0, 0, 0, US_50, 0, 0, fsa->cnt++);
    rmaio_pc2fsa_add_read(
        &fsa->rmaio_pc2fsa, RMAIO_ADDR_REAL_TIME_ERROR_CODE_BASE, FSA_ERROR_CODE_COUNT);
    const int ret = (int)net_send_recv(&g_net,
                                       &fsa->ch,
                                       fsa->rmaio_pc2fsa.send_buf,
                                       fsa->rmaio_pc2fsa.generate_index,
                                       fsa->rmaio_fsa2pc.recv_buf,
                                       sizeof(fsa->rmaio_fsa2pc.recv_buf),
                                       g_options.timeout_us);
    if (ret < 0) {
        return ret;
    }
    rmaio_fsa2pc_parse(&fsa->rmaio_fsa2pc, ret);
    for (size_t i = 0U; i < FSA_ERROR_CODE_COUNT; i++) {
        error_codes[i] = rmaio_fsa2pc_read_mem(
            &fsa->rmaio_fsa2pc, (uint16_t)(RMAIO_ADDR_REAL_TIME_ERROR_CODE_BASE + i));
    }
    return ret;
}

static bool
check_fsa_error_codes(struct fsa *fsa, const uint32_t *error_codes)
{
    uint32_t active_error_codes[FSA_ERROR_CODE_COUNT] = {0U};
    bool     has_active_error                         = false;

    if (fsa->fsa_error_detected) {
        return false;
    }
    for (size_t i = 0U; i < FSA_ERROR_CODE_COUNT; i++) {
        active_error_codes[i] = error_codes[i] & ~g_options.ignored_error_masks[i];
        if (active_error_codes[i] != 0U) {
            has_active_error = true;
        }
    }
    if (!has_active_error) {
        return false;
    }

    const uint64_t detected_us   = get_mono_ts_us();
    const uint64_t stop_delay_us = g_options.error_stop_delay_s * 1000000U;
    uint64_t       deadline_us;
    uint64_t       expected_deadline_us = 0U;

    fsa->fsa_error_detected = true;
    if (UINT64_MAX - detected_us < stop_delay_us) {
        deadline_us = UINT64_MAX;
    } else {
        deadline_us = detected_us + stop_delay_us;
    }
    ATOMIC_CAS_STRONG(&g_error_stop_deadline_us, &expected_deadline_us, deadline_us);

    print_error(true,
                " [%s] FSA error detected; collection will stop in %llu s",
                fsa->ip,
                (unsigned long long)g_options.error_stop_delay_s);
    for (size_t i = 0U; i < FSA_ERROR_CODE_COUNT; i++) {
        if (active_error_codes[i] != 0U) {
            print_error(true,
                        " [%s] ERROR%u=0x%08X, ignored=0x%08X, active=0x%08X",
                        fsa->ip,
                        (unsigned int)(i + 1U),
                        error_codes[i],
                        g_options.ignored_error_masks[i],
                        active_error_codes[i]);
        }
    }
    if (g_options.error_stop_delay_s == 0U) {
        g_stop_requested = 1;
        return true;
    }
    return false;
}

static bool
fsa_error_stop_delay_elapsed(void)
{
    const uint64_t deadline_us = ATOMIC_LOAD(&g_error_stop_deadline_us);

    if (deadline_us == 0U || get_mono_ts_us() < deadline_us) {
        return false;
    }
    if (ATOMIC_EXCHANGE(&g_error_stop_announced, 1U) == 0U) {
        print_error(true, " FSA error stop delay elapsed; stopping all collection");
    }
    g_stop_requested = 1;
    return true;
}

static int
create_directory(const char *path)
{
#if OS(WIN)
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

static int
ensure_directory(const char *path)
{
    char   buffer[MAX_PATH_LENGTH];
    size_t length;

    if (path == NULL || *path == '\0') {
        return -1;
    }
    length = strlen(path);
    if (length >= sizeof(buffer)) {
        print_error(true, " output directory path is too long");
        return -1;
    }
    memcpy(buffer, path, length + 1U);

    while (length > 1U && (buffer[length - 1U] == '/' || buffer[length - 1U] == '\\')) {
        buffer[--length] = '\0';
    }
    for (size_t i = 1U; i < length; i++) {
        if (buffer[i] != '/' && buffer[i] != '\\') {
            continue;
        }
        if (i == 2U && buffer[1] == ':') {
            continue;
        }

        const char separator = buffer[i];
        buffer[i]            = '\0';
        if (create_directory(buffer) != 0 && errno != EEXIST) {
            print_error(true, " cannot create directory '%s': %s", buffer, strerror(errno));
            return -1;
        }
        buffer[i] = separator;
    }
    if (create_directory(buffer) != 0 && errno != EEXIST) {
        print_error(true, " cannot create directory '%s': %s", buffer, strerror(errno));
        return -1;
    }
    return 0;
}

static bool
init_device_log(struct fsa *fsa)
{
    const char *last_octet = strrchr(fsa->ip, '.');
    if (last_octet == NULL || last_octet[1] == '\0') {
        return false;
    }
    last_octet++;

    snprintf(fsa->log_dir, sizeof(fsa->log_dir), "%s/%s", g_options.output_dir, last_octet);
    fsa->log_mempool_buf = malloc(LOG_MEMPOOL_SIZE);
    if (fsa->log_mempool_buf == NULL) {
        print_error(true, " [%s] unable to allocate log memory pool", fsa->ip);
        return false;
    }

    fsa->log_mempool.buf = fsa->log_mempool_buf;
    fsa->log_mempool.cap = LOG_MEMPOOL_SIZE;
    mempool_init(&fsa->log_mempool);

    const struct log_cfg log_cfg = {
        .e_mode      = LOG_MODE_SYNC,
        .e_level     = LOG_LEVEL_DATA,
        .e_format    = LOG_FORMAT_CSV,
        .mempool     = &fsa->log_mempool,
        .file_path   = fsa->log_dir,
        .file_header = g_log_file_header,
        .file_size   = (size_t)g_options.rotate_size,
        .max_files   = g_options.max_files,
        .e_ring      = LOG_RING_ROTATE,
        .chunk_size  = LOG_CHUNK_SIZE,
        .flush_cap   = LOG_FLUSH_BUF_SIZE,
        .nproducers  = 1U, // 每个 IP 一个日志器且只有一个采集线程写入
        .f_get_ts    = get_real_ts_us,
    };
    log_init(&fsa->log, log_cfg);
    if (fsa->log.lo.mmap_ptr == NULL) {
        print_error(
            true, " [%s] cannot initialize rotating log directory '%s'", fsa->ip, fsa->log_dir);
        log_deinit(&fsa->log);
        free(fsa->log_mempool_buf);
        fsa->log_mempool_buf = NULL;
        return false;
    }

    fsa->log_initialized = true;
    return true;
}

static bool
write_record(struct fsa                   *fsa,
             const struct foc_ref_pvct    *ref_pvct,
             const struct foc_fdb_pvct    *fdb_pvct,
             const struct optional_sample *optional,
             uint64_t                      latency_us)
{
    char   record[LOG_RECORD_LENGTH];
    size_t length = 0U;

    if (g_options.include_pvct) {
        if (!append_format(record,
                           sizeof(record),
                           &length,
                           "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f",
                           ref_pvct->pos,
                           fdb_pvct->pos,
                           ref_pvct->vel,
                           fdb_pvct->vel,
                           ref_pvct->cur,
                           fdb_pvct->cur,
                           ref_pvct->tor,
                           fdb_pvct->elec_tor)) {
            return false;
        }
    }
    if (g_options.include_control && !append_format(record,
                                                    sizeof(record),
                                                    &length,
                                                    "%s%u,%u,%u",
                                                    length == 0U ? "" : ",",
                                                    optional->control_mode,
                                                    optional->control_word,
                                                    optional->work_mode)) {
        return false;
    }
    if (g_options.include_pid && !append_format(record,
                                                sizeof(record),
                                                &length,
                                                "%s%.3f,%.3f,%.3f,%.3f,%.3f",
                                                length == 0U ? "" : ",",
                                                optional->p_kp,
                                                optional->v_kp,
                                                optional->v_ki,
                                                optional->pd_kp,
                                                optional->pd_kd)) {
        return false;
    }
    if (g_options.include_power_status && !append_format(record,
                                                         sizeof(record),
                                                         &length,
                                                         "%s%.3f,%.3f,%.3f",
                                                         length == 0U ? "" : ",",
                                                         optional->temp_mos,
                                                         optional->temp_coil,
                                                         optional->vbus)) {
        return false;
    }
    if (!append_format(record,
                       sizeof(record),
                       &length,
                       "%s%llu",
                       length == 0U ? "" : ",",
                       (unsigned long long)latency_us)) {
        return false;
    }

    log_data(&fsa->log, 0U, "%s\n", record);
    log_flush(&fsa->log);
    return fsa->log.lo.mmap_ptr != NULL;
}

static uint64_t
next_deadline_us(uint64_t started_us, uint32_t frequency_hz)
{
    const uint64_t period_us = 1000000U / frequency_hz;

    if (UINT64_MAX - started_us < period_us) {
        return UINT64_MAX;
    }
    return started_us + period_us;
}

static void
wait_until(uint64_t deadline_us)
{
    while (!g_stop_requested) {
        if (fsa_error_stop_delay_elapsed()) {
            return;
        }
        const uint64_t error_stop_deadline_us = ATOMIC_LOAD(&g_error_stop_deadline_us);
        if (error_stop_deadline_us != 0U && error_stop_deadline_us < deadline_us) {
            deadline_us = error_stop_deadline_us;
        }

        const uint64_t now_us = get_mono_ts_us();
        if (now_us >= deadline_us) {
            return;
        }
        const uint64_t remaining_us = deadline_us - now_us;
        const uint64_t sleep_us     = remaining_us > 50000U ? 50000U : remaining_us;
        if (sleep_us >= 1000U) {
            delay_ms(sleep_us / 1000U, DELAY_YIELD);
        } else {
            delay_us(sleep_us);
        }
    }
}

static void *
fsa_thread_func(void *argument)
{
    struct fsa *fsa                     = argument;
    bool        data_was_failing        = false;
    bool        error_read_was_failing  = false;
    uint64_t    next_sample_deadline_us = get_mono_ts_us();
    uint64_t    next_error_deadline_us  = next_sample_deadline_us;

    while (!g_stop_requested &&
           (g_options.sample_count == 0U || fsa->attempts < g_options.sample_count)) {
        if (fsa_error_stop_delay_elapsed()) {
            break;
        }

        uint64_t now_us = get_mono_ts_us();
        if (now_us >= next_sample_deadline_us) {
            struct foc_fdb_pvct    fdb_pvct   = {0};
            struct foc_ref_pvct    ref_pvct   = {0};
            struct optional_sample optional   = {0};
            const uint64_t         started_us = now_us;
            int                    ret        = fsa_get_sample_data(fsa, &fdb_pvct, &optional);

            if (ret >= 0 && g_options.include_pvct) {
                ret = fsa_get_ref_pvct(fsa, &ref_pvct);
            }
            fsa->attempts++;
            if (ret >= 0) {
                if (!write_record(
                        fsa, &ref_pvct, &fdb_pvct, &optional, get_mono_ts_us() - started_us)) {
                    print_error(true, " [%s] failed to write rotating log", fsa->ip);
                    g_stop_requested = 1;
                    break;
                }
                fsa->records++;
                if (data_was_failing) {
                    if (!g_options.quiet) {
                        print_success(true, " [%s] data communication restored", fsa->ip);
                    }
                    data_was_failing = false;
                }
            } else {
                fsa->errors++;
                if (!data_was_failing || fsa->errors % 10U == 0U) {
                    print_warn(true,
                               " [%s] data read failed (error %d, failures: %llu)",
                               fsa->ip,
                               ret,
                               (unsigned long long)fsa->errors);
                }
                data_was_failing = true;
            }
            next_sample_deadline_us = next_deadline_us(started_us, g_options.frequency_hz);
        }

        if (g_stop_requested ||
            (g_options.sample_count != 0U && fsa->attempts >= g_options.sample_count)) {
            break;
        }

        now_us = get_mono_ts_us();
        if (now_us >= next_error_deadline_us) {
            uint32_t       error_codes[FSA_ERROR_CODE_COUNT] = {0U};
            const uint64_t started_us                        = now_us;
            const int      ret = fsa_get_error_codes(fsa, error_codes);

            if (ret >= 0) {
                if (check_fsa_error_codes(fsa, error_codes)) {
                    break;
                }
                if (error_read_was_failing) {
                    if (!g_options.quiet) {
                        print_success(true, " [%s] error-code communication restored", fsa->ip);
                    }
                    error_read_was_failing = false;
                }
            } else {
                fsa->error_read_errors++;
                if (!error_read_was_failing || fsa->error_read_errors % 10U == 0U) {
                    print_warn(true,
                               " [%s] error-code read failed (error %d, failures: %llu)",
                               fsa->ip,
                               ret,
                               (unsigned long long)fsa->error_read_errors);
                }
                error_read_was_failing = true;
            }
            next_error_deadline_us = next_deadline_us(started_us, g_options.error_frequency_hz);
        }

        if (next_sample_deadline_us < next_error_deadline_us) {
            wait_until(next_sample_deadline_us);
        } else {
            wait_until(next_error_deadline_us);
        }
    }
    return NULL;
}

int
main(int argc, char **argv)
{
    const int parse_result = parse_options(argc, argv, &g_options);
    if (parse_result > 0) {
        return 0;
    }
    if (parse_result < 0) {
        print_info(true, " Try '%s --help' for usage.", TOOL_NAME);
        return 2;
    }
    if (!build_log_file_header()) {
        print_error(true, " failed to build CSV header");
        return 1;
    }

    signal(SIGINT, signal_handler);
#ifdef SIGTERM
    signal(SIGTERM, signal_handler);
#endif
    if (ensure_directory(g_options.output_dir) != 0) {
        return 1;
    }

    const int net_result = net_init(&g_net, g_net_cfg);
    if (net_result < 0) {
        print_error(true, " failed to initialize networking (%d)", net_result);
        return 1;
    }

    g_fsas = calloc(g_options.ip_count, sizeof(*g_fsas));
    if (g_fsas == NULL) {
        print_error(true, " unable to allocate device state");
        net_destroy(&g_net);
        return 1;
    }

    if (!g_options.quiet) {
        print_info(true, " %s %s", TOOL_NAME, FSA_RT_LOG_VER_STRING);
        print_info(true, " output directory: %s", g_options.output_dir);
        print_info(true,
                   " FSA error stop delay: %llu s",
                   (unsigned long long)g_options.error_stop_delay_s);
        if (g_options.include_pvct) {
            print_info(true, " CSV group enabled: pvct");
        }
        if (g_options.include_control) {
            print_info(true, " CSV group enabled: control");
        }
        if (g_options.include_pid) {
            print_info(true, " CSV group enabled: pid");
        }
        if (g_options.include_power_status) {
            print_info(true, " CSV group enabled: power-status");
        }
        for (size_t i = 0U; i < FSA_ERROR_CODE_COUNT; i++) {
            if (g_options.ignored_error_masks[i] != 0U) {
                print_info(true,
                           " ignored FSA error mask: ERROR%u=0x%08X",
                           (unsigned int)(i + 1U),
                           g_options.ignored_error_masks[i]);
            }
        }
    }
    size_t started_count = 0U;
    for (size_t i = 0U; i < g_options.ip_count; i++) {
        struct fsa *fsa         = &g_fsas[i];
        const int   init_result = fsa_init(fsa, g_options.ips[i], g_options.port);

        if (init_result < 0) {
            print_error(
                true, " [%s] failed to create UDP channel (%d)", g_options.ips[i], init_result);
            continue;
        }
        if (!init_device_log(fsa)) {
            continue;
        }
        const int thread_result = pthread_create(&fsa->thread, NULL, fsa_thread_func, fsa);
        if (thread_result != 0) {
            print_error(true, " [%s] failed to create worker thread (%d)", fsa->ip, thread_result);
            continue;
        }
        fsa->thread_started = true;
        started_count++;
        if (!g_options.quiet) {
            print_success(true,
                          " [%s:%u] %s, frequency %u Hz, error frequency %u Hz, timeout %u us",
                          fsa->ip,
                          g_options.port,
                          fsa->log_dir,
                          g_options.frequency_hz,
                          g_options.error_frequency_hz,
                          g_options.timeout_us);
        }
    }

    if (started_count == 0U) {
        print_error(true, " no device workers could be started");
        g_stop_requested = 1;
    }
    for (size_t i = 0U; i < g_options.ip_count; i++) {
        if (g_fsas[i].thread_started) {
            pthread_join(g_fsas[i].thread, NULL);
        }
    }

    uint64_t total_records           = 0U;
    uint64_t total_errors            = 0U;
    uint64_t total_error_read_errors = 0U;
    for (size_t i = 0U; i < g_options.ip_count; i++) {
        total_records           += g_fsas[i].records;
        total_errors            += g_fsas[i].errors;
        total_error_read_errors += g_fsas[i].error_read_errors;
        if (g_fsas[i].log_initialized) {
            log_deinit(&g_fsas[i].log);
            g_fsas[i].log_initialized = false;
        }
        if (g_fsas[i].log_mempool_buf != NULL) {
            free(g_fsas[i].log_mempool_buf);
            g_fsas[i].log_mempool_buf = NULL;
        }
    }
    if (!g_options.quiet) {
        print_info(true,
                   " stopped: %llu records, %llu data read failures, %llu error-code read failures",
                   (unsigned long long)total_records,
                   (unsigned long long)total_errors,
                   (unsigned long long)total_error_read_errors);
    }

    net_destroy(&g_net);
    free(g_fsas);
    return started_count == 0U ? 1 : 0;
}
