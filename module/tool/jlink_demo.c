#include "module.h"

#include <signal.h>

#include "jlink_demo.h"

static volatile sig_atomic_t g_running = 1;

static void
sigint_handler(int signum)
{
    (void)signum;
    g_running = 0;
}

int
jlink_set_word(const comm_shm_word_e word, const uint32_t addr)
{
    return jlink_port_write_mem(addr, sizeof(uint32_t), &word);
}

int
jlink_set_mode(const comm_shm_mode_e mode, const uint32_t addr)
{
    return jlink_port_write_mem(addr, sizeof(uint32_t), &mode);
}

int
jlink_set_pvct(const struct foc_ref_pvct *pvct, uint32_t pvct_addr)
{
    if (pvct == NULL)
        return -1;
    return jlink_port_write_mem(pvct_addr, sizeof(struct foc_ref_pvct), pvct);
}

struct wave     wave;
struct wave_cfg wave_cfg = {
    .fs        = K(2.0F),
    .wave_freq = 5.0F,
    .amp       = 1000.0F,
    .type      = WAVE_TYPE_SINE,
};

int
main(void)
{
    signal(SIGINT, sigint_handler);

    const char *dll_path = "C:\\Program Files\\SEGGER\\JLink\\JLink_x64.dll";

    print_info(true, "init J-Link port...");
    if (jlink_port_init(dll_path, "STM32H745II", 50000, 0, false) < 0) {
        print_error(true, "J-Link init failed! Please check DLL path or hardware");
        return -1;
    }

    wave_init(&wave, wave_cfg);

    print_info(true, "Running... Press Ctrl+C to exit.");

    uint64_t ret = 0;

    while (g_running) {
        // TIMED_EXEC(ret, 500, {
        //         wave_exec(&wave);
        //         jlink_port_write_mem(0x20000070, 4, &wave.out.val);
        //         print_info(true, "elapsed: %llu us, cnt: %f", ret, wave.out.val);
        // });

        TIMED_EXEC(ret, S2US(5), { jlink_port_reset(); });
    }

    print_info(true, "\nCtrl+C received, exiting safely...");

    jlink_port_deinit();
    print_info(true, "J-Link deinit complete.");

    return 0;
}
