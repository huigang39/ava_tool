/* FSANeo / ava_eth SDK 表格接口，Windows x64，C ABI v2。
 * 由 ava_eth 管理周期 EtherCAT 通信，不再使用 SOEM 共享内存。
 * 位置 rad，速度 rad/s，电流 A，转矩 Nm；axis 从 0 开始。
 */
#ifndef NEOFSA_C_H
#define NEOFSA_C_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* 0 成功，负值为错误；-100-result 对应 AvaEthResult。
 * 错误详情通过 neofsa_last_error 获取，SDK 原文附在中文说明后。
 */
typedef enum neofsa_result {
        NEOFSA_OK            = 0,
        NEOFSA_ERR_ARG       = -1,
        NEOFSA_ERR_NO_MASTER = -2,
        NEOFSA_ERR_RUNTIME   = -3,
        NEOFSA_ERR_ABI       = -4
} neofsa_result;
typedef enum neofsa_backend {
        NEOFSA_BACKEND_NPCAP           = 4,
        NEOFSA_BACKEND_SIM             = 1,
        NEOFSA_BACKEND_DEVICE_SIM      = 2,
        NEOFSA_BACKEND_DEVICE_HARDWARE = 3
} neofsa_backend;
typedef enum neofsa_master_action {
        NEOFSA_MASTER_NONE  = 0,
        NEOFSA_MASTER_START = 1,
        NEOFSA_MASTER_STOP  = 2
} neofsa_master_action;
typedef enum neofsa_mode_of_op {
        NEOFSA_MODE_NONE    = 0,    // 无模式
        NEOFSA_MODE_HM      = 6,    // 原点回归模式
        NEOFSA_MODE_CSP     = 8,    // 周期同步位置模式 (基于转矩)
        NEOFSA_MODE_CSPC    = -8,   // 周期同步位置模式 (基于电流)
        NEOFSA_MODE_CSV     = 9,    // 周期同步速度模式 (基于转矩)
        NEOFSA_MODE_CSVC    = -9,   // 周期同步速度模式 (基于电流)
        NEOFSA_MODE_CST     = 10,   // 周期同步转矩模式
        NEOFSA_MODE_CSC     = -10,  // 周期同步电流模式
        NEOFSA_MODE_IF      = -126, // I/F 模式
        NEOFSA_MODE_VF      = -125, // V/F 模式
        NEOFSA_MODE_PD      = -128, // 力位混合模式
        NEOFSA_MODE_RATCHET = -127  // 棘轮模式
} neofsa_mode_of_op;

// 操作一次后清零；设置目标本身不会使能; 读反馈不续约看门狗
typedef enum neofsa_action {
        NEOFSA_ACTION_NONE        = 0,
        NEOFSA_ACTION_ENABLE      = 1,
        NEOFSA_ACTION_DISABLE     = 2,
        NEOFSA_ACTION_RESET_FAULT = 3,
        NEOFSA_ACTION_KEEPALIVE   = 4
} neofsa_action;

typedef struct neofsa_param {
        // UTF-8 DLL 路径; 空值自动寻找随插件分发或仓库内的运行库
        char runtime_dll[1024];
        // Npcap 网卡路径或内核设备路径，模拟器留空
        char                 adapter[1024];
        neofsa_backend       backend;     // 0 -> Npcap，不会自动连接
        uint32_t             axes;        // 0 -> 1；当前 Npcap 仅支持 1 轴
        uint32_t             cycle_us;    // 0 -> 1000
        uint32_t             watchdog_ms; // 0 -> 100；调用方必须持续写目标或 KEEPALIVE
        neofsa_master_action master_action;
        uint32_t             master_running; // 句柄已打开，不代表反馈有效或已使能
        uint32_t             capabilities;
        uint32_t             online;
} neofsa_param;

typedef struct neofsa_adapter_param {
        char           runtime_dll[1024];
        neofsa_backend backend; // 0 -> Npcap；模拟器无需枚举
        uint32_t       index;
        uint32_t       count;
        char           name[1024];
} neofsa_adapter_param;

struct neofsa_target {
        neofsa_mode_of_op mode;
        float             position;
        float             velocity;
        float             current;
        float             torque;
};
struct neofsa_feedback {
        uint32_t flags; // 1在线 2反馈有效 4使能 8故障 16命令过期 32温度有效 64模拟 128电磁转矩有效
        uint32_t error_code;
        int32_t  mode;
        uint32_t special_cmd;
        float    position;
        float    velocity;
        float    current;
        float    torque;                 // 负载转矩
        float    electromagnetic_torque; // 必须 flags & 128
        float    vbus;
        float    motor_temperature;
        float    drive_temperature;
        uint64_t timestamp_ns;
        uint64_t accepted_sequence;
        uint64_t applied_sequence;
};
typedef struct neofsa_rt {
        uint32_t               axis;         // 0-based
        uint32_t               write_target; // 编辑 target.* 自动置位；成功后清零
        neofsa_action          action;       // 执行后清零，失败也不自动重试
        struct neofsa_target   target;
        struct neofsa_feedback feedback;
} neofsa_rt;

/* PD 增益通过 ava_eth 在线 PDO 参数接口异步发送，仅允许失能时排队。
 * state: 0空闲 1发送中 2已发送 3失败；sent_* 是发送精度，不是设备回读。
 * 当前 Npcap/axis=0 支持；本地模拟器不支持增益写入。
 */
typedef struct neofsa_pd_param {
        uint32_t axis;
        float    kp;
        float    kd;
        uint32_t state;
        uint32_t result;
        uint64_t sequence;
        float    sent_kp;
        float    sent_kd;
        char     message[512];
} neofsa_pd_param;
int neofsa_write_pd_param(neofsa_pd_param *param);
int neofsa_read_pd_param(neofsa_pd_param *param);

int neofsa_read_param(neofsa_param *param);
int neofsa_write_param(neofsa_param *param);
int neofsa_read_adapter_param(neofsa_adapter_param *param);
int neofsa_read_rt(neofsa_rt *rt);
int neofsa_write_rt(neofsa_rt *rt);
/**
 * @brief UTF-8，当前调用线程；下次本线程 API 调用可能覆盖内容
 */
const char *neofsa_last_error(void);
/**
 * @brief 显式释放会话，调用卸载 DLL 前必须执行；不在 DllMain 中等待线程
 */
int neofsa_shutdown(void);
#ifdef __cplusplus
}
#endif
#endif
