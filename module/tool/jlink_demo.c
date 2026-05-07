#include "module.h"

#include <signal.h>

#include "jlink_demo.h"

// 2. 定义全局运行标志位，使用 sig_atomic_t 保证信号中断时的读写原子性
static volatile sig_atomic_t g_running = 1;

// 3. 定义 Ctrl+C (SIGINT) 信号处理函数
static void
sigint_handler(int signum)
{
        (void)signum;
        g_running = 0; // 按下 Ctrl+C 时，清除运行标志
}

int
jlink_set_word(const comm_shm_word_e word, const u32 addr)
{
        return jlink_port_write_mem(addr, sizeof(u32), &word);
}

int
jlink_set_mode(const comm_shm_mode_e mode, const u32 addr)
{
        return jlink_port_write_mem(addr, sizeof(u32), &mode);
}

int
jlink_set_pvct(const foc_ref_pvct_t *pvct, u32 pvct_addr)
{
        if (pvct == NULL)
                return -1;
        return jlink_port_write_mem(pvct_addr, sizeof(foc_ref_pvct_t), pvct);
}

wave_t     wave;
wave_cfg_t wave_cfg = {
    .fs        = K(2.0f),
    .wave_freq = 5.0f,
    .amp       = 1000.0f,
    .type      = WAVE_TYPE_SINE,
};

int
main(void)
{
        // 4. 注册 SIGINT (Ctrl+C) 信号回调函数
        signal(SIGINT, sigint_handler);

        const char *dll_path = "C:\\Program Files\\SEGGER\\JLink\\JLink_x64.dll";

        print_info(TRUE, "init J-Link port...");
        if (jlink_port_init(dll_path, "STM32H745II", 50000, 0, false) < 0) {
                print_error(TRUE, "J-Link init failed! Please check DLL path or hardware");
                return -1;
        }

        wave_init(&wave, wave_cfg);

        print_info(TRUE, "Running... Press Ctrl+C to exit.");

        u64 ret = 0;

        while (g_running) {
                // TIMED_EXEC(ret, 500, {
                //         wave_exec(&wave);
                //         jlink_port_write_mem(0x20000070, 4, &wave.out.val);
                //         print_info(TRUE, "elapsed: %llu us, cnt: %f", ret, wave.out.val);
                // });

                TIMED_EXEC(ret, S2US(5), { jlink_port_reset(); });
        }

        print_info(TRUE, "\nCtrl+C received, exiting safely...");

        // 6. 安全释放 J-Link 动态库和连接资源
        jlink_port_deinit();
        print_info(TRUE, "J-Link deinit complete.");

        return 0;
}
