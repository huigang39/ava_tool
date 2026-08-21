#ifndef FSAV3_H
#define FSAV3_H

#define FSAV3_SDK_VERSION_4U8_TO_U32( a, b, c, d )                                                                                    \
    ( uint32_t )( ( ( ( ( uint32_t )( a )&0xFF ) << 24 ) | ( ( ( uint32_t )( b )&0xFF ) << 16 ) | ( ( ( uint32_t )( c )&0xFF ) << 8 ) \
                    | ( ( uint32_t )( d )&0xFF ) ) )
#define FSAV3_SDK_VERSION_U32 FSAV3_SDK_VERSION_4U8_TO_U32( 3, 1, 0, 16 )  // SDK 版本号

#ifdef __cplusplus

#include <array>
#include <cstdint>
#include <string>
#include <vector>

/* 前置声明 */
struct fsa_interface_dev_t;

namespace AC3 {
/* --------------------------------- SDK 配置 --------------------------------- */
struct sdk_config_t {
    const uint32_t SDK_HEADER_VERSION;  // SDK 头文件版本

    const uint32_t MIN_M4_VER;  // 最低 M4核 固件版本
    const uint32_t MIN_M7_VER;  // 最低 M7核 固件版本

    int DEFAULT_TIMEOUT_MS;  // 用户默认超时时间, 单位毫秒
    int DEFAULT_MAX_RETRY;   // 用户默认最大重试次数
};
static constexpr sdk_config_t SDK_CONFIG = { FSAV3_SDK_VERSION_U32, 212, 209, 5, 1 };
/* --------------------------------- SDK 配置 --------------------------------- */

class FSA {
    /* -------------------------------------------------------------------------- */
    /*                                    类型定义                                */
    /* -------------------------------------------------------------------------- */
public:
    enum class ret_e : int {
        SUCCESS = 0,  // 操作成功

        /**
         * @brief SDK 版本不匹配
         * @note 执行器固件版本过低或 SDK 动态库与头文件版本不匹配
         */
        SDK_VER_NOT_MATCH = -101,

        /**
         * @brief 执行器获取配置参数错误
         * @note 型号和版本号未获取成功
         */
        CTRL_PARAM_ERR = -102,

        /**
         * @brief 执行器对象未初始化
         * @note 可能未执行 init 函数
         */
        INTERFACE_HANDLE_ERR = -201,

        /**
         * @brief 接口参数非法
         * @note 传入接口的参数不符合要求
         */
        ARG_ERR = -202,

        /**
         * @brief 网络错误
         * @note 可能对应 IP 的 FSA 网络已断开
         */
        NET_ERR = -301,

        /**
         * @brief 网络接收超时
         * @note 接收执行器返回数据等待时间超过指定超时时间
         */
        TIMEOUT = -302,

        /**
         * @brief 网络接收缓冲区数据与发送指令不匹配
         * @note 可能接收到系统 socket 输入缓冲区前几帧数据导致指令不匹配
         */
        NET_RXBUF_DATA_ERR = -303,
    };

    enum class net_recv_mode_e : int {
        NONE = 0,

        /**
         * @brief 等待时让出 CPU
         * @warning 实际超时返回时间可能会大于指定超时时间(取决于系统调度和负载)
         */
        YIELD_WAIT = 1,

        /**
         * @brief 等待时 while 自旋, 不让出 CPU
         * @warning 可能会造成 CPU 满载
         */
        SPIN_WAIT = 2,
    };

    enum class ctrl_mode_e : int {
        NONE          = 0,  // 无控制模式
        CURRENT_MODE  = 1,  // 电流控制模式，建议使用转矩控制模式
        TORQUE_MODE   = 4,  // 转矩控制模式
        VELOCITY_MODE = 5,  // 转矩速度控制模式
        POSITION_MODE = 6,  // 转矩位置控制模式
        PD_MODE       = 7,  // 转矩PD控制模式
    };

    struct pvctte_t {
        double pos;
        double vel;
        double cur;
        double tor;
        double tor_e;
    };

    struct temp_vbus_t {
        double mos;
        double coil;
        double vbus;
    };

    struct pid_param_t {
        double pos_kp;
        double vel_kp;
        double vel_ki;
    };

    struct pd_param_t {
        double pd_kp;
        double pd_kd;
    };

    struct err_code_t {
        std::array< uint32_t, 8 > arr;
    };

    struct parsed_err_code_item_t {
        int8_t      err_level;      // 3:错误，2:警告且限制部分功能，1:警告，-1:信息，-2:信息
        uint8_t     err_u32_index;  // 从0到7
        uint8_t     err_bit_index;  // 从0到31
        uint16_t    parsed_code;    // 将定长bit位错误码转换为具体错误码，方便用户查阅
        const char* str_for_log;    // 方便打印日志的字符串
    };

    struct parsed_err_code_t {
        std::vector< parsed_err_code_item_t > vec;
    };

    struct comm_timeout_protect_config_t {
        bool     config_timeout_ms     = true;   // 是否修改timeout_ms
        uint32_t set_timeout_ms        = 0;      // 超时时间，单位毫秒，0代表关闭此功能
        bool     config_protect_pos_kp = false;  // 是否修改进入保护状态时的pos_kp参数
        bool     config_protect_vel_kp = false;  // 是否修改进入保护状态时的vel_kp参数
        bool     config_protect_vel_ki = false;  // 是否修改进入保护状态时的vel_ki参数
        pid_param_t set_protect_pid;  // 如果config_protect_xxx为true，则需要用户配置对应参数，false则对应参数不会下发给执行器
        uint32_t    get_timeout_ms;   // 获取超时时间
        pid_param_t get_protect_pid;  // 获取保护状态pid参数
    };

    /* 订阅模式 */
    struct subs_config_t {
        uint32_t enable            = 1;      // 订阅使能，0:禁用，1:启用
        uint16_t freq              = 2000;   // 订阅回传频率，单位Hz
        uint32_t keepalive_time_ms = 10000;  // 保持时间时间，单位ms
        uint32_t pos               = 1;      // 位置订阅使能，0:禁用，1:启用
        uint32_t vel               = 1;      // 速度订阅使能，0:禁用，1:启用
        uint32_t cur               = 1;      // 电流订阅使能，0:禁用，1:启用
        uint32_t tor               = 1;      // 转矩订阅使能，0:禁用，1:启用
        uint32_t tor_em            = 1;      // 电磁转矩订阅使能，0:禁用，1:启用
        uint32_t error             = 1;      // 错误码订阅使能，0:禁用，1:启用
        uint32_t error_ext2        = 1;      // 错误码扩展2订阅使能，0:禁用，1:启用
        uint32_t error_ext3        = 1;      // 错误码扩展3订阅使能，0:禁用，1:启用
        uint32_t error_ext4        = 1;      // 错误码扩展4订阅使能，0:禁用，1:启用
        uint32_t error_ext5        = 1;      // 错误码扩展5订阅使能，0:禁用，1:启用
        uint32_t error_ext6        = 1;      // 错误码扩展6订阅使能，0:禁用，1:启用
        uint32_t error_ext7        = 1;      // 错误码扩展7订阅使能，0:禁用，1:启用
        uint32_t error_ext8        = 1;      // 错误码扩展8订阅使能，0:禁用，1:启用
        uint32_t temp_mos          = 1;      // MOS温度订阅使能，0:禁用，1:启用
        uint32_t temp_coil         = 1;      // 线圈温度订阅使能，0:禁用，1:启用
        uint32_t vbus              = 1;      // VBUS电压订阅使能，0:禁用，1:启用
    };

    struct subs_data_t {
        uint64_t                  cnt = 0;
        pvctte_t                  pvct;
        temp_vbus_t               temp_vbus;
        std::array< uint32_t, 8 > error;
        int64_t                   latency_from_recv_ns;
    };

    struct type_subversion_t {
        std::array< char, 33 > type;         // 执行器型号字符串, 前32字节有效, 最后1字节固定为'\0'
        std::array< char, 33 > sub_version;  // 执行器子版本字符串, 前32字节有效, 最后1字节固定为'\0'
    };

    struct comm_req_get_t {
        bool name           = false;  // 是否获取名称
        bool type           = false;  // 是否获取类型
        bool mcu_fw_version = false;  // 是否获取 MCU 固件版本
        bool mac_address    = false;  // 是否获取 MAC 地址
        bool uid            = false;  // 是否获取 UID
        bool sn             = false;  // 是否获取 SN
        bool PCBA_sn        = false;  // 是否获取 PCBA SN
        bool gearbox_sn     = false;  // 是否获取齿轮箱 SN
        bool static_IP      = false;  // 是否获取静态 IP
        bool gateway        = false;  // 是否获取网关
        bool subnet_mask    = false;  // 是否获取子网掩码
        bool dns_1          = false;  // 是否获取 DNS 1
        bool dns_2          = false;  // 是否获取 DNS 2
        bool DHCP_enable    = false;  // 是否获取 DHCP 启用状态
    };

    struct comm_resp_get_t {
        bool        name_valid = false;
        std::string name;  // 名称
        bool        type_valid = false;
        std::string type;  // 类型
        bool        mcu_fw_version_valid = false;
        std::string mcu_fw_version;  // MCU 固件版本
        bool        mac_address_valid = false;
        std::string mac_address;  // MAC 地址
        bool        uid_valid = false;
        std::string uid;  // UID
        bool        sn_valid = false;
        std::string sn;  // SN
        bool        PCBA_sn_valid = false;
        std::string PCBA_sn;  // PCBA SN
        bool        gearbox_sn_valid = false;
        std::string gearbox_sn;  // 减速机 SN
        bool        static_IP_valid = false;
        std::string static_IP;  // 静态 IP
        bool        gateway_valid = false;
        std::string gateway;  // 网关
        bool        subnet_mask_valid = false;
        std::string subnet_mask;  // 子网掩码
        bool        dns_1_valid = false;
        std::string dns_1;  // DNS 1
        bool        dns_2_valid = false;
        std::string dns_2;  // DNS 2
        bool        DHCP_enable_valid = false;
        bool        DHCP_enable;  // DHCP 启用状态
    };
    /* ---------------------------------- 实时控制 ---------------------------------- */

    /*------------------------ Power ------------------------*/
    /**
     * @brief 重启执行器
     * @param timeout_ms 超时时间
     * @param max_retry 最大重试次数
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e Reboot( double timeout_ms = SDK_CONFIG.DEFAULT_TIMEOUT_MS, int max_retry = SDK_CONFIG.DEFAULT_MAX_RETRY );

    /**
     * @brief 开启机器人电源板继电器
     * @param timeout_ms 超时时间
     * @param max_retry 最大重试次数
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e OpenRelay( double timeout_ms = SDK_CONFIG.DEFAULT_TIMEOUT_MS, int max_retry = SDK_CONFIG.DEFAULT_MAX_RETRY );

    /**
     * @brief 关闭机器人电源板继电器
     * @param timeout_ms 超时时间
     * @param max_retry 最大重试次数
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e CloseRelay( double timeout_ms = SDK_CONFIG.DEFAULT_TIMEOUT_MS, int max_retry = SDK_CONFIG.DEFAULT_MAX_RETRY );
    /*------------------------ Power ------------------------*/

    /*----------------------- Control -----------------------*/
    /**
     * @brief 执行器控制使能
     * @param control_mode 控制模式
     * @param timeout_ms 超时时间
     * @param max_retry 最大重试次数
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e EnableControl( ctrl_mode_e ctrl_mode, double timeout_ms = SDK_CONFIG.DEFAULT_TIMEOUT_MS, int max_retry = SDK_CONFIG.DEFAULT_MAX_RETRY );

    /**
     * @brief 失能执行器
     * @param timeout_ms 超时时间
     * @param max_retry 最大重试次数
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e DisableControl( double timeout_ms = SDK_CONFIG.DEFAULT_TIMEOUT_MS, int max_retry = SDK_CONFIG.DEFAULT_MAX_RETRY );

    /**
     * @brief 发送位置目标指令、速度前馈指令、力矩前馈指令，执行器不回复任何数据帧
     * @param p_rad 目标位置
     * @param v_ff_radps 速度前馈
     * @param t_ff_Nm 力矩前馈
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e SetPositionNoAck( double p_rad, double v_ff_radps = 0, double t_ff_Nm = 0 );

    /**
     * @brief 发送位置目标指令、速度前馈指令、力矩前馈指令，执行器回复数据帧
     * @param p_rad 目标位置
     * @param v_ff_radps 速度前馈
     * @param t_ff_Nm 力矩前馈
     * @param timeout_ms 超时时间
     * @param max_retry 最大重试次数
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e SetPosition( double p_rad,
                       double v_ff_radps = 0,
                       double t_ff_Nm    = 0,
                       double timeout_ms = SDK_CONFIG.DEFAULT_TIMEOUT_MS,
                       int    max_retry  = SDK_CONFIG.DEFAULT_MAX_RETRY );

    /**
     * @brief 发送位置目标指令、速度目标指令、力矩前馈指令，执行器不回复任何数据帧
     * @param p_rad 目标位置
     * @param v_radps 目标速度
     * @param t_ff_Nm 力矩前馈
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e SetPDPositionVelocityNoAck( double p_rad, double v_radps = 0, double t_ff_Nm = 0 );

    /**
     * @brief 发送位置目标指令、速度目标指令、力矩前馈指令，执行器回复数据帧
     * @param p_rad 目标位置
     * @param v_radps 目标速度
     * @param t_ff_Nm 力矩前馈
     * @param timeout_ms 超时时间
     * @param max_retry 最大重试次数
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e SetPDPositionVelocity( double p_rad,
                                 double v_radps    = 0,
                                 double t_ff_Nm    = 0,
                                 double timeout_ms = SDK_CONFIG.DEFAULT_TIMEOUT_MS,
                                 int    max_retry  = SDK_CONFIG.DEFAULT_MAX_RETRY );

    /**
     * @brief 发送速度目标指令、力矩前馈指令，执行器不回复任何数据帧
     * @param v_radps 目标速度
     * @param t_ff_Nm 力矩前馈
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e SetVelocityNoAck( double v_radps, double t_ff_Nm = 0 );

    /**
     * @brief 发送速度目标指令、力矩前馈指令，执行器回复数据帧
     * @param v_radps 目标速度
     * @param t_ff_Nm 力矩前馈
     * @param timeout_ms 超时时间
     * @param max_retry 最大重试次数
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e SetVelocity( double v_radps,
                       double t_ff_Nm    = 0,
                       double timeout_ms = SDK_CONFIG.DEFAULT_TIMEOUT_MS,
                       int    max_retry  = SDK_CONFIG.DEFAULT_MAX_RETRY );

    /**
     * @brief 发送力矩目标指令，执行器不回复任何数据帧
     * @param t_Nm 目标力矩
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e SetTorqueNoAck( double t_Nm );

    /**
     * @brief 发送力矩目标指令，执行器回复数据帧
     * @param t_Nm 目标力矩
     * @param timeout_ms 超时时间
     * @param max_retry 最大重试次数
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e SetTorque( double t_Nm, double timeout_ms = SDK_CONFIG.DEFAULT_TIMEOUT_MS, int max_retry = SDK_CONFIG.DEFAULT_MAX_RETRY );

    /**
     * @brief 发送电流目标指令，执行器不回复任何数据帧
     * @param c_A 目标电流
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e SetCurrentNoAck( double c_A );

    /**
     * @brief 发送电流目标指令，执行器回复数据帧
     * @param c_A 目标电流
     * @param timeout_ms 超时时间
     * @param max_retry 最大重试次数
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e SetCurrent( double c_A, double timeout_ms = SDK_CONFIG.DEFAULT_TIMEOUT_MS, int max_retry = SDK_CONFIG.DEFAULT_MAX_RETRY );
    /*----------------------- Control -----------------------*/

    /*----------------------- State -----------------------*/
    /**
     * @brief 获取执行器位置、速度、电流、力矩，电磁转矩
     * @param rx_PVCTTe 存放获取的位置、速度、电流、力矩、电磁转矩
     * @param timeout_ms 超时时间
     * @param max_retry 最大重试次数
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e GetPVCTTe( pvctte_t& rx_PVCTTe, double timeout_ms = SDK_CONFIG.DEFAULT_TIMEOUT_MS, int max_retry = SDK_CONFIG.DEFAULT_MAX_RETRY );

    /**
     * @brief 配置订阅模式
     * @param subs_config 订阅模式配置
     * @param timeout_ms 超时时间
     * @param max_retry 最大重试次数
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e
    EnableSubscribe( subs_config_t& subs_config, double timeout_ms = SDK_CONFIG.DEFAULT_TIMEOUT_MS, int max_retry = SDK_CONFIG.DEFAULT_MAX_RETRY );

    /**
     * @brief 获取最新的订阅数据
     * @param subs_data 存放获取的订阅数据
     * @return 状态码
     */
    ret_e GetSubsData( subs_data_t& subs_data );

    /**
     * @brief 获取执行器mos温度，绕组温度，电压
     * @param rx_temp_vbus 存放获取的mos温度，绕组温度，电压
     * @param timeout_ms 超时时间
     * @param max_retry 最大重试次数
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e GetTempVbus( temp_vbus_t& rx_temp_vbus, double timeout_ms = SDK_CONFIG.DEFAULT_TIMEOUT_MS, int max_retry = SDK_CONFIG.DEFAULT_MAX_RETRY );

    /**
     * @brief 获取执行器当前错误码
     * @param rx_errcode 存放获取的错误码
     * @param timeout_ms 超时时间
     * @param max_retry 最大重试次数
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e GetErrCode( err_code_t& rx_errcode, double timeout_ms = SDK_CONFIG.DEFAULT_TIMEOUT_MS, int max_retry = SDK_CONFIG.DEFAULT_MAX_RETRY );

    /**
     * @brief 解析执行器错误码
     * @param err_code 获取到的8个uint32_t错误码
     * @param parsed_err_code 解析后的错误码
     * @return 返回详细执行状态ret_e(int)
     */
    static ret_e ParseErrCode( const err_code_t err_code, parsed_err_code_t& parsed_err_code );

    /*----------------------- State -----------------------*/

    /*---------------------- Config ----------------------*/
    /**
     * @brief 实时设置位置环速度环PID参数
     * @param p_kp 位置环比例增益
     * @param v_kp 速度环比例增益
     * @param v_ki 速度环积分增益
     * @param timeout_ms 超时时间
     * @param max_retry 最大重试次数
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e SetPIDParams(
        double p_kp, double v_kp, double v_ki, double timeout_ms = SDK_CONFIG.DEFAULT_TIMEOUT_MS, int max_retry = SDK_CONFIG.DEFAULT_MAX_RETRY );

    /**
     * @brief 实时获取位置环速度环PID参数
     * @param rx_pid_param 存放获取的 PID 参数
     * @param timeout_ms 超时时间
     * @param max_retry 最大重试次数
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e GetPIDParams( pid_param_t& rx_pid, double timeout_ms = SDK_CONFIG.DEFAULT_TIMEOUT_MS, int max_retry = SDK_CONFIG.DEFAULT_MAX_RETRY );

    /**
     * @brief 实时设置 PD 参数
     * @param pd_kp PD比例增益
     * @param pd_kd PD微分增益
     * @param timeout_ms 超时时间
     * @param max_retry 最大重试次数
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e SetPDParams( double pd_kp, double pd_kd, double timeout_ms = SDK_CONFIG.DEFAULT_TIMEOUT_MS, int max_retry = SDK_CONFIG.DEFAULT_MAX_RETRY );

    /**
     * @brief 获取临时立即生效 (imm) PD 参数
     * @param rx_pd_param 存放获取的 PD 参数
     * @param timeout_ms 超时时间
     * @param max_retry 最大重试次数
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e GetPDParams( pd_param_t& rx_pd, double timeout_ms = SDK_CONFIG.DEFAULT_TIMEOUT_MS, int max_retry = SDK_CONFIG.DEFAULT_MAX_RETRY );

    /**
     * @brief 配置通信超时保护功能
     * @param comm_timeout_protect_config 通信超时保护功能配置
     * @param timeout_ms 超时时间
     * @param max_retry 最大重试次数
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e CommTimeoutProtect( comm_timeout_protect_config_t& comm_timeout_protect_config,
                              double                         timeout_ms = SDK_CONFIG.DEFAULT_TIMEOUT_MS,
                              int                            max_retry  = SDK_CONFIG.DEFAULT_MAX_RETRY );

    /**
     * @brief 内置绝编置零
     * @param timeout_ms 超时时间
     * @param max_retry 最大重试次数
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e SetAbsEncoderZero( double timeout_ms = SDK_CONFIG.DEFAULT_TIMEOUT_MS, int max_retry = SDK_CONFIG.DEFAULT_MAX_RETRY );

    /**
     * @brief 获取执行器型号和子版本号
     * @param type_subversion 存放获取的型号和子版本号
     * @param timeout_ms 超时时间
     * @param max_retry 最大重试次数
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e GetTypeSubversion( type_subversion_t& type_subversion,
                             double             timeout_ms = SDK_CONFIG.DEFAULT_TIMEOUT_MS,
                             int                max_retry  = SDK_CONFIG.DEFAULT_MAX_RETRY );

    /**
     * @brief 获取执行器通信配置参数
     * @param comm_req_get 请求哪些通信配置参数
     * @param comm_resp_get 存放获取的通信配置参数
     * @param timeout_ms 超时时间
     * @param max_retry 最大重试次数
     * @return 返回详细执行状态ret_e(int)
     */
    ret_e GetCommConfig( comm_req_get_t&  comm_req_get,
                         comm_resp_get_t& comm_resp_get,
                         double           timeout_ms = SDK_CONFIG.DEFAULT_TIMEOUT_MS,
                         int              max_retry  = SDK_CONFIG.DEFAULT_MAX_RETRY );

    /*---------------------- Config ----------------------*/

    /**
     * @brief 构造函数
     * @note 在此定义构造，方便将用户头文件版本号传入进行版本匹配
     */
    FSA() {
        sdk_header_version = SDK_CONFIG.SDK_HEADER_VERSION;
        m4_min_version     = SDK_CONFIG.MIN_M4_VER;
        m7_min_version     = SDK_CONFIG.MIN_M7_VER;
    }
    ~FSA();
    ret_e Init( const char* ip, net_recv_mode_e net_recv_mode = net_recv_mode_e::YIELD_WAIT, const char* rtcko_path = "" );

private:
    fsa_interface_dev_t* fsa_dev = nullptr;
    uint16_t             cnt     = 0;
    uint32_t             sdk_header_version;
    uint32_t             m4_min_version;  // 最低 M4核 固件版本
    uint32_t             m7_min_version;  // 最低 M7核 固件版本
};

}  // namespace AC3

#endif  //__cplusplus

#endif  // FSAV3_H
