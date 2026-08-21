#include "platdef.h"

#if OS(LINUX)
#define _GNU_SOURCE
#endif

#if OS(POSIX)
#include <pthread.h>
#endif
#if OS(LINUX)
#include <sched.h>
#endif
#if OS(MAC)
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#endif
#if OS(WIN)
#include <windows.h>
#endif

#include <stdio.h>

#include "bitops.h"
#include "errdef.h"
#include "mathdef.h"
#include "printops.h"
#include "sch.h"

/* -------------------------------------------------------------------------- */
/*                                  内部函数                                  */
/* -------------------------------------------------------------------------- */

#if OS(POSIX)
static void *
sch_thread_exec(void *arg)
{
    struct sch *t = (struct sch *)arg;
    for (;;)
        sch_exec(t);

    return NULL;
}

static void
sch_bind_thread_to_cpu(pthread_t thread_tid, const int cpu_id)
{
#if OS(LINUX)
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(cpu_id, &cpu_set);
    int ret = pthread_setaffinity_np(thread_tid, sizeof(cpu_set_t), &cpu_set);
    if (ret)
        print_error(false, "[SCH] set thread affinity failed, errcode: %d", ret);

    print_error(false, "[SCH] bind thread to CPU %d success", cpu_id);
#elif OS(MAC)
    /* macOS pthread 不支持硬亲和性, thread_policy 仅提供调度提示. */
    thread_affinity_policy_data_t policy      = {.affinity_tag = cpu_id + 1};
    const mach_port_t             mach_thread = pthread_mach_thread_np(thread_tid);
    const kern_return_t           ret         = thread_policy_set(mach_thread,
                                                THREAD_AFFINITY_POLICY,
                                                (thread_policy_t)&policy,
                                                THREAD_AFFINITY_POLICY_COUNT);
    if (ret != KERN_SUCCESS)
        print_error(false, "[SCH] set thread affinity hint failed, errcode: %d", ret);
    else
        print_error(false, "[SCH] set thread affinity hint to tag %d", cpu_id + 1);
#else
    ARG_UNUSED(thread_tid);
    ARG_UNUSED(cpu_id);
#endif
}
#elif OS(WIN)
static DWORD WINAPI
sch_thread_exec(LPVOID arg)
{
    struct sch *t = (struct sch *)arg;
    for (;;)
        sch_exec(t);

    return 0;
}

static void
sch_bind_thread_to_cpu(HANDLE thread_handle, const int cpu_id)
{
    const DWORD_PTR mask = (DWORD_PTR)1 << cpu_id;
    const DWORD_PTR ret  = SetThreadAffinityMask(thread_handle, mask);
    if (!ret)
        print_error(false, "[SCH] set thread affinity failed, errcode: %lu", GetLastError());

    print_error(false, "[SCH] bind thread to CPU %d success", cpu_id);
}
#endif

static void
sch_thread_init(void *arg, const int cpu_id)
{
    ARG_UNUSED(arg);
    ARG_UNUSED(cpu_id);

#if OS(POSIX)
    pthread_t sch_tid;
    int       ret = pthread_create(&sch_tid, NULL, sch_thread_exec, arg);
    if (ret != 0) {
        print_error(false, "[SCH] create thread failed, errcode: %d", ret);
        return;
    }
    sch_bind_thread_to_cpu(sch_tid, cpu_id);
#elif OS(WIN)
    DWORD  thread_id;
    HANDLE sch_tid = CreateThread(NULL,            // 默认安全属性
                                  0,               // 默认堆栈大小
                                  sch_thread_exec, // 线程函数
                                  arg,             // 传递给线程函数的参数
                                  0,               // 默认创建标志
                                  &thread_id       // 用于接收线程ID
    );
    if (sch_tid == NULL) {
        print_error(false, "[SCH] create thread failed, errcode: %lu", GetLastError());
        return;
    }
    sch_bind_thread_to_cpu(sch_tid, cpu_id);
#endif
}

static int
sch_cfs_task_cmp(const struct sch_task *a, const struct sch_task *b)
{
    if (a->status.next_exec_ts < b->status.next_exec_ts)
        return -1;
    if (a->status.next_exec_ts > b->status.next_exec_ts)
        return 1;
    if (a->cfg.priority < b->cfg.priority)
        return -1;
    if (a->cfg.priority > b->cfg.priority)
        return 1;
    if (a->cfg.id < b->cfg.id)
        return -1;
    if (a->cfg.id > b->cfg.id)
        return 1;

    return 0;
}

static void
sch_time_insert_task(struct rb_root *rb_root, struct sch_task *task)
{
    if (task->in_time_queue)
        return;

    struct rb_node **new_rb_node = &rb_root->rb_node;
    struct rb_node  *rb_parent   = NULL;

    if (task->cfg.id >= SCH_TASK_MAX)
        return;

    while (*new_rb_node) {
        const struct sch_task *curr = CONTAINER_OF(*new_rb_node, struct sch_task, rb_node);
        const int              cmp  = sch_cfs_task_cmp(task, curr);
        rb_parent                   = *new_rb_node;
        new_rb_node = (cmp < 0) ? &(*new_rb_node)->rb_left : &(*new_rb_node)->rb_right;
    }
    rb_link_node(&task->rb_node, rb_parent, new_rb_node);
    rb_insert_color(&task->rb_node, rb_root);
    task->in_time_queue = true;
}

static void
sch_time_remove_task(struct rb_root *rb_root, struct sch_task *task)
{
    if (task->in_time_queue) {
        rb_erase(&task->rb_node, rb_root);
        memset(&task->rb_node, 0, sizeof(struct rb_node));
        task->in_time_queue = false;
    }
}

static struct sch_task *
sch_cfs_get_task(struct sch *sch)
{
    DECL(sch, lo);

    const struct rb_root *rb_root = &lo->algo_ctx.cfs.rb_root;
    struct rb_node       *rb_node = rb_first(rb_root);
    if (!rb_node)
        return NULL;

    return CONTAINER_OF(rb_node, struct sch_task, rb_node);
}

static struct sch_task *
sch_fcfs_get_task(struct sch *sch)
{
    DECL(sch, lo);

    const size_t prev_idx = lo->algo_ctx.fcfs.prev_idx;
    for (size_t i = 0; i < lo->ntasks; ++i) {
        const size_t     idx = (prev_idx + i) % lo->ntasks;
        struct sch_task *t   = &lo->tasks[idx];
        if (t->status.e_state == SCH_TASK_STATE_RUN && t->status.next_exec_ts <= lo->curr_ts) {
            lo->algo_ctx.fcfs.prev_idx = idx + 1;
            return t;
        }
    }
    return NULL;
}

static void
sch_cfs_insert_task(struct sch *sch, struct sch_task *task)
{
    sch_time_insert_task(&sch->lo.algo_ctx.cfs.rb_root, task);
}

static void
sch_cfs_remove_task(struct sch *sch, struct sch_task *task)
{
    sch_time_remove_task(&sch->lo.algo_ctx.cfs.rb_root, task);
}

static void
sch_fixed_ready_insert(struct sch *sch, struct sch_task *task)
{
    DECL(sch, lo);
    const uint32_t priority = task->cfg.priority;
    if (priority >= SCH_PRIORITY_MAX || task->in_ready_queue)
        return;

    list_add_tail(&task->ready_node, &lo->algo_ctx.fixed_priority.ready_queues[priority]);
    lo->algo_ctx.fixed_priority.ready_bitmap |= (uint32_t)1U << (31U - priority);
    task->in_ready_queue                      = true;
}

static void
sch_fixed_ready_remove(struct sch *sch, struct sch_task *task)
{
    DECL(sch, lo);
    const uint32_t priority = task->cfg.priority;
    if (priority >= SCH_PRIORITY_MAX || !task->in_ready_queue)
        return;

    list_del(&task->ready_node);
    list_init(&task->ready_node);
    task->in_ready_queue = false;
    if (list_empty(&lo->algo_ctx.fixed_priority.ready_queues[priority]))
        lo->algo_ctx.fixed_priority.ready_bitmap &= ~((uint32_t)1U << (31U - priority));
}

static void
sch_fixed_timer_insert(struct sch *sch, struct sch_task *task)
{
    sch_time_insert_task(&sch->lo.algo_ctx.fixed_priority.timer_root, task);
}

static void
sch_fixed_timer_remove(struct sch *sch, struct sch_task *task)
{
    sch_time_remove_task(&sch->lo.algo_ctx.fixed_priority.timer_root, task);
}

static void
sch_fixed_promote_due_tasks(struct sch *sch)
{
    DECL(sch, lo);
    for (;;) {
        struct rb_node *node = rb_first(&lo->algo_ctx.fixed_priority.timer_root);
        if (!node)
            return;
        struct sch_task *task = CONTAINER_OF(node, struct sch_task, rb_node);
        if (task->status.next_exec_ts > lo->curr_ts)
            return;
        sch_fixed_timer_remove(sch, task);
        sch_fixed_ready_insert(sch, task);
    }
}

static struct sch_task *
sch_fixed_get_task(struct sch *sch)
{
    DECL(sch, lo);
    sch_fixed_promote_due_tasks(sch);
    const uint32_t bitmap = lo->algo_ctx.fixed_priority.ready_bitmap;
    if (!bitmap)
        return NULL;
    const uint32_t priority = clz32(bitmap);
    return LIST_FIRST_ENTRY(
        &lo->algo_ctx.fixed_priority.ready_queues[priority], struct sch_task, ready_node);
}

static void
sch_remove_queued_task(struct sch *sch, struct sch_task *task)
{
    if (sch->cfg.e_type == SCH_TYPE_CFS)
        sch_cfs_remove_task(sch, task);
    else if (sch->cfg.e_type == SCH_TYPE_FIXED_PRIORITY) {
        sch_fixed_timer_remove(sch, task);
        sch_fixed_ready_remove(sch, task);
    }
}

static void
sch_insert_runnable_task(struct sch *sch, struct sch_task *task)
{
    if (task->status.e_state != SCH_TASK_STATE_RUN)
        return;
    if (sch->cfg.e_type == SCH_TYPE_CFS)
        sch_cfs_insert_task(sch, task);
    else if (sch->cfg.e_type == SCH_TYPE_FIXED_PRIORITY)
        sch_fixed_timer_insert(sch, task);
}

/* -------------------------------------------------------------------------- */
/*                                  接口定义                                  */
/* -------------------------------------------------------------------------- */

int
sch_init(struct sch *sch, const struct sch_cfg sch_cfg)
{
    if (!sch || !sch_cfg.f_get_ts)
        return -MEINVAL;

    memset(&sch->lo, 0, sizeof(sch->lo));
    memset(&sch->tmp, 0, sizeof(sch->tmp));
    CFG_INIT(sch, sch_cfg);
    DECL(sch, cfg, lo);

    switch (cfg->e_type) {
        case SCH_TYPE_FCFS: {
            lo->f_get_task = sch_fcfs_get_task;
            break;
        }
        case SCH_TYPE_CFS: {
            lo->f_get_task = sch_cfs_get_task;
            break;
        }
        case SCH_TYPE_FIXED_PRIORITY: {
            lo->f_get_task = sch_fixed_get_task;
            for (uint32_t i = 0; i < SCH_PRIORITY_MAX; ++i)
                list_init(&lo->algo_ctx.fixed_priority.ready_queues[i]);
            break;
        }
        default:
            return -MEINVAL;
    }

    return 0;
}

int
sch_run(struct sch *sch)
{
    DECL(sch, cfg);
    sch_thread_init(sch, cfg->cpu_id);
    return 0;
}

int
sch_exec(struct sch *sch)
{
    DECL(sch, cfg, lo, tmp);

    lo->curr_ts = cfg->f_get_ts();

    for (;;) {
        struct sch_task *task = lo->f_get_task(sch);
        if (!task)
            return 0;
        if (!task->cfg.f_exec)
            return -MEINVAL;

        if (task->status.next_exec_ts > lo->curr_ts)
            return 0;

        sch_remove_queued_task(sch, task);

        const uint64_t start_ts = lo->curr_ts;
        task->cfg.f_exec(task->cfg.arg);
        const uint64_t end_ts = cfg->f_get_ts();

        task->status.exec_cnt++;
        task->status.elapsed_us     = (uint32_t)(end_ts - start_ts);
        task->status.elapsed_us_max = MAX(task->status.elapsed_us_max, task->status.elapsed_us);
        if (task->cfg.budget_us && task->status.elapsed_us > task->cfg.budget_us)
            task->status.overrun_cnt++;

        if (task->cfg.exec_cnt_max == 0 || task->status.exec_cnt < task->cfg.exec_cnt_max) {
            task->status.next_exec_ts = end_ts + (size_t)HZ2US(task->cfg.exec_freq);
            sch_insert_runnable_task(sch, task);
        } else {
            task->status.e_state = SCH_TASK_STATE_DEAD;
            if (task->cfg.f_deinit)
                task->cfg.f_deinit(task->cfg.arg);
        }

        tmp->elapsed_us =
            (int64_t)(end_ts - lo->curr_ts) > 0 ? (uint32_t)(end_ts - lo->curr_ts) : 0;
        lo->elapsed_us_max   = MAX(lo->elapsed_us_max, tmp->elapsed_us);
        tmp->prev_elapsed_us = tmp->elapsed_us;
        lo->curr_ts          = end_ts;
    }
}

int
sch_add_task(struct sch *sch, const struct sch_task_cfg task_cfg)
{
    DECL(sch, cfg, lo);

    if (lo->ntasks >= SCH_TASK_MAX || !task_cfg.f_exec || task_cfg.exec_freq <= 0.0F ||
        (sch->cfg.e_type == SCH_TYPE_FIXED_PRIORITY && task_cfg.priority >= SCH_PRIORITY_MAX))
        return -MEINVAL;

    struct sch_task *task     = &lo->tasks[lo->ntasks];
    task->cfg                 = task_cfg;
    task->status.e_state      = task_cfg.e_init_state;
    task->status.next_exec_ts = cfg->f_get_ts() + task->cfg.init_delay_us;
    list_init(&task->ready_node);

    lo->ntasks++;

    if (task->status.e_state != SCH_TASK_STATE_DEAD && task->cfg.f_init)
        task->cfg.f_init(task->cfg.arg);

    sch_insert_runnable_task(sch, task);
    return 0;
}

int
sch_reinit_tasks(struct sch *sch)
{
    DECL(sch, cfg, lo);

    for (size_t i = 0; i < lo->ntasks; i++) {
        struct sch_task *task = &lo->tasks[i];
        sch_remove_queued_task(sch, task);
        task->status.next_exec_ts = cfg->f_get_ts() + task->cfg.init_delay_us;
        if (task->cfg.f_init)
            task->cfg.f_init(task->cfg.arg);

        sch_insert_runnable_task(sch, task);
    }
    return 0;
}

int
sch_set_task_freq(struct sch *sch, const size_t id, const size_t exec_freq)
{
    DECL(sch, cfg, lo);

    for (size_t i = 0; i < lo->ntasks; i++) {
        struct sch_task *task = &lo->tasks[i];
        if (task->cfg.id == id) {
            if (!exec_freq)
                return -MEINVAL;
            sch_remove_queued_task(sch, task);
            task->cfg.exec_freq       = (float32_t)exec_freq;
            task->status.next_exec_ts = cfg->f_get_ts() + (size_t)HZ2US(exec_freq);
            sch_insert_runnable_task(sch, task);
            return 0;
        }
    }
    return -MEINVAL;
}

int
sch_set_task_state(struct sch *sch, const size_t id, const enum sch_task_state e_state)
{
    DECL(sch, cfg, lo);

    for (size_t i = 0; i < lo->ntasks; i++) {
        struct sch_task *task = &lo->tasks[i];
        if (task->cfg.id == id) {
            if (task->status.e_state == e_state)
                return -MEXIST;

            sch_remove_queued_task(sch, task);

            if (e_state == SCH_TASK_STATE_DEAD) {
                task->status.exec_cnt = 0;
                if (task->cfg.f_deinit)
                    task->cfg.f_deinit(task->cfg.arg);
            }

            if (task->status.e_state == SCH_TASK_STATE_DEAD) {
                if (task->cfg.f_init)
                    task->cfg.f_init(task->cfg.arg);
            }

            task->status.next_exec_ts = cfg->f_get_ts() + task->cfg.init_delay_us;
            task->status.e_prev_state = task->status.e_state;
            task->status.e_state      = e_state;
            sch_insert_runnable_task(sch, task);
            return 0;
        }
    }
    return -MEINVAL;
}
