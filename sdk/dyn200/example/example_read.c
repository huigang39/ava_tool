#include "dyn200.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
        Dyn200Measurement m;
        int baud, address, active, stop_bits;
        if (argc < 4 || argc > 5) {
                fprintf(stderr, "usage: %s <serial-port> <baud> <address|active0|active3> [stop-bits]\n", argv[0]);
                return 2;
        }
        baud = atoi(argv[2]);
        active = (strcmp(argv[3], "active0") == 0 || strcmp(argv[3], "0") == 0) ? 6 :
                 strcmp(argv[3], "active3") == 0 ? 8 : 0;
        address = active ? 1 : atoi(argv[3]);
        stop_bits = argc == 5 ? atoi(argv[4]) : 1;
        if ((active ? (active == 8 ? dyn200_open_active8(argv[1], baud, 1000, stop_bits)
                                  : dyn200_open_active(argv[1], baud, 1000, stop_bits))
                    : dyn200_open_ex(argv[1], baud, address, 1000, stop_bits))) {
                fprintf(stderr, "open: %s\n", dyn200_last_error()); return 1;
        }
        /* Change these if the sensor's display parameters are different. */
        dyn200_set_decimals(2, 0, 0);
        printf("Reading continuously; press Ctrl+C to stop.\n");
        for (;;) {
                if ((active == 6 ? dyn200_read_active6(&m) : active == 8 ? dyn200_read_active8(&m) : dyn200_read(&m))) {
                        fprintf(stderr, "read: %s\n", dyn200_last_error());
                        dyn200_close();
                        return 1;
                }
                printf("torque=%.2f Nm, speed=%.0f rpm, power=%.3f kW\n", m.torque_nm, m.speed_rpm, m.power_kw);
                fflush(stdout);
        }
}
