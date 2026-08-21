#include "dsox2024a.h"
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>
static void
sleep_10_seconds(void)
{
        Sleep(10000);
}
#else
#include <unistd.h>
static void
sleep_10_seconds(void)
{
        sleep(10);
}
#endif

int
main(int argc, char **argv)
{
        const char *resource = argc == 2 ? argv[1] : NULL;
        if (argc > 2) {
                fprintf(stderr, "usage: %s [visa-resource]\n", argv[0]);
                return 2;
        }
        if (dsox2024a_open_usb(resource, 30000)) {
                fprintf(stderr, "connect: %s\n", dsox2024a_last_error());
                return 1;
        }
        char id[256];
        if (!dsox2024a_identify(id, sizeof(id)))
                printf("%s\n", id);
        if (dsox2024a_set_channel_enabled(1, 1))
                goto error;
        /* AUTO sweep guarantees a trigger even when no valid edge is present. */
        if (dsox2024a_write(":TRIGger:SWEep AUTO"))
                goto error;
        printf("Triggering SINGLE every 10 seconds. Press Ctrl+C to stop.\n");
        for (;;) {
                char completed[16];
                if (dsox2024a_single())
                        goto error;
                /* Wait until this single acquisition has completed. */
                if (dsox2024a_query("*OPC?", completed, sizeof(completed)))
                        goto error;
                printf("SINGLE acquisition complete.\n");
                sleep_10_seconds();
        }
error:
        fprintf(stderr, "DSOX2024A: %s\n", dsox2024a_last_error());
        dsox2024a_close();
        return 1;
}
