#ifndef SCH_H
#define SCH_H

#include "errdef.h"
#include "list.h"
#include "rbtree.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SCH_TASK_MAX
#define SCH_TASK_MAX (16)
#endif

#define SCH_PRIORITY_MAX (32)

typedef void (*sch_cb_f)(void *arg);

struct sch_task;

enum sch_coro_result {
    SCH_CORO_YIELDED, // 协程挂起并等待再次调度
    SCH_CORO_DONE,    // 协程执行结束
};

typedef enum sch_coro_result (*sch_coro_f)(struct sch_task *task, void *arg);

struct sch_coro_ctx {
    uint32_t resume_line; // 协程恢复位置
    size_t delay_us;      // 协程等待时间, 单位: us
};

/*
 * 无栈协程只能在同一个函数中挂起; 跨挂起点的局部状态应放在 arg 指向的对象中
 * SCH_CORO_YIELD 使用任务周期再次调度, SCH_CORO_SLEEP_US 使用指定延时
 */
#define SCH_CORO_BEGIN(task)            \
    switch ((task)->coro.resume_line) { \
    case 0:
#define SCH_CORO_SUSPEND_AT(task, line)    \
    do {                                   \
        (task)->coro.resume_line = (line); \
        return SCH_CORO_YIELDED;           \
    case (line):;                          \
    } while (0)
#define SCH_CORO_YIELD(task) SCH_CORO_SUSPEND_AT((task), __LINE__)
#define SCH_CORO_SLEEP_US(task, us)            \
    do {                                       \
        (task)->coro.delay_us = (size_t)(us);  \
        SCH_CORO_SUSPEND_AT((task), __LINE__); \
    } while (0)
#define SCH_CORO_END(task)        \
    }                             \
    (task)->coro.resume_line = 0; \
    return SCH_CORO_DONE

typedef union sch_algo_ctx {
    struct {
        size_t prev_idx; // 最近一次任务索引
    } fcfs;

    struct {
        struct rb_root rb_root; // 按执行时间排序的任务树
    } cfs;

    struct {
        struct rb_root timer_root;                       // 等待任务的定时树
        struct list_head ready_queues[SCH_PRIORITY_MAX]; // 各优先级的就绪队列
        uint32_t ready_bitmap; // 存在就绪任务的优先级位图
    } fixed_priority;
} sch_algo_ctx_u;

enum sch_task_state {
    SCH_TASK_STATE_RUN,   // 就绪或运行
    SCH_TASK_STATE_SLEEP, // 阻塞
    SCH_TASK_STATE_STOP,  // 手动挂起
    SCH_TASK_STATE_DEAD,  // 已结束
};

struct sch_task_cfg {
    size_t id;           // 任务 ID
    uint32_t priority;   // 任务优先级, 数值越小优先级越高
    float32_t exec_freq; // 执行频率, 单位: Hz, 周期须能表示为至少 1 us
    size_t exec_cnt_max; // 最多执行次数
    enum sch_task_state init_state; // 初始任务状态
    size_t init_delay_us;           // 初始延时
    uint32_t budget_us;             // 单次执行预算, 0 表示不检查超时
    sch_cb_f
        init; // 初始化函数, 新建任务 (初始状态不为 DEAD) 或从 DEAD 状态变为其他状态时调用一次
    sch_cb_f exec;   // 执行函数, 周期调用
    sch_coro_f coro; // 无栈协程函数; 设置后不能同时设置 exec
    sch_cb_f deinit; // 清理函数, 任务从其他状态变为 DEAD 时调用一次
    void *arg;       // 回调参数
};

struct sch_task_status {
    enum sch_task_state state;      // 当前任务状态
    enum sch_task_state prev_state; // 上一任务状态
};

struct sch_task_elapsed {
    uint64_t next_exec_ts; // 下次执行时间戳, 与 get_ts 一致
    size_t exec_cnt;       // 任务累计执行次数
    uint32_t exec_us;      // 最近一次执行耗时, 单位: us
    uint32_t exec_us_max;  // 最大执行耗时, 单位: us
    uint32_t overrun_cnt;  // 超过执行预算的次数
};

struct sch_task {
    struct sch_task_cfg cfg;         // 配置参数
    struct sch_task_status status;   // 当前与前次任务状态
    struct sch_task_elapsed elapsed; // 执行计数, 耗时及下次调度时间
    struct sch_coro_ctx coro;        // 协程运行上下文
    uint8_t in_time_queue;           // 任务已加入时间队列
    uint8_t in_ready_queue;          // 任务已加入就绪队列
    struct rb_node rb_node;          // 红黑树根节点
    struct list_head ready_node;     // 就绪队列链接节点
};

struct sch;
typedef uint64_t (*sch_get_ts_f)(void);
typedef struct sch_task *(*sch_get_task_f)(struct sch *sch);
typedef void (*sch_insert_task_f)(struct sch *sch, struct sch_task *task);
typedef void (*sch_remove_task_f)(struct sch *sch, struct sch_task *task);

enum sch_type {
    SCH_TYPE_FCFS,           // 先来先服务调度
    SCH_TYPE_CFS,            // 按下次执行时间调度
    SCH_TYPE_FIXED_PRIORITY, // 固定优先级调度
};

struct sch_cfg {
    uint8_t cpu_id;      // 调度线程绑定的处理器编号
    enum sch_type type;  // 处理类型
    sch_get_ts_f get_ts; // 读取时间戳的回调
};

struct sch_elapsed {
    uint64_t curr_ts;               // 当前调度时间戳
    uint64_t active_start_ts;       // 当前任务开始时间戳
    uint32_t exec_us;               // 最近一次调度耗时, 单位: us
    uint32_t exec_us_max;           // 最大调度耗时, 单位: us
    uint32_t prev_exec_us;          // 最近已完成调度耗时, 单位: us
    volatile uint32_t loop_cnt;     // 调度入口心跳, 无就绪任务时仍递增
    volatile uint32_t dispatch_cnt; // 实际调用任务执行函数的次数
};

struct sch_check {
    int prev_err; // 最近操作错误码
};

struct sch_lo {
    volatile uint8_t task_active; // 当前正在执行任务回调
    size_t active_task_id;        // task_active 为 0 时保留最后一次任务 ID
    struct sch_check check;       // 调度错误诊断
    size_t task_cnt;              // 已注册的任务数量
    struct sch_elapsed elapsed;   // 调度时间, 耗时与心跳计数
    sch_get_task_f get_task;      // 选择下一执行任务的回调
    sch_algo_ctx_u algo_ctx;      // 调度算法工作区
    struct sch_task tasks[SCH_TASK_MAX]; // 任务存储数组
};

struct sch {
    struct sch_cfg cfg; // 配置参数
    struct sch_lo lo;   // 运行状态
};

/**
 * @brief                 初始化调度器
 * @param[in,out] sch     调度器对象
 * @param[in]     sch_cfg 调度器配置
 * @return                操作结果, 失败时返回对应错误码
 */
int sch_init(struct sch *sch, struct sch_cfg sch_cfg);

/**
 * @brief             运行调度循环
 * @param[in,out] sch 调度器对象
 * @return            操作结果, 失败时返回对应错误码
 */
int sch_run(struct sch *sch);

/**
 * @brief             执行一次任务调度
 * @param[in,out] sch 调度器对象
 * @return            操作结果, 失败时返回对应错误码
 */
int sch_exec(struct sch *sch);

/**
 * @brief         读取最近一次调度错误
 * @param[in] sch 调度器对象
 * @return        最近一次调度错误码
 */
int sch_prev_err(const struct sch *sch);

/**
 * @brief                  注册任务及其配置
 * @param[in,out] sch      调度器对象
 * @param[in]     task_cfg 任务配置
 * @return                 操作结果, 失败时返回对应错误码
 */
int sch_add_task(struct sch *sch, struct sch_task_cfg task_cfg);

/**
 * @brief             重新初始化已注册任务
 * @param[in,out] sch 调度器对象
 * @return            操作结果, 失败时返回对应错误码
 */
int sch_reinit_tasks(struct sch *sch); // 显式重新调用所有已注册任务的 init

/**
 * @brief                   设置任务执行频率
 * @param[in,out] sch       调度器对象
 * @param[in]     task_id   任务标识
 * @param[in]     exec_freq 任务执行频率, 单位: Hz
 * @return                  成功返回 0, 无效频率返回 -MEINVAL, 其他失败返回负错误码
 * @note                    支持小数频率, 周期须能表示为至少 1 us; 失败时不修改任务
 */
int sch_set_task_freq(struct sch *sch, size_t task_id, float32_t exec_freq);

/**
 * @brief               设置任务状态
 * @param[in,out] sch   调度器对象
 * @param[in]     id    任务标识
 * @param[in]     state 目标任务状态
 * @return              操作结果, 失败时返回对应错误码
 * @note                进入目标状态后调用生命周期回调, 保留回调对任务状态的修改
 */
int sch_set_task_state(struct sch *sch,
                       const size_t id,
                       const enum sch_task_state state);

#ifdef __cplusplus
}
#endif

#endif
