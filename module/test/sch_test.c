#include <stdio.h>

#include "module.h"

sch_t cfs;

u64 fake_time_us = 0;

u64
get_ts_us(void)
{
        return fake_time_us;
}

void
task_cb(void *arg)
{
        const char *name = arg;
        println("[%.6llu] task %s executed", fake_time_us, name);
}

int
main(void)
{
        const sch_cfg_t sch_cfg = {
            .cpu_id   = 19,
            .e_type   = SCH_TYPE_CFS,
            .f_get_ts = get_ts_us,
        };
        sch_init(&cfs, sch_cfg);

        const sch_task_cfg_t tasks[] = {
            {
                .id            = 0,
                .priority      = 1,
                .exec_freq     = 1000,
                .exec_cnt_max  = 3,
                .init_delay_us = 0,
                .f_exec        = task_cb,
                .arg           = "A",
            },
            {
                .id            = 1,
                .priority      = 1,
                .exec_freq     = 500,
                .exec_cnt_max  = 5,
                .init_delay_us = 0,
                .f_exec        = task_cb,
                .arg           = "B",
            },
            {
                .id            = 2,
                .priority      = 1,
                .exec_freq     = 800,
                .exec_cnt_max  = 2,
                .init_delay_us = 0,
                .f_exec        = task_cb,
                .arg           = "C",
            },
        };

        for (int i = 0; i < 3; i++)
                sch_add_task(&cfs, tasks[i]);

        for (int step = 0; step < 10; step++) {
                fake_time_us += 500;

                println("\n--- CFS step %d ---", step);
                sch_exec(&cfs);
        }

        return 0;
}
