#include <stdio.h>
#include <string.h>

#include "module.h"

#define CHECK(expr)                                                \
    do {                                                           \
        if (!(expr)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            return 1;                                              \
        }                                                          \
    } while (0)

static uint64_t    g_now_us;
static char        g_order[64];
static size_t      g_order_len;
static struct sch *g_init_sch;
static size_t      g_init_task_id;
static int         g_init_freq_ret;

static uint64_t
fake_get_ts(void)
{
    return g_now_us;
}

static void
record_task(void *arg)
{
    g_order[g_order_len++] = *(const char *)arg;
}

static void
slow_task(void *arg)
{
    ARG_UNUSED(arg);
    g_now_us += 7;
}

static void
set_own_frequency(void *arg)
{
    ARG_UNUSED(arg);
    g_init_freq_ret = sch_set_task_freq(g_init_sch, g_init_task_id, 2000);
}

static struct sch_task_cfg
make_task(size_t id, uint32_t priority, float32_t freq, size_t delay_us, char *name)
{
    struct sch_task_cfg cfg = {
        .id            = id,
        .priority      = priority,
        .exec_freq     = freq,
        .exec_cnt_max  = 1,
        .e_init_state  = SCH_TASK_STATE_RUN,
        .init_delay_us = delay_us,
        .f_exec        = record_task,
        .arg           = name,
    };
    return cfg;
}

static int
test_fcfs_skips_not_due_task(void)
{
    struct sch sch = {0};
    char       a = 'A', b = 'B';
    CHECK(sch_init(&sch, (struct sch_cfg){.e_type = SCH_TYPE_FCFS, .f_get_ts = fake_get_ts}) == 0);
    CHECK(sch_add_task(&sch, make_task(0, 0, 1000, 100, &a)) == 0);
    CHECK(sch_add_task(&sch, make_task(1, 0, 1000, 0, &b)) == 0);

    g_now_us    = 0;
    g_order_len = 0;
    CHECK(sch_exec(&sch) == 0);
    CHECK(g_order_len == 1 && g_order[0] == 'B');
    return 0;
}

static int
test_init_can_update_own_frequency(void)
{
    struct sch          sch = {0};
    struct sch_task_cfg cfg = make_task(3, 0, 1000, 0, NULL);
    cfg.f_init              = set_own_frequency;
    cfg.f_exec              = slow_task;
    g_init_sch              = &sch;
    g_init_task_id          = cfg.id;
    g_init_freq_ret         = -1;
    CHECK(sch_init(&sch, (struct sch_cfg){.e_type = SCH_TYPE_FCFS, .f_get_ts = fake_get_ts}) == 0);
    CHECK(sch_add_task(&sch, cfg) == 0);
    CHECK(g_init_freq_ret == 0);
    CHECK(sch.lo.tasks[0].cfg.exec_freq == 2000.0F);
    return 0;
}

static int
test_cfs_state_and_frequency_updates(void)
{
    struct sch sch = {0};
    char       a   = 'A';
    CHECK(sch_init(&sch, (struct sch_cfg){.e_type = SCH_TYPE_CFS, .f_get_ts = fake_get_ts}) == 0);
    CHECK(sch_add_task(&sch, make_task(0, 0, 1000, 0, &a)) == 0);
    CHECK(sch_set_task_state(&sch, 0, SCH_TASK_STATE_STOP) == 0);
    CHECK(sch_exec(&sch) == 0);
    CHECK(sch_set_task_freq(&sch, 0, 500) == 0);
    CHECK(sch_set_task_state(&sch, 0, SCH_TASK_STATE_RUN) == 0);
    CHECK(sch_exec(&sch) == 0);
    CHECK(g_order[g_order_len - 1] == 'A');
    return 0;
}

static int
test_reinit_keeps_callback_frequency_update(void)
{
    struct sch          sch = {0};
    struct sch_task_cfg cfg = make_task(4, 0, 1000, 0, NULL);
    cfg.f_init              = set_own_frequency;
    cfg.f_exec              = slow_task;
    cfg.exec_cnt_max        = 0;
    g_init_sch              = &sch;
    g_init_task_id          = cfg.id;
    CHECK(sch_init(&sch,
                   (struct sch_cfg){.e_type = SCH_TYPE_FIXED_PRIORITY, .f_get_ts = fake_get_ts}) ==
          0);
    CHECK(sch_add_task(&sch, cfg) == 0);
    g_now_us += 100;
    CHECK(sch_reinit_tasks(&sch) == 0);
    CHECK(sch.lo.tasks[0].status.next_exec_ts == g_now_us + 500);
    return 0;
}

static int
test_fixed_priority_order(void)
{
    struct sch sch = {0};
    char       low = 'L', high = 'H', mid = 'M';
    CHECK(sch_init(&sch,
                   (struct sch_cfg){.e_type = SCH_TYPE_FIXED_PRIORITY, .f_get_ts = fake_get_ts}) ==
          0);
    CHECK(sch_add_task(&sch, make_task(0, 20, 1000, 0, &low)) == 0);
    CHECK(sch_add_task(&sch, make_task(1, 1, 1000, 0, &high)) == 0);
    CHECK(sch_add_task(&sch, make_task(2, 10, 1000, 0, &mid)) == 0);

    g_order_len = 0;
    CHECK(sch_exec(&sch) == 0);
    CHECK(g_order_len == 3);
    CHECK(memcmp(g_order, "HML", 3) == 0);
    return 0;
}

static int
test_execution_budget(void)
{
    struct sch          sch = {0};
    struct sch_task_cfg cfg = make_task(0, 0, 1000, 0, NULL);
    cfg.f_exec              = slow_task;
    cfg.exec_cnt_max        = 2;
    cfg.budget_us           = 5;
    CHECK(sch_init(&sch, (struct sch_cfg){.e_type = SCH_TYPE_FCFS, .f_get_ts = fake_get_ts}) == 0);
    CHECK(sch_add_task(&sch, cfg) == 0);
    CHECK(sch_exec(&sch) == 0);
    g_now_us = sch.lo.tasks[0].status.next_exec_ts;
    CHECK(sch_exec(&sch) == 0);
    CHECK(sch.lo.tasks[0].status.elapsed_us == 7);
    CHECK(sch.lo.tasks[0].status.elapsed_us_max == 7);
    CHECK(sch.lo.tasks[0].status.overrun_cnt == 2);
    return 0;
}

int
main(void)
{
    CHECK(test_fcfs_skips_not_due_task() == 0);
    CHECK(test_init_can_update_own_frequency() == 0);
    CHECK(test_cfs_state_and_frequency_updates() == 0);
    CHECK(test_reinit_keeps_callback_frequency_update() == 0);
    CHECK(test_fixed_priority_order() == 0);
    CHECK(test_execution_budget() == 0);
    printf("scheduler tests passed\n");
    return 0;
}
