#include "platdef.h"

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
#include "task_scheduler.h"
#include "timeops.h"

/**
 * @brief                创建调度线程
 * @param[in,out] arg    线程入口参数
 * @param[in]     cpu_id 目标处理器编号
 */
static int sch_thread_init(void *arg, const int cpu_id);

/**
 * @brief       比较任务的下次执行时间
 * @param[in] a 第一个比较对象
 * @param[in] b 第二个比较对象
 */
static int sch_cfs_task_cmp(const struct sch_task *a, const struct sch_task *b);

/**
 * @brief                 将任务插入时间队列
 * @param[in,out] rb_root 任务红黑树根
 * @param[in,out] task    任务对象
 */
static void sch_time_insert_task(struct rb_root *rb_root,
                                 struct sch_task *task);

/**
 * @brief                 从时间队列移除任务
 * @param[in,out] rb_root 任务红黑树根
 * @param[in,out] task    任务对象
 */
static void sch_time_remove_task(struct rb_root *rb_root,
                                 struct sch_task *task);

/**
 * @brief             选择当前应执行的定时任务
 * @param[in,out] sch 调度器对象
 */
static struct sch_task *sch_cfs_get_task(struct sch *sch);

/**
 * @brief             按先来先服务选择任务
 * @param[in,out] sch 调度器对象
 */
static struct sch_task *sch_fcfs_get_task(struct sch *sch);

/**
 * @brief              插入定时调度任务
 * @param[in,out] sch  调度器对象
 * @param[in,out] task 任务对象
 */
static void sch_cfs_insert_task(struct sch *sch, struct sch_task *task);

/**
 * @brief              移除定时调度任务
 * @param[in,out] sch  调度器对象
 * @param[in,out] task 任务对象
 */
static void sch_cfs_remove_task(struct sch *sch, struct sch_task *task);

/**
 * @brief              将任务插入优先级就绪队列
 * @param[in,out] sch  调度器对象
 * @param[in,out] task 任务对象
 */
static void sch_fixed_ready_insert(struct sch *sch, struct sch_task *task);

/**
 * @brief              将任务移出优先级就绪队列
 * @param[in,out] sch  调度器对象
 * @param[in,out] task 任务对象
 */
static void sch_fixed_ready_remove(struct sch *sch, struct sch_task *task);

/**
 * @brief              将等待任务插入时间队列
 * @param[in,out] sch  调度器对象
 * @param[in,out] task 任务对象
 */
static void sch_fixed_timer_insert(struct sch *sch, struct sch_task *task);

/**
 * @brief              将等待任务移出时间队列
 * @param[in,out] sch  调度器对象
 * @param[in,out] task 任务对象
 */
static void sch_fixed_timer_remove(struct sch *sch, struct sch_task *task);

/**
 * @brief             将到期任务移入就绪队列
 * @param[in,out] sch 调度器对象
 */
static void sch_fixed_promote_due_tasks(struct sch *sch);

/**
 * @brief             选择最高优先级的就绪任务
 * @param[in,out] sch 调度器对象
 */
static struct sch_task *sch_fixed_get_task(struct sch *sch);

/**
 * @brief              移除任务所在的调度队列
 * @param[in,out] sch  调度器对象
 * @param[in,out] task 任务对象
 */
static void sch_remove_queued_task(struct sch *sch, struct sch_task *task);

/**
 * @brief              将可运行任务加入调度队列
 * @param[in,out] sch  调度器对象
 * @param[in,out] task 任务对象
 */
static void sch_insert_runnable_task(struct sch *sch, struct sch_task *task);

/**
 * @brief          将任务频率转换为整数周期
 * @param[in] freq 执行频率, 单位: Hz
 * @return         周期, 单位: us, 频率无效或周期超出范围返回 0
 */
static size_t sch_period_us(float32_t freq);

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#if OS(POSIX)
/**
 * @brief             执行调度线程入口
 * @param[in,out] arg 线程入口参数
 */
static void *sch_thread_exec(void *arg);

static void *sch_thread_exec(void *arg)
{
    struct sch *t = (struct sch *)arg;
    for (;;) {
        const int ret = sch_exec(t);
        if (ret < 0) {
            t->lo.check.prev_err = ret;
            print_err(false,
                      "[SCHEDULER] scheduler stopped: %s (%d)",
                      merr_msg(ret),
                      ret);
            break;
        }
    }

    return NULL;
}

/**
 * @brief                设置调度线程的处理器亲和性
 * @param[in] thread_tid 线程标识
 * @param[in] cpu_id     目标处理器编号
 */
static void sch_bind_thread_to_cpu(pthread_t thread_tid, const int cpu_id);

static void sch_bind_thread_to_cpu(pthread_t thread_tid, const int cpu_id)
{
#if OS(LINUX)
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(cpu_id, &cpu_set);
    int ret = pthread_setaffinity_np(thread_tid, sizeof(cpu_set_t), &cpu_set);
    if (ret) {
        print_err(false,
                  "[SCHEDULER] set thread affinity failed, error_code: %d",
                  ret);
    }

    print_err(false, "[SCHEDULER] bind thread to CPU %d success", cpu_id);
#elif OS(MAC)
    // macOS pthread 不支持硬亲和性, thread_policy 仅提供调度提示
    thread_affinity_policy_data_t policy = {.affinity_tag = cpu_id + 1};
    const mach_port_t mach_thread = pthread_mach_thread_np(thread_tid);
    const kern_return_t ret = thread_policy_set(mach_thread,
                                                THREAD_AFFINITY_POLICY,
                                                (thread_policy_t)&policy,
                                                THREAD_AFFINITY_POLICY_CNT);
    if (ret != KERN_SUCCESS) {
        print_err(false,
                  "[SCHEDULER] set thread affinity hint failed, error_code: %d",
                  ret);
    } else {
        print_err(false,
                  "[SCHEDULER] set thread affinity hint to tag %d",
                  cpu_id + 1);
    }
#else
    ARG_UNUSED(thread_tid);
    ARG_UNUSED(cpu_id);
#endif
}
#elif OS(WIN)
static DWORD WINAPI sch_thread_exec(LPVOID arg)
{
    struct sch *t = (struct sch *)arg;
    for (;;) {
        const int ret = sch_exec(t);
        if (ret < 0) {
            t->lo.check.prev_err = ret;
            print_err(false,
                      "[SCHEDULER] scheduler stopped: %s (%d)",
                      merr_msg(ret),
                      ret);
            break;
        }
    }

    return 0;
}

/**
 * @brief                   设置调度线程的处理器亲和性
 * @param[in] thread_handle 线程句柄
 * @param[in] cpu_id        目标处理器编号
 */
static void sch_bind_thread_to_cpu(HANDLE thread_handle, const int cpu_id);

static void sch_bind_thread_to_cpu(HANDLE thread_handle, const int cpu_id)
{
    const DWORD_PTR mask = (DWORD_PTR)1 << cpu_id;
    const DWORD_PTR ret = SetThreadAffinityMask(thread_handle, mask);
    if (!ret) {
        print_err(false,
                  "[SCHEDULER] set thread affinity failed, error_code: %lu",
                  GetLastError());
        return;
    }

    print_err(false, "[SCHEDULER] bind thread to CPU %d success", cpu_id);
}
#endif

int sch_init(struct sch *sch, const struct sch_cfg sch_cfg)
{
    if (!sch || !sch_cfg.get_ts) {
        return MERR(MENULLPTR);
    }

    memset(&sch->lo, 0, sizeof(sch->lo));
    CFG_INIT(sch, sch_cfg);
    DECL(sch, cfg, lo);

    switch (cfg->type) {
    case SCH_TYPE_FCFS: {
        lo->get_task = sch_fcfs_get_task;
        break;
    }
    case SCH_TYPE_CFS: {
        lo->get_task = sch_cfs_get_task;
        break;
    }
    case SCH_TYPE_FIXED_PRIORITY: {
        lo->get_task = sch_fixed_get_task;
        for (uint32_t i = 0; i < SCH_PRIORITY_MAX; ++i) {
            list_init(&lo->algo_ctx.fixed_priority.ready_queues[i]);
        }
        break;
    }
    default: {
        return MERR(MEINVAL);
    }
    }

    return 0;
}

int sch_run(struct sch *sch)
{
    if (!sch) {
        return MERR(MENULLPTR);
    }
    if (!sch->cfg.get_ts || !sch->lo.get_task) {
        return MERR(MESTATE);
    }
    DECL(sch, cfg, lo);

    const int ret = sch_thread_init(sch, cfg->cpu_id);
    lo->check.prev_err = ret;
    return ret;
}

int sch_prev_err(const struct sch *sch)
{
    return sch ? sch->lo.check.prev_err : MERR(MENULLPTR);
}

int sch_exec(struct sch *sch)
{
    if (!sch) {
        return MERR(MENULLPTR);
    }
    if (!sch->cfg.get_ts || !sch->lo.get_task) {
        return MERR(MESTATE);
    }
    DECL(sch, cfg, lo);

    lo->elapsed.loop_cnt++;
    lo->elapsed.curr_ts = cfg->get_ts();

    for (;;) {
        struct sch_task *task = lo->get_task(sch);
        if (!task) {
            return 0;
        }
        if ((!task->cfg.exec && !task->cfg.coro) ||
            sch_period_us(task->cfg.exec_freq) == 0U) {
            return MERR(MEINVAL);
        }

        if (task->elapsed.next_exec_ts > lo->elapsed.curr_ts) {
            return 0;
        }

        sch_remove_queued_task(sch, task);

        const uint64_t start_ts = lo->elapsed.curr_ts;
        enum sch_coro_result coro_result = SCH_CORO_DONE;
        lo->active_task_id = task->cfg.id;
        lo->elapsed.active_start_ts = start_ts;
        lo->task_active = true;
        lo->elapsed.dispatch_cnt++;
        if (task->cfg.coro) {
            coro_result = task->cfg.coro(task, task->cfg.arg);
        } else {
            task->cfg.exec(task->cfg.arg);
        }
        const uint64_t end_ts = cfg->get_ts();

        task->elapsed.exec_cnt++;
        task->elapsed.exec_us = (uint32_t)(end_ts - start_ts);
        if (task->elapsed.exec_us > task->elapsed.exec_us_max) {
            task->elapsed.exec_us_max = task->elapsed.exec_us;
        }
        if (task->cfg.budget_us &&
            task->elapsed.exec_us > task->cfg.budget_us) {
            task->elapsed.overrun_cnt++;
        }

        const bool coro_done = task->cfg.coro && coro_result == SCH_CORO_DONE;
        if (task->status.state != SCH_TASK_STATE_RUN) {
            task->coro.delay_us = 0;
        } else if (!coro_done &&
                   (task->cfg.exec_cnt_max == 0 ||
                    task->elapsed.exec_cnt < task->cfg.exec_cnt_max)) {
            const size_t delay_us = task->coro.delay_us
                                        ? task->coro.delay_us
                                        : sch_period_us(task->cfg.exec_freq);
            task->coro.delay_us = 0;
            task->elapsed.next_exec_ts = end_ts + delay_us;
            sch_insert_runnable_task(sch, task);
        } else {
            task->status.state = SCH_TASK_STATE_DEAD;
            task->coro.resume_line = 0;
            task->coro.delay_us = 0;
            if (task->cfg.deinit) {
                task->cfg.deinit(task->cfg.arg);
            }
        }

        lo->elapsed.exec_us = (int64_t)(end_ts - lo->elapsed.curr_ts) > 0
                                  ? (uint32_t)(end_ts - lo->elapsed.curr_ts)
                                  : 0;
        if (lo->elapsed.exec_us > lo->elapsed.exec_us_max) {
            lo->elapsed.exec_us_max = lo->elapsed.exec_us;
        }
        lo->elapsed.prev_exec_us = lo->elapsed.exec_us;
        lo->elapsed.curr_ts = end_ts;
        lo->task_active = false;
    }
}

int sch_add_task(struct sch *sch, const struct sch_task_cfg task_cfg)
{
    if (!sch) {
        return MERR(MENULLPTR);
    }
    if (!sch->cfg.get_ts || !sch->lo.get_task) {
        return MERR(MESTATE);
    }
    DECL(sch, cfg, lo);

    if (lo->task_cnt >= SCH_TASK_MAX || (!task_cfg.exec && !task_cfg.coro) ||
        (task_cfg.exec && task_cfg.coro) ||
        sch_period_us(task_cfg.exec_freq) == 0U ||
        (uint32_t)task_cfg.init_state > SCH_TASK_STATE_DEAD ||
        (sch->cfg.type == SCH_TYPE_FIXED_PRIORITY &&
         task_cfg.priority >= SCH_PRIORITY_MAX)) {
        return lo->task_cnt >= SCH_TASK_MAX ? MERR(MEOVERFLOW) : MERR(MEINVAL);
    }

    for (size_t i = 0; i < lo->task_cnt; ++i) {
        if (lo->tasks[i].cfg.id == task_cfg.id) {
            return MERR(MEXIST);
        }
    }

    struct sch_task *task = &lo->tasks[lo->task_cnt];
    task->cfg = task_cfg;
    task->status.state = task_cfg.init_state;
    task->elapsed.next_exec_ts = cfg->get_ts() + task->cfg.init_delay_us;
    list_init(&task->ready_node);

    lo->task_cnt++;

    if (task->status.state != SCH_TASK_STATE_DEAD && task->cfg.init) {
        task->cfg.init(task->cfg.arg);
    }

    sch_insert_runnable_task(sch, task);
    return 0;
}

int sch_reinit_tasks(struct sch *sch)
{
    if (!sch) {
        return MERR(MENULLPTR);
    }
    if (!sch->cfg.get_ts || !sch->lo.get_task) {
        return MERR(MESTATE);
    }
    DECL(sch, cfg, lo);

    for (size_t i = 0; i < lo->task_cnt; i++) {
        struct sch_task *task = &lo->tasks[i];
        sch_remove_queued_task(sch, task);
        task->elapsed.next_exec_ts = cfg->get_ts() + task->cfg.init_delay_us;
        task->coro.resume_line = 0;
        task->coro.delay_us = 0;
        if (task->cfg.init) {
            task->cfg.init(task->cfg.arg);
        }

        sch_insert_runnable_task(sch, task);
    }
    return 0;
}

int sch_set_task_freq(struct sch *sch,
                      const size_t id,
                      const float32_t exec_freq)
{
    if (!sch) {
        return MERR(MENULLPTR);
    }
    if (!sch->cfg.get_ts || !sch->lo.get_task) {
        return MERR(MESTATE);
    }
    DECL(sch, cfg, lo);

    for (size_t i = 0; i < lo->task_cnt; i++) {
        struct sch_task *task = &lo->tasks[i];
        if (task->cfg.id == id) {
            const size_t period_us = sch_period_us(exec_freq);
            if (period_us == 0U) {
                return MERR(MEINVAL);
            }
            sch_remove_queued_task(sch, task);
            task->cfg.exec_freq = exec_freq;
            task->elapsed.next_exec_ts = cfg->get_ts() + period_us;
            sch_insert_runnable_task(sch, task);
            return 0;
        }
    }
    return MERR(MENOTFOUND);
}

int sch_set_task_state(struct sch *sch,
                       const size_t id,
                       const enum sch_task_state state)
{
    if (!sch) {
        return MERR(MENULLPTR);
    }
    if (!sch->cfg.get_ts || !sch->lo.get_task) {
        return MERR(MESTATE);
    }
    if (state < SCH_TASK_STATE_RUN || state > SCH_TASK_STATE_DEAD) {
        return MERR(MEINVAL);
    }
    DECL(sch, cfg, lo);

    for (size_t i = 0; i < lo->task_cnt; i++) {
        struct sch_task *task = &lo->tasks[i];
        if (task->cfg.id == id) {
            if (task->status.state == state) {
                return MERR(MEXIST);
            }

            sch_remove_queued_task(sch, task);

            const enum sch_task_state prev_state = task->status.state;
            task->elapsed.next_exec_ts =
                cfg->get_ts() + task->cfg.init_delay_us;
            task->status.prev_state = prev_state;
            task->status.state = state;

            if (state == SCH_TASK_STATE_DEAD) {
                task->elapsed.exec_cnt = 0;
                task->coro.resume_line = 0;
                task->coro.delay_us = 0;
                if (task->cfg.deinit) {
                    task->cfg.deinit(task->cfg.arg);
                }
            }

            if (prev_state == SCH_TASK_STATE_DEAD) {
                task->coro.resume_line = 0;
                task->coro.delay_us = 0;
                if (task->cfg.init) {
                    task->cfg.init(task->cfg.arg);
                }
            }

            sch_insert_runnable_task(sch, task);
            return 0;
        }
    }
    return MERR(MENOTFOUND);
}

static int sch_thread_init(void *arg, const int cpu_id)
{
    ARG_UNUSED(arg);
    ARG_UNUSED(cpu_id);

#if OS(POSIX)
    pthread_t sch_tid;
    int ret = pthread_create(&sch_tid, NULL, sch_thread_exec, arg);
    if (ret != 0) {
        print_err(
            false, "[SCHEDULER] create thread failed, error_code: %d", ret);
        return MERR(MESYSERR);
    }
    sch_bind_thread_to_cpu(sch_tid, cpu_id);
#elif OS(WIN)
    if (cpu_id < 0 || (unsigned int)cpu_id >= sizeof(DWORD_PTR) * 8U) {
        return MERR(MEINVAL);
    }
    DWORD thread_id;
    HANDLE sch_tid = CreateThread(NULL,            // 默认安全属性
                                  0,               // 默认堆栈大小
                                  sch_thread_exec, // 线程函数
                                  arg,             // 传递给线程函数的参数
                                  0,               // 默认创建标志
                                  &thread_id       // 用于接收线程ID
    );
    if (sch_tid == NULL) {
        print_err(false,
                  "[SCHEDULER] create thread failed, error_code: %lu",
                  GetLastError());
        return MERR(MESYSERR);
    }
    sch_bind_thread_to_cpu(sch_tid, cpu_id);
    CloseHandle(sch_tid);
#endif
    return MOK;
}

static int sch_cfs_task_cmp(const struct sch_task *a, const struct sch_task *b)
{
    if (a->elapsed.next_exec_ts < b->elapsed.next_exec_ts) {
        return -1;
    }
    if (a->elapsed.next_exec_ts > b->elapsed.next_exec_ts) {
        return 1;
    }
    if (a->cfg.priority < b->cfg.priority) {
        return -1;
    }
    if (a->cfg.priority > b->cfg.priority) {
        return 1;
    }
    if (a->cfg.id < b->cfg.id) {
        return -1;
    }
    if (a->cfg.id > b->cfg.id) {
        return 1;
    }

    return 0;
}

static void sch_time_insert_task(struct rb_root *rb_root, struct sch_task *task)
{
    if (task->in_time_queue) {
        return;
    }

    struct rb_node **new_rb_node = &rb_root->rb_node;
    struct rb_node *rb_parent = NULL;

    while (*new_rb_node) {
        const struct sch_task *curr =
            CONTAINER_OF(*new_rb_node, struct sch_task, rb_node);
        const int cmp = sch_cfs_task_cmp(task, curr);
        rb_parent = *new_rb_node;
        new_rb_node =
            (cmp < 0) ? &(*new_rb_node)->rb_left : &(*new_rb_node)->rb_right;
    }
    rb_link_node(&task->rb_node, rb_parent, new_rb_node);
    rb_insert_color(&task->rb_node, rb_root);
    task->in_time_queue = true;
}

static void sch_time_remove_task(struct rb_root *rb_root, struct sch_task *task)
{
    if (task->in_time_queue) {
        rb_erase(&task->rb_node, rb_root);
        memset(&task->rb_node, 0, sizeof(struct rb_node));
        task->in_time_queue = false;
    }
}

static struct sch_task *sch_cfs_get_task(struct sch *sch)
{
    DECL(sch, lo);

    const struct rb_root *rb_root = &lo->algo_ctx.cfs.rb_root;
    struct rb_node *rb_node = rb_first(rb_root);
    if (!rb_node) {
        return NULL;
    }

    return CONTAINER_OF(rb_node, struct sch_task, rb_node);
}

static struct sch_task *sch_fcfs_get_task(struct sch *sch)
{
    DECL(sch, lo);

    const size_t prev_idx = lo->algo_ctx.fcfs.prev_idx;
    for (size_t i = 0; i < lo->task_cnt; ++i) {
        const size_t idx = (prev_idx + i) % lo->task_cnt;
        struct sch_task *t = &lo->tasks[idx];
        if (t->status.state == SCH_TASK_STATE_RUN &&
            t->elapsed.next_exec_ts <= lo->elapsed.curr_ts) {
            lo->algo_ctx.fcfs.prev_idx = idx + 1;
            return t;
        }
    }
    return NULL;
}

static void sch_cfs_insert_task(struct sch *sch, struct sch_task *task)
{
    sch_time_insert_task(&sch->lo.algo_ctx.cfs.rb_root, task);
}

static void sch_cfs_remove_task(struct sch *sch, struct sch_task *task)
{
    sch_time_remove_task(&sch->lo.algo_ctx.cfs.rb_root, task);
}

static void sch_fixed_ready_insert(struct sch *sch, struct sch_task *task)
{
    DECL(sch, lo);

    const uint32_t priority = task->cfg.priority;
    if (priority >= SCH_PRIORITY_MAX || task->in_ready_queue) {
        return;
    }

    list_add_tail(&task->ready_node,
                  &lo->algo_ctx.fixed_priority.ready_queues[priority]);
    lo->algo_ctx.fixed_priority.ready_bitmap |= (uint32_t)1U
                                                << (31U - priority);
    task->in_ready_queue = true;
}

static void sch_fixed_ready_remove(struct sch *sch, struct sch_task *task)
{
    DECL(sch, lo);

    const uint32_t priority = task->cfg.priority;
    if (priority >= SCH_PRIORITY_MAX || !task->in_ready_queue) {
        return;
    }

    list_del(&task->ready_node);
    list_init(&task->ready_node);
    task->in_ready_queue = false;
    if (is_list_empty(&lo->algo_ctx.fixed_priority.ready_queues[priority])) {
        lo->algo_ctx.fixed_priority.ready_bitmap &=
            ~((uint32_t)1U << (31U - priority));
    }
}

static void sch_fixed_timer_insert(struct sch *sch, struct sch_task *task)
{
    sch_time_insert_task(&sch->lo.algo_ctx.fixed_priority.timer_root, task);
}

static void sch_fixed_timer_remove(struct sch *sch, struct sch_task *task)
{
    sch_time_remove_task(&sch->lo.algo_ctx.fixed_priority.timer_root, task);
}

static void sch_fixed_promote_due_tasks(struct sch *sch)
{
    DECL(sch, lo);

    for (;;) {
        struct rb_node *node =
            rb_first(&lo->algo_ctx.fixed_priority.timer_root);
        if (!node) {
            return;
        }
        struct sch_task *task = CONTAINER_OF(node, struct sch_task, rb_node);
        if (task->elapsed.next_exec_ts > lo->elapsed.curr_ts) {
            return;
        }
        sch_fixed_timer_remove(sch, task);
        sch_fixed_ready_insert(sch, task);
    }
}

static struct sch_task *sch_fixed_get_task(struct sch *sch)
{
    DECL(sch, lo);

    sch_fixed_promote_due_tasks(sch);
    const uint32_t bitmap = lo->algo_ctx.fixed_priority.ready_bitmap;
    if (!bitmap) {
        return NULL;
    }
    const uint32_t priority = clz32(bitmap);
    return LIST_FIRST_ENTRY(&lo->algo_ctx.fixed_priority.ready_queues[priority],
                            struct sch_task,
                            ready_node);
}

static void sch_remove_queued_task(struct sch *sch, struct sch_task *task)
{
    if (sch->cfg.type == SCH_TYPE_CFS) {
        sch_cfs_remove_task(sch, task);
    } else if (sch->cfg.type == SCH_TYPE_FIXED_PRIORITY) {
        sch_fixed_timer_remove(sch, task);
        sch_fixed_ready_remove(sch, task);
    }
}

static void sch_insert_runnable_task(struct sch *sch, struct sch_task *task)
{
    if (task->status.state != SCH_TASK_STATE_RUN) {
        return;
    }
    if (sch->cfg.type == SCH_TYPE_CFS) {
        sch_cfs_insert_task(sch, task);
    } else if (sch->cfg.type == SCH_TYPE_FIXED_PRIORITY) {
        sch_fixed_timer_insert(sch, task);
    }
}

static size_t sch_period_us(const float32_t freq)
{
    if (!is_f32_finite_rt(freq) || freq <= 0.0F) {
        return 0U;
    }
    const float32_t period = HZ2US(freq);
    if (!is_f32_finite_rt(period) || period < 1.0F ||
        period >= (float32_t)SIZE_MAX) {
        return 0U;
    }
    return (size_t)period;
}
