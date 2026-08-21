#include "dsox2024a.h"
#include "fsav3_c.h"
#include "kps6050d.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#define WARNING3_DOUBLE_ENCODER 0x00000008u
static const char *GR3_IP_LIST[] = {
    // /* 左臂 */
    // "192.168.137.10",
    // "192.168.137.11",
    // "192.168.137.12",
    // "192.168.137.13",
    // "192.168.137.14",
    // "192.168.137.15",
    // "192.168.137.16",
    // /* 右臂 */
    // "192.168.137.30",
    // "192.168.137.31",
    // "192.168.137.32",
    // "192.168.137.33",
    // "192.168.137.34",
    // "192.168.137.35",
    // "192.168.137.36",
    /* 左腿 */
//     "192.168.137.70",
    "192.168.137.71",
    "192.168.137.72",
    "192.168.137.73",
    "192.168.137.74",
    "192.168.137.75",
    /* 右腿 */
//     "192.168.137.50",
    "192.168.137.51",
    "192.168.137.52",
    "192.168.137.53",
    "192.168.137.54",
    "192.168.137.55",
    // /* 腰 */
    // "192.168.137.90",
    // "192.168.137.91",
    // "192.168.137.92",
    // /* 头 */
    // "192.168.137.93",
    // "192.168.137.95",
};

static int
kps_ok(int rc, const char *op)
{
        if (!rc)
                return 1;
        fprintf(stderr, "%s: %s\n", op, kps6050d_last_error());
        return 0;
}

static void
warning_beep(void)
{
        for (;;) {
                Beep(1500, 500);
                Sleep(200);
        }
}

int
main(int argc, char **argv)
{
        const char *com  = argc > 1 ? argv[1] : "COM7";
        const char *visa = argc > 2 ? argv[2] : NULL;
        size_t      i;
        int         warning;
        unsigned long long cnt = 0;
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        if (dsox2024a_open_usb(visa, 30000)) {
                fprintf(stderr, "DSOX: %s\n", dsox2024a_last_error());
                return 1;
        }
        if (!kps_ok(kps6050d_open(com, 2400, 0), "KPS open"))
                return 1;
        if (!kps_ok(kps6050d_set_output(0), "KPS off") || !kps_ok(kps6050d_set_voltage(48), "set 48V") ||
            !kps_ok(kps6050d_set_current(5), "set 5A"))
                goto fail;
        for (;;) {
                ++cnt;
                printf("\n========== cycle %llu ==========\n", cnt);
                if (!kps_ok(kps6050d_set_output(1), "KPS on"))
                        goto fail;
                Sleep(10000);
                warning = 0;
                for (i = 0; i < sizeof(GR3_IP_LIST) / sizeof(GR3_IP_LIST[0]); ++i) {
                        uint32_t error3 = 0;
                        int      rc     = fsav3_c_read_error3(GR3_IP_LIST[i], 1000, &error3);
                        if (rc == -302) {
                                printf("%s timeout, skipped\n", GR3_IP_LIST[i]);
                                continue;
                        }
                        if (rc) {
                                printf("%s error %d, skipped\n", GR3_IP_LIST[i], rc);
                                continue;
                        }
                        printf("%s error3=0x%08X\n", GR3_IP_LIST[i], error3);
                        if (error3 & WARNING3_DOUBLE_ENCODER)
                                warning = 1;
                }
                if (warning) {
                        printf("DOUBLE_ENCODER at cycle %llu: keep KPS power ON and stop cycling\n", cnt);
                        warning_beep();
                }
                if (!kps_ok(kps6050d_set_output(0), "KPS off"))
                        goto fail;
                printf("KPS output OFF; waiting 10 seconds before DSOX SINGLE\n");
                Sleep(10000);
                if (dsox2024a_single()) {
                        fprintf(stderr, "DSOX single: %s\n", dsox2024a_last_error());
                        goto fail;
                }
                printf("DSOX SINGLE enabled; starting next power-on cycle\n");
        }
fail:
        kps6050d_set_output(0);
        kps6050d_close();
        dsox2024a_close();
        return 1;
}












