#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "fsa_interface.h"
#include "fsav3.h"

#define CHECK_FSA_DEV( fsa_dev )                \
    do {                                        \
        if ( fsa_dev == nullptr )               \
            return ret_e::INTERFACE_HANDLE_ERR; \
    } while ( 0 );

#define CHECK_ARG_INVALID( timeout_ms, max_retry )                                                           \
    do {                                                                                                     \
        if ( ( timeout_ms < 0 ) || ( timeout_ms > 1000 * 60 ) || ( max_retry < 0 ) || ( max_retry > 1000 ) ) \
            return ret_e::ARG_ERR;                                                                           \
    } while ( 0 );

#define CHECK_INVALID_FP( value )       \
    do {                                \
        if ( std::isnan( value ) ) {    \
            return FSA::ret_e::ARG_ERR; \
        }                               \
        if ( std::isinf( value ) ) {    \
            return FSA::ret_e::ARG_ERR; \
        }                               \
    } while ( 0 );

extern fsa_rtcko_dev_t fsa_rtcko_dev;

namespace AC3 {

FSA::~FSA() {
    if ( fsa_dev != nullptr ) {
        fsa_interface_destroy( fsa_dev );
    }
}

FSA::ret_e FSA::Init( const char* ip, net_recv_mode_e net_recv_mode, const char* rtcko_path ) {
    int interface_ret =
        fsa_interface_init( &fsa_dev, ip, ( fsa_net_recv_mode_e )net_recv_mode, rtcko_path, sdk_header_version, m4_min_version, m7_min_version );
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::Reboot( double timeout_ms, int max_retry ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_ARG_INVALID( timeout_ms, max_retry )
    CHECK_INVALID_FP( timeout_ms )
    int interface_ret = fsa_interface_reboot( fsa_dev, timeout_ms, max_retry );
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::OpenRelay( double timeout_ms, int max_retry ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_ARG_INVALID( timeout_ms, max_retry )
    CHECK_INVALID_FP( timeout_ms )
    int interface_ret = fsa_interface_power_open_relay( fsa_dev, timeout_ms, max_retry );
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::CloseRelay( double timeout_ms, int max_retry ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_ARG_INVALID( timeout_ms, max_retry )
    CHECK_INVALID_FP( timeout_ms )
    int interface_ret = fsa_interface_power_close_relay( fsa_dev, timeout_ms, max_retry );
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::EnableControl( ctrl_mode_e control_mode, double timeout_ms, int max_retry ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_ARG_INVALID( timeout_ms, max_retry )
    CHECK_INVALID_FP( timeout_ms )
    int interface_ret = fsa_interface_enable_control( fsa_dev, ( fsa_ctrl_mode_e )control_mode, timeout_ms, max_retry, cnt++ );
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::DisableControl( double timeout_ms, int max_retry ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_ARG_INVALID( timeout_ms, max_retry )
    CHECK_INVALID_FP( timeout_ms )
    int interface_ret = fsa_interface_disable_control( fsa_dev, timeout_ms, max_retry, cnt++ );
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::SetPositionNoAck( double p_rad, double v_ff_radps, double t_ff_Nm ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_INVALID_FP( p_rad )
    CHECK_INVALID_FP( v_ff_radps )
    CHECK_INVALID_FP( t_ff_Nm )
    int interface_ret = fsa_interface_set_p_vff_tff_noack( fsa_dev, p_rad, v_ff_radps, t_ff_Nm, cnt++ );
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::SetPosition( double p_rad, double v_ff_radps, double t_ff_Nm, double timeout_ms, int max_retry ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_ARG_INVALID( timeout_ms, max_retry )
    CHECK_INVALID_FP( p_rad )
    CHECK_INVALID_FP( v_ff_radps )
    CHECK_INVALID_FP( t_ff_Nm )
    CHECK_INVALID_FP( timeout_ms )
    int interface_ret = fsa_interface_set_p_vff_tff( fsa_dev, p_rad, v_ff_radps, t_ff_Nm, timeout_ms, max_retry, cnt++ );
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::SetPDPositionVelocityNoAck( double p_rad, double v_radps, double t_ff_Nm ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_INVALID_FP( p_rad )
    CHECK_INVALID_FP( v_radps )
    CHECK_INVALID_FP( t_ff_Nm )
    int interface_ret = fsa_interface_set_p_v_tff_noack( fsa_dev, p_rad, v_radps, t_ff_Nm, cnt++ );
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::SetPDPositionVelocity( double p_rad, double v_radps, double t_ff_Nm, double timeout_ms, int max_retry ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_ARG_INVALID( timeout_ms, max_retry )
    CHECK_INVALID_FP( p_rad )
    CHECK_INVALID_FP( v_radps )
    CHECK_INVALID_FP( t_ff_Nm )
    CHECK_INVALID_FP( timeout_ms )
    int interface_ret = fsa_interface_set_p_v_tff( fsa_dev, p_rad, v_radps, t_ff_Nm, timeout_ms, max_retry, cnt++ );
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::SetVelocityNoAck( double v_radps, double t_ff_Nm ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_INVALID_FP( v_radps )
    CHECK_INVALID_FP( t_ff_Nm )
    int interface_ret = fsa_interface_set_v_tff_noack( fsa_dev, v_radps, t_ff_Nm, cnt++ );
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::SetVelocity( double v_radps, double t_ff_Nm, double timeout_ms, int max_retry ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_ARG_INVALID( timeout_ms, max_retry )
    CHECK_INVALID_FP( v_radps )
    CHECK_INVALID_FP( t_ff_Nm )
    CHECK_INVALID_FP( timeout_ms )
    int interface_ret = fsa_interface_set_v_tff( fsa_dev, v_radps, t_ff_Nm, timeout_ms, max_retry, cnt++ );
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::SetTorqueNoAck( double t_ff_Nm ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_INVALID_FP( t_ff_Nm )
    int interface_ret = fsa_interface_set_t_noack( fsa_dev, t_ff_Nm, cnt++ );
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::SetTorque( double t_ff_Nm, double timeout_ms, int max_retry ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_ARG_INVALID( timeout_ms, max_retry )
    CHECK_INVALID_FP( t_ff_Nm )
    CHECK_INVALID_FP( timeout_ms )
    int interface_ret = fsa_interface_set_t( fsa_dev, t_ff_Nm, timeout_ms, max_retry, cnt++ );
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::SetCurrentNoAck( double c_A ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_INVALID_FP( c_A )
    int interface_ret = fsa_interface_set_c_noack( fsa_dev, c_A, cnt++ );
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::SetCurrent( double c_A, double timeout_ms, int max_retry ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_ARG_INVALID( timeout_ms, max_retry )
    CHECK_INVALID_FP( c_A )
    CHECK_INVALID_FP( timeout_ms )
    int interface_ret = fsa_interface_set_c( fsa_dev, c_A, timeout_ms, max_retry, cnt++ );
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::GetPVCTTe( pvctte_t& rx_PVCTTe, double timeout_ms, int max_retry ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_ARG_INVALID( timeout_ms, max_retry )
    CHECK_INVALID_FP( timeout_ms )
    fsa_PVCTTe_t interface_PVCTTe;
    int          interface_ret = fsa_interface_get_PVCTTe( fsa_dev, &interface_PVCTTe, timeout_ms, max_retry, cnt++ );
    rx_PVCTTe.pos              = interface_PVCTTe.p;
    rx_PVCTTe.vel              = interface_PVCTTe.v;
    rx_PVCTTe.cur              = interface_PVCTTe.c;
    rx_PVCTTe.tor              = interface_PVCTTe.t;
    rx_PVCTTe.tor_e            = interface_PVCTTe.te;
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::EnableSubscribe( subs_config_t& subs_config, double timeout_ms, int max_retry ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_ARG_INVALID( timeout_ms, max_retry )
    CHECK_INVALID_FP( timeout_ms )
    fsa_subs_config_t interface_subs_config;
    interface_subs_config.enable            = subs_config.enable;
    interface_subs_config.freq              = subs_config.freq;
    interface_subs_config.keepalive_time_ms = subs_config.keepalive_time_ms;
    interface_subs_config.pos               = subs_config.pos;
    interface_subs_config.vel               = subs_config.vel;
    interface_subs_config.cur               = subs_config.cur;
    interface_subs_config.tor               = subs_config.tor;
    interface_subs_config.tor_em            = subs_config.tor_em;
    interface_subs_config.error             = subs_config.error;
    interface_subs_config.error_ext2        = subs_config.error_ext2;
    interface_subs_config.error_ext3        = subs_config.error_ext3;
    interface_subs_config.error_ext4        = subs_config.error_ext4;
    interface_subs_config.error_ext5        = subs_config.error_ext5;
    interface_subs_config.error_ext6        = subs_config.error_ext6;
    interface_subs_config.error_ext7        = subs_config.error_ext7;
    interface_subs_config.error_ext8        = subs_config.error_ext8;
    interface_subs_config.temp_mos          = subs_config.temp_mos;
    interface_subs_config.temp_coil         = subs_config.temp_coil;
    interface_subs_config.vbus              = subs_config.vbus;
    int interface_ret                       = fsa_interface_config_subscribe( fsa_dev, &interface_subs_config, timeout_ms, max_retry, cnt++ );
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::GetSubsData( subs_data_t& subs_data ) {
    fsa_subs_data_t interface_subs_data;
    interface_subs_data.cnt = subs_data.cnt;
    int interface_ret;

#ifdef _WIN32
    // Windows not support rtcko
    interface_ret = fsa_interface_get_subs_data( fsa_dev, &interface_subs_data );
#else
    if ( fsa_rtcko_dev.rtcko_fd >= 0 ) {
        interface_ret = fsa_interface_get_rtcko_subs_data( fsa_dev, &interface_subs_data );
    }
    else {
        interface_ret = fsa_interface_get_subs_data( fsa_dev, &interface_subs_data );
    }
#endif
    if ( interface_ret != FSA_RET_SUCCESS ) {
        return ( ret_e )interface_ret;
    }
    subs_data.cnt                  = interface_subs_data.cnt;
    subs_data.pvct.pos             = interface_subs_data.PVCTTe.p;
    subs_data.pvct.vel             = interface_subs_data.PVCTTe.v;
    subs_data.pvct.cur             = interface_subs_data.PVCTTe.c;
    subs_data.pvct.tor             = interface_subs_data.PVCTTe.t;
    subs_data.pvct.tor_e           = interface_subs_data.PVCTTe.te;
    subs_data.error[ 0 ]           = interface_subs_data.error[ 0 ];
    subs_data.error[ 1 ]           = interface_subs_data.error[ 1 ];
    subs_data.error[ 2 ]           = interface_subs_data.error[ 2 ];
    subs_data.error[ 3 ]           = interface_subs_data.error[ 3 ];
    subs_data.error[ 4 ]           = interface_subs_data.error[ 4 ];
    subs_data.error[ 5 ]           = interface_subs_data.error[ 5 ];
    subs_data.error[ 6 ]           = interface_subs_data.error[ 6 ];
    subs_data.error[ 7 ]           = interface_subs_data.error[ 7 ];
    subs_data.temp_vbus.mos        = interface_subs_data.temp_vbus.mos;
    subs_data.temp_vbus.coil       = interface_subs_data.temp_vbus.coil;
    subs_data.temp_vbus.vbus       = interface_subs_data.temp_vbus.vbus;
    subs_data.latency_from_recv_ns = interface_subs_data.latency_from_recv_ns;
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::GetTempVbus( temp_vbus_t& rx_temp_vbus, double timeout_ms, int max_retry ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_ARG_INVALID( timeout_ms, max_retry )
    CHECK_INVALID_FP( timeout_ms )
    fsa_temp_vbus_t interface_temp_vbus;
    int             interface_ret = fsa_interface_get_temp_vbus( fsa_dev, &interface_temp_vbus, timeout_ms, max_retry, cnt++ );
    rx_temp_vbus.mos              = interface_temp_vbus.mos;
    rx_temp_vbus.coil             = interface_temp_vbus.coil;
    rx_temp_vbus.vbus             = interface_temp_vbus.vbus;
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::GetErrCode( err_code_t& rx_errcode, double timeout_ms, int max_retry ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_ARG_INVALID( timeout_ms, max_retry )
    CHECK_INVALID_FP( timeout_ms )
    int interface_ret = fsa_interface_get_error_code( fsa_dev, rx_errcode.arr.data(), timeout_ms, max_retry, cnt++ );
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::SetPIDParams( double p_kp, double v_kp, double v_ki, double timeout_ms, int max_retry ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_ARG_INVALID( timeout_ms, max_retry )
    CHECK_INVALID_FP( p_kp )
    CHECK_INVALID_FP( v_kp )
    CHECK_INVALID_FP( v_ki )
    CHECK_INVALID_FP( timeout_ms )
    int interface_ret = fsa_interface_set_pid( fsa_dev, p_kp, v_kp, v_ki, timeout_ms, max_retry, cnt++ );
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::GetPIDParams( pid_param_t& rx_pid, double timeout_ms, int max_retry ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_ARG_INVALID( timeout_ms, max_retry )
    CHECK_INVALID_FP( timeout_ms )
    fsa_pid_t interface_pid;
    int       interface_ret = fsa_interface_get_pid( fsa_dev, &interface_pid, timeout_ms, max_retry, cnt++ );
    rx_pid.pos_kp           = interface_pid.p_kp;
    rx_pid.vel_kp           = interface_pid.v_kp;
    rx_pid.vel_ki           = interface_pid.v_ki;
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::SetPDParams( double pd_kp, double pd_kd, double timeout_ms, int max_retry ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_ARG_INVALID( timeout_ms, max_retry )
    CHECK_INVALID_FP( pd_kp )
    CHECK_INVALID_FP( pd_kd )
    CHECK_INVALID_FP( timeout_ms )
    int interface_ret = fsa_interface_set_pd( fsa_dev, pd_kp, pd_kd, timeout_ms, max_retry, cnt++ );
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::GetPDParams( pd_param_t& rx_pd, double timeout_ms, int max_retry ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_ARG_INVALID( timeout_ms, max_retry )
    CHECK_INVALID_FP( timeout_ms )
    fsa_pd_t interface_pd;
    int      interface_ret = fsa_interface_get_pd( fsa_dev, &interface_pd, timeout_ms, max_retry, cnt++ );
    rx_pd.pd_kp            = interface_pd.pd_kp;
    rx_pd.pd_kd            = interface_pd.pd_kd;
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::CommTimeoutProtect( comm_timeout_protect_config_t& comm_timeout_protect_config, double timeout_ms, int max_retry ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_ARG_INVALID( timeout_ms, max_retry )
    CHECK_INVALID_FP( timeout_ms )
    fsa_comm_timeout_protect_config_t interface_comm_timeout_protect_config;
    interface_comm_timeout_protect_config.config_timeout_ms     = comm_timeout_protect_config.config_timeout_ms;
    interface_comm_timeout_protect_config.set_timeout_ms        = comm_timeout_protect_config.set_timeout_ms;
    interface_comm_timeout_protect_config.config_protect_pos_kp = comm_timeout_protect_config.config_protect_pos_kp;
    interface_comm_timeout_protect_config.config_protect_vel_kp = comm_timeout_protect_config.config_protect_vel_kp;
    interface_comm_timeout_protect_config.config_protect_vel_ki = comm_timeout_protect_config.config_protect_vel_ki;
    interface_comm_timeout_protect_config.set_protect_pid.p_kp  = comm_timeout_protect_config.set_protect_pid.pos_kp;
    interface_comm_timeout_protect_config.set_protect_pid.v_kp  = comm_timeout_protect_config.set_protect_pid.vel_kp;
    interface_comm_timeout_protect_config.set_protect_pid.v_ki  = comm_timeout_protect_config.set_protect_pid.vel_ki;

    int interface_ret = fsa_interface_comm_timeout_protect( fsa_dev, &interface_comm_timeout_protect_config, timeout_ms, max_retry, cnt++ );

    comm_timeout_protect_config.get_timeout_ms         = interface_comm_timeout_protect_config.get_timeout_ms;
    comm_timeout_protect_config.get_protect_pid.pos_kp = interface_comm_timeout_protect_config.get_protect_pid.p_kp;
    comm_timeout_protect_config.get_protect_pid.vel_kp = interface_comm_timeout_protect_config.get_protect_pid.v_kp;
    comm_timeout_protect_config.get_protect_pid.vel_ki = interface_comm_timeout_protect_config.get_protect_pid.v_ki;
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::SetAbsEncoderZero( double timeout_ms, int max_retry ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_ARG_INVALID( timeout_ms, max_retry )
    CHECK_INVALID_FP( timeout_ms )
    int interface_ret = fsa_interface_set_abs_encoder_zero( fsa_dev, timeout_ms, max_retry, cnt++ );
    return ( ret_e )interface_ret;
}

struct ErrorList {
    uint32_t    mask;
    int8_t      error_level;
    const char* str;
};
#define PER_ERROR_CODE_BIT_NUM 32
static constexpr const uint32_t ERROR2_TYPE_MASK        = 0X03;
static constexpr const uint32_t ERROR2_RESERVE_TYPE     = 0X00;
static constexpr const uint32_t ERROR2_ENCODER1_TYPE    = 0X01;
static constexpr const uint32_t ERROR2_LINER_HALL1_TYPE = 0X02;
static constexpr const uint32_t ERROR2_ENCODER2_TYPE    = 0X03;

static constexpr const ErrorList error_list1[ PER_ERROR_CODE_BIT_NUM ] = {
    { 0x00000001, 3, "ERROR1_ADC_CAL_FAULT" },          // 相电流偏置校准错误
    { 0x00000002, 3, "ERROR1_OVER_CURRENT" },           // 电流过大
    { 0x00000004, 3, "ERROR1_OVER_VBUS" },              // 电压过高
    { 0x00000008, 3, "ERROR1_UNDER_VBUS" },             // 电压过低
    { 0x00000010, 3, "ERROR1_FOCENCODER_NOT_CALI" },    // FOC角度传感器未校准
    { 0x00000020, 3, "ERROR1_HIGHSPEED_ENCODER_ERR" },  // 高速编码器错误
    { 0x00000040, 3, "ERROR1_LOWSPEED_ENCODER_ERR" },   // 低速编码器错误
    { 0x00000080, 3, "ERROR1_OVER_LOAD_TMP" },          // 电机绕组过载过温
    { 0x00000100, 3, "ERROR1_MOSFET_ERROR" },           // MOS异常
    { 0x00000200, 3, "ERROR1_OVER_TEMP_COIL" },         // 绕组温度过高
    { 0x00000400, 3, "ERROR1_OVER_TEMP_MOSFET" },       // MOS温度过高
    { 0x00000800, 3, "ERROR1_OVER_TEMP_TRIP" },         // 主控芯片温度过高
    { 0x00001000, 3, "ERROR1_U_PHASE_LOSS" },           // U相丢失
    { 0x00002000, 3, "ERROR1_V_PHASE_LOSS" },           // V相丢失
    { 0x00004000, 3, "ERROR1_W_PHASE_LOSS" },           // W相丢失
    { 0x00008000, 3, "ERROR1_LOSSPHASECHECK_ERROR" },   // 自检缺相
    { 0x00010000, 3, "ERROR1_MOTOR_RUNAWAY" },          // 飞车保护
    { 0x00020000, 3, "ERROR1_RESERVE_0x20000" },        // 保留
    { 0x00040000, 2, "ERROR1_RESERVE_0x40000" },        // 保留
    { 0x00080000, 3, "ERROR1_PARAM_LIST_ERR" },         // 获取参数列表错误
    { 0x00100000, 3, "ERROR1_RESERVE_0x00100000" },     // 保留
    { 0x00200000, 3, "ERROR1_RESERVE_0x00200000" },     // 保留
    { 0x00400000, 3, "ERROR1_RESERVE_0x00400000" },     // 保留
    { 0x00800000, 3, "ERROR1_RESERVE_0x00800000" },     // 保留
    { 0x01000000, 3, "ERROR1_RESERVE_0x01000000" },     // 保留
    { 0x02000000, 3, "ERROR1_RESERVE_0x02000000" },     // 保留
    { 0x04000000, 3, "ERROR1_RESERVE_0x04000000" },     // 保留
    { 0x08000000, 3, "ERROR1_RESERVE_0x08000000" },     // 保留
    { 0x10000000, 3, "ERROR1_RESERVE_0x10000000" },     // 保留
    { 0x20000000, 3, "ERROR1_RESERVE_0x20000000" },     // 保留
    { 0x40000000, 3, "ERROR1_RESERVE_0x40000000" },     // 保留
    { 0x80000000, 3, "ERROR1_RESERVE_0x80000000" },     // 保留
};
static constexpr const ErrorList error_list2_reserve[ PER_ERROR_CODE_BIT_NUM ] = {
    { 0x00000001, 3, "ERROR2_RESERVE_0x00000001" },  // 保留
    { 0x00000002, 3, "ERROR2_RESERVE_0x00000002" },  // 保留
    { 0x00000004, 3, "ERROR2_RESERVE_0x00000004" },  // 保留
    { 0x00000008, 3, "ERROR2_RESERVE_0x00000008" },  // 保留
    { 0x00000010, 3, "ERROR2_RESERVE_0x00000010" },  // 保留
    { 0x00000020, 3, "ERROR2_RESERVE_0x00000020" },  // 保留
    { 0x00000040, 3, "ERROR2_RESERVE_0x00000040" },  // 保留
    { 0x00000080, 3, "ERROR2_RESERVE_0x00000080" },  // 保留
    { 0x00000100, 3, "ERROR2_RESERVE_0x00000100" },  // 保留
    { 0x00000200, 3, "ERROR2_RESERVE_0x00000200" },  // 保留
    { 0x00000400, 3, "ERROR2_RESERVE_0x00000400" },  // 保留
    { 0x00000800, 3, "ERROR2_RESERVE_0x00000800" },  // 保留
    { 0x00001000, 3, "ERROR2_RESERVE_0x00001000" },  // 保留
    { 0x00002000, 3, "ERROR2_RESERVE_0x00002000" },  // 保留
    { 0x00004000, 3, "ERROR2_RESERVE_0x00004000" },  // 保留
    { 0x00008000, 3, "ERROR2_RESERVE_0x00008000" },  // 保留
    { 0x00010000, 3, "ERROR2_RESERVE_0x00010000" },  // 保留
    { 0x00020000, 3, "ERROR2_RESERVE_0x00020000" },  // 保留
    { 0x00040000, 3, "ERROR2_RESERVE_0x00040000" },  // 保留
    { 0x00080000, 3, "ERROR2_RESERVE_0x00080000" },  // 保留
    { 0x00100000, 3, "ERROR2_RESERVE_0x00100000" },  // 保留
    { 0x00200000, 3, "ERROR2_RESERVE_0x00200000" },  // 保留
    { 0x00400000, 3, "ERROR2_RESERVE_0x00400000" },  // 保留
    { 0x00800000, 3, "ERROR2_RESERVE_0x00800000" },  // 保留
    { 0x01000000, 3, "ERROR2_RESERVE_0x01000000" },  // 保留
    { 0x02000000, 3, "ERROR2_RESERVE_0x02000000" },  // 保留
    { 0x04000000, 3, "ERROR2_RESERVE_0x04000000" },  // 保留
    { 0x08000000, 3, "ERROR2_RESERVE_0x08000000" },  // 保留
    { 0x10000000, 3, "ERROR2_RESERVE_0x10000000" },  // 保留
    { 0x20000000, 3, "ERROR2_RESERVE_0x20000000" },  // 保留
    { 0x40000000, 3, "ERROR2_RESERVE_0x40000000" },  // 保留
    { 0x80000000, 3, "ERROR2_RESERVE_0x80000000" },  // 保留
};
static constexpr const ErrorList error_list2_encoder1[ PER_ERROR_CODE_BIT_NUM ] = {
    { 0x00000001, 3, "ERROR2_INVALID" },                     // 无效错误
    { 0x00000002, 3, "ERROR2_INVALID" },                     // 无效错误
    { 0x00000004, 3, "ERROR2_ENCODER_NOT_CAIL" },            // 磁编码器未校准
    { 0x00000008, 3, "ERROR2_ENCODER_CALIBRATION_ERR" },     // 磁编码器校准错误
    { 0x00000010, 3, "ERROR2_ENCODER_COMMUNICATION_ERR" },   // 磁编码器通信错误
    { 0x00000020, 3, "ERROR2_ENCODER_PORT_NULL" },           // 磁编码器空指针错误
    { 0x00000040, 3, "ERROR2_ENCODER_AMPLITUDE_ERR" },       // 磁编码器幅值错误
    { 0x00000080, 3, "ERROR2_ENCODER_ANGLE_FULL_ERR" },      // 磁编码器最大值错误
    { 0x00000100, 3, "ERROR2_ENCODER_REVERSAL_ERR" },        // 磁编码器反转错误
    { 0x00000200, 3, "ERROR2_ENCODER_OVER_MAX_PP" },         // 磁编码器配置极对数超上限
    { 0x00000400, 3, "ERROR2_ENCODER_ECCENTRIC_ERR" },       // 磁编码器偏心校准数据异常
    { 0x00000800, 3, "ERROR2_ENCODER_WRITE_DATA_ERR" },      // 磁编码器写数据错误
    { 0x00001000, 3, "ERROR2_ENCODER_RESERVE_0x00001000" },  // 磁编码器保留
    { 0x00002000, 3, "ERROR2_ENCODER_RESERVE_0x00002000" },  // 磁编码器保留
    { 0x00004000, 3, "ERROR2_ENCODER_RESERVE_0x00004000" },  // 磁编码器保留
    { 0x00008000, 3, "ERROR2_ENCODER_RESERVE_0x00008000" },  // 磁编码器保留
    { 0x00010000, 3, "ERROR2_ENCODER_RESERVE_0x00010000" },  // 磁编码器保留
    { 0x00020000, 3, "ERROR2_ENCODER_INNER_ROTOR_NEAR" },    // 金刚编码器内圈转子过近
    { 0x00040000, 3, "ERROR2_ENCODER_INNER_ROTOR_FAR" },     // 金刚编码器内圈转子过远
    { 0x00080000, 3, "ERROR2_ENCODER_OUTER_ROTOR_NEAR" },    // 金刚编码器外圈转子过近
    { 0x00100000, 3, "ERROR2_ENCODER_OUTER_ROTOR_FAR" },     // 金刚编码器外圈转子过远
    { 0x00200000, 1, "WARNING2_ENCODER_INNER_ROTOR_NEAR" },  // 金刚编码器内圈转子较近
    { 0x00400000, 1, "WARNING2_ENCODER_INNER_ROTOR_FAR" },   // 金刚编码器内圈转子较远
    { 0x00800000, 1, "WARNING2_ENCODER_OUTER_ROTOR_NEAR" },  // 金刚编码器外圈转子较近
    { 0x01000000, 1, "WARNING2_ENCODER_OUTER_ROTOR_FAR" },   // 金刚编码器外圈转子较远
    { 0x02000000, 3, "ERROR2_ENCODER_INNER" },               // 金刚编码器内圈错误
    { 0x04000000, 3, "ERROR2_ENCODER_OUTER" },               // 金刚编码器外圈错误
    { 0x08000000, 3, "ERROR2_ENCODER_RESERVE_0x08000000" },  // 金刚编码器保留
    { 0x10000000, 3, "ERROR2_ENCODER_RESERVE_0x10000000" },  // 金刚编码器保留
    { 0x20000000, 3, "ERROR2_ENCODER_RESERVE_0x20000000" },  // 金刚编码器保留
    { 0x40000000, 3, "ERROR2_ENCODER_RESERVE_0x40000000" },  // 金刚编码器保留
    { 0x80000000, 3, "ERROR2_ENCODER_RESERVE_0x80000000" },  // 金刚编码器保留
};
static constexpr const ErrorList error_list2_liner_hall1[ PER_ERROR_CODE_BIT_NUM ] = {
    { 0x00000001, 3, "ERROR2_INVALID" },                           // 无效错误
    { 0x00000002, 3, "ERROR2_INVALID" },                           // 无效错误
    { 0x00000004, 3, "ERROR2_LINER_HALL_NOT_CAIL" },               // 线性霍尔未校准
    { 0x00000008, 3, "ERROR2_LINER_HALL_AMP_ERR" },                // 线性霍尔最终校准值幅值错误
    { 0x00000010, 3, "ERROR2_LINER_HALL_MED_ERR" },                // 线性霍尔中值错误
    { 0x00000020, 3, "ERROR2_LINER_HALL_MIN_ERR" },                // 线性霍尔最小值错误
    { 0x00000040, 3, "ERROR2_LINER_HALL_MAX_ERR" },                // 线性霍尔最大值错误
    { 0x00000080, 3, "ERROR2_LINER_HALL_ANGLE_OFFSET_ERR" },       // 线性霍尔电角度偏置错误
    { 0x00000100, 3, "ERROR2_LINER_HALL_REVERSAL_ERR" },           // 线性霍尔反转错误
    { 0x00000200, 3, "ERROR2_LINER_HALL_PORT_NULL" },              // 线性霍尔空指针错误
    { 0x00000400, 3, "ERROR2_LINER_HALL_AMP_ARRAY_ERR" },          // 线性霍尔某一个电周期幅值错误
    { 0x00000800, 3, "ERROR2_LINER_HALL_ANGLE_OFFSET_AVEG_ERR" },  // 线性霍尔校准值与平均校准值偏差过大
    { 0x00001000, 3, "ERROR2_LINER_HALL_MED_AVEG_ERR" },           // 线性霍尔中值与平均中值偏差过大
    { 0x00002000, 3, "ERROR2_LINER_HALL_PHASE_DELTA_ERR" },        // 线性霍尔相位差错误
    { 0x00004000, 3, "ERROR2_LINER_HALL_COMM_ERR" },               // 线性霍尔通讯错误
    { 0x00008000, 3, "ERROR2_LINER_HALL_WRITE_DATA_ERR" },         // 线性霍尔写数据错误
    { 0x00010000, 3, "ERROR2_LINER_HALL_RESERVE_0x00010000" },     // 保留
    { 0x00020000, 3, "ERROR2_LINER_HALL_RESERVE_0x00020000" },     // 保留
    { 0x00040000, 3, "ERROR2_LINER_HALL_RESERVE_0x00040000" },     // 保留
    { 0x00080000, 3, "ERROR2_LINER_HALL_RESERVE_0x00080000" },     // 保留
    { 0x00100000, 3, "ERROR2_LINER_HALL_RESERVE_0x00100000" },     // 保留
    { 0x00200000, 3, "ERROR2_LINER_HALL_RESERVE_0x00200000" },     // 保留
    { 0x00400000, 3, "ERROR2_LINER_HALL_RESERVE_0x00400000" },     // 保留
    { 0x00800000, 3, "ERROR2_LINER_HALL_RESERVE_0x00800000" },     // 保留
    { 0x01000000, 3, "ERROR2_LINER_HALL_RESERVE_0x01000000" },     // 保留
    { 0x02000000, 3, "ERROR2_LINER_HALL_RESERVE_0x02000000" },     // 保留
    { 0x04000000, 3, "ERROR2_LINER_HALL_RESERVE_0x04000000" },     // 保留
    { 0x08000000, 3, "ERROR2_LINER_HALL_RESERVE_0x08000000" },     // 保留
    { 0x10000000, 3, "ERROR2_LINER_HALL_RESERVE_0x10000000" },     // 保留
    { 0x20000000, 3, "ERROR2_LINER_HALL_RESERVE_0x20000000" },     // 保留
    { 0x40000000, 3, "ERROR2_LINER_HALL_RESERVE_0x40000000" },     // 保留
    { 0x80000000, 3, "ERROR2_LINER_HALL_RESERVE_0x80000000" },     // 保留
};
static constexpr const ErrorList error_list2_encoder2[ PER_ERROR_CODE_BIT_NUM ] = {
    { 0x00000001, 3, "ERROR2_INVALID" },                          // 无效错误
    { 0x00000002, 3, "ERROR2_INVALID" },                          // 无效错误
    { 0x00000004, 3, "ERROR2_ENCODER_NOT_CAIL" },                 // 磁编码器未校准
    { 0x00000008, 3, "ERROR2_ENCODER_CALIBRATION_ERR" },          // 磁编码器校准错误
    { 0x00000010, 3, "ERROR2_ENCODER_COMMUNICATION_ERR" },        // 磁编码器通信错误
    { 0x00000020, 3, "ERROR2_ENCODER_PORT_NULL" },                // 磁编码器空指针错误
    { 0x00000040, 3, "ERROR2_ENCODER_AMPLITUDE_ERR" },            // 磁编码器幅值错误
    { 0x00000080, 3, "ERROR2_ENCODER_ANGLE_FULL_ERR" },           // 磁编码器最大值错误
    { 0x00000100, 3, "ERROR2_ENCODER_REVERSAL_ERR" },             // 磁编码器反转错误
    { 0x00000200, 3, "ERROR2_ENCODER_OVER_MAX_PP" },              // 磁编码器配置极对数超上限
    { 0x00000400, 3, "ERROR2_ENCODER_ECCENTRIC_ERR" },            // 磁编码器偏心校准数据异常
    { 0x00000800, 3, "ERROR2_ENCODER_WRITE_DATA_ERR" },           // 磁编码器写数据错误
    { 0x00001000, 3, "ERROR2_ENCODER_RESERVE_0x00001000" },       // 磁编码器保留
    { 0x00002000, 3, "ERROR2_ENCODER_RESERVE_0x00002000" },       // 磁编码器保留
    { 0x00004000, 3, "ERROR2_ENCODER_RESERVE_0x00004000" },       // 磁编码器保留
    { 0x00008000, 3, "ERROR2_ENCODER_RESERVE_0x00008000" },       // 磁编码器保留
    { 0x00010000, 3, "ERROR2_ENCODER_RESERVE_0x00010000" },       // 磁编码器保留
    { 0x00020000, 3, "ERROR2_ENCODER_GW_OUTSPEED" },              // 弓望编码器异常导致失速
    { 0x00040000, 3, "ERROR2_ENCODER_RESERVE_0x00040000" },       // 弓望编码器保留
    { 0x00080000, 3, "ERROR2_ENCODER_GW_INNER_ROTOR_NEAR_FAR" },  // 弓望编码器电机端气隙过近过远
    { 0x00100000, 3, "ERROR2_ENCODER_RESERVE_0x00100000" },       // 弓望编码器保留
    { 0x00200000, 3, "ERROR2_ENCODER_GW_HIGH_TEMP" },             // 弓望编码器过温超125°
    { 0x00400000, 3, "ERROR2_ENCODER_RESERVE_0x00400000" },       // 弓望编码器保留
    { 0x00800000, 3, "ERROR2_ENCODER_RESERVE_0x00800000" },       // 弓望编码器保留
    { 0x01000000, 3, "ERROR2_ENCODER_RESERVE_0x01000000" },       // 弓望编码器保留
    { 0x02000000, 3, "ERROR2_ENCODER_RESERVE_0x02000000" },       // 弓望编码器保留
    { 0x04000000, 3, "ERROR2_ENCODER_RESERVE_0x04000000" },       // 弓望编码器保留
    { 0x08000000, 3, "ERROR2_ENCODER_RESERVE_0x08000000" },       // 弓望编码器保留
    { 0x10000000, 3, "ERROR2_ENCODER_RESERVE_0x10000000" },       // 弓望编码器保留
    { 0x20000000, 3, "ERROR2_ENCODER_RESERVE_0x20000000" },       // 弓望编码器保留
    { 0x40000000, 3, "ERROR2_ENCODER_RESERVE_0x40000000" },       // 弓望编码器保留
    { 0x80000000, 3, "ERROR2_ENCODER_RESERVE_0x80000000" },       // 弓望编码器保留
};
static constexpr const ErrorList error_list3[ PER_ERROR_CODE_BIT_NUM ] = {
    { 0x00000001, 1, "WARNING3_MOS_HIGH_TMP" },       // 电机MOS高温警告
    { 0x00000002, 1, "WARNING3_ARMATURE_HIGH_TMP" },  // 电机绕组高温警告
    { 0x00000004, 1, "WARNING3_NTC" },                // NTC故障
    { 0x00000008, 1, "WARNING3_DOUBLE_ENCODER" },     // 双编码器警告
    { 0x00000010, 1, "WARNING3_FPU" },                // FPU 除零或非法操作警告
    { 0x00000020, 2, "WARNING3_SOFT_LIMIT_ERR" },     // 软限位警告
    { 0x00000040, 2, "WARNING3_COMM_TIMEOUT" },       // 通信超时保护
    { 0x00000080, 1, "WARNING3_PARAMLIST_VER" },      // 参数列表版本号警告
    { 0x00000100, 3, "ERROR3_RESERVE_0X00000100" },   // 保留
    { 0x00000200, 3, "ERROR3_RESERVE_0X00000200" },   // 保留
    { 0x00000400, 3, "ERROR3_RESERVE_0X00000400" },   // 保留
    { 0x00000800, 3, "ERROR3_RESERVE_0X00000800" },   // 保留
    { 0x00001000, 3, "ERROR3_RESERVE_0X00001000" },   // 保留
    { 0x00002000, 3, "ERROR3_RESERVE_0X00002000" },   // 保留
    { 0x00004000, 3, "ERROR3_RESERVE_0X00004000" },   // 保留
    { 0x00008000, 3, "ERROR3_RESERVE_0X00008000" },   // 保留
    { 0x00010000, 3, "ERROR3_RESERVE_0X00010000" },   // 保留
    { 0x00020000, 3, "ERROR3_RESERVE_0X00020000" },   // 保留
    { 0x00040000, 3, "ERROR3_RESERVE_0X00040000" },   // 保留
    { 0x00080000, 3, "ERROR3_RESERVE_0X00080000" },   // 保留
    { 0x00100000, 3, "ERROR3_RESERVE_0X00100000" },   // 保留
    { 0x00200000, 3, "ERROR3_RESERVE_0X00200000" },   // 保留
    { 0x00400000, 3, "ERROR3_RESERVE_0X00400000" },   // 保留
    { 0x00800000, 3, "ERROR3_RESERVE_0X00800000" },   // 保留
    { 0x01000000, 3, "ERROR3_RESERVE_0X01000000" },   // 保留
    { 0x02000000, 3, "ERROR3_RESERVE_0X02000000" },   // 保留
    { 0x04000000, 3, "ERROR3_RESERVE_0X04000000" },   // 保留
    { 0x08000000, 3, "ERROR3_RESERVE_0X08000000" },   // 保留
    { 0x10000000, 3, "ERROR3_RESERVE_0X10000000" },   // 保留
    { 0x20000000, 3, "ERROR3_RESERVE_0X20000000" },   // 保留
    { 0x40000000, 3, "ERROR3_RESERVE_0X40000000" },   // 保留
    { 0x80000000, 3, "ERROR3_RESERVE_0X80000000" },   // 保留
};
static constexpr const ErrorList error_list8[ PER_ERROR_CODE_BIT_NUM ] = {
    { 0x00000001, -2, "INFO8_M7_FW_CHANGED" },          // 检测到M7固件被修改
    { 0x00000002, -2, "INFO8_M7_VER_CHANGED" },         // 检测到M7版本被修改
    { 0x00000004, -2, "INFO8_M4_FW_CHANGED" },          // 检测到M4固件被修改
    { 0x00000008, -2, "INFO8_M4_VER_CHANGED" },         // 检测到M4版本被修改
    { 0x00000010, -1, "INFO8_SAME_IP_DETECTED" },       // 检测到相同IP
    { 0x00000020, -2, "INFO8_FW_INFO_INVALID" },        // 无效的固件信息
    { 0x00000040, 3, "ERROR8_DRV_VERSION_NOT_MATCH" },  // 驱动版本不匹配
    { 0x00000080, -2, "INFO8_CHECK_SPI_FLASH" },        // 正在检查SPI FLASH
    { 0x00000100, 1, "WARNING8_SPI_FLASH_WARNING" },    // SPI FLASH 警告
    { 0x00000200, 3, "ERROR8_SPI_FLASH_ERROR" },        // SPI FLASH 错误
    { 0x00000400, 3, "ERROR8_RESERVE_0X00000400" },     // 保留
    { 0x00000800, 3, "ERROR8_RESERVE_0X00000800" },     // 保留
    { 0x00001000, 3, "ERROR8_RESERVE_0X00001000" },     // 保留
    { 0x00002000, 3, "ERROR8_RESERVE_0X00002000" },     // 保留
    { 0x00004000, 3, "ERROR8_RESERVE_0X00004000" },     // 保留
    { 0x00008000, 3, "ERROR8_RESERVE_0X00008000" },     // 保留
    { 0x00010000, 3, "ERROR8_RESERVE_0X00010000" },     // 保留
    { 0x00020000, 3, "ERROR8_RESERVE_0X00020000" },     // 保留
    { 0x00040000, 3, "ERROR8_RESERVE_0X00040000" },     // 保留
    { 0x00080000, 3, "ERROR8_RESERVE_0X00080000" },     // 保留
    { 0x00100000, 3, "ERROR8_RESERVE_0X00100000" },     // 保留
    { 0x00200000, 3, "ERROR8_RESERVE_0X00200000" },     // 保留
    { 0x00400000, 3, "ERROR8_RESERVE_0X00400000" },     // 保留
    { 0x00800000, 3, "ERROR8_RESERVE_0X00800000" },     // 保留
    { 0x01000000, 3, "ERROR8_RESERVE_0X01000000" },     // 保留
    { 0x02000000, 3, "ERROR8_RESERVE_0X02000000" },     // 保留
    { 0x04000000, 3, "ERROR8_RESERVE_0X04000000" },     // 保留
    { 0x08000000, 3, "ERROR8_RESERVE_0X08000000" },     // 保留
    { 0x10000000, 3, "ERROR8_RESERVE_0X10000000" },     // 保留
    { 0x20000000, 3, "ERROR8_RESERVE_0X20000000" },     // 保留
    { 0x40000000, 3, "ERROR8_RESERVE_0X40000000" },     // 保留
    { 0x80000000, 3, "ERROR8_RESERVE_0X80000000" },     // 保留
};

FSA::ret_e FSA::ParseErrCode( const err_code_t err_code, parsed_err_code_t& parsed_err_code ) {
    int error2_type = err_code.arr[ 1 ] & ERROR2_TYPE_MASK;
    parsed_err_code.vec.clear();
    for ( uint32_t u32_index = 0; u32_index < 8; u32_index++ ) {
        // step1: 取出当前的错误码，如果为0，表示无错误，跳过
        uint32_t u32_value;
        if ( u32_index == 1 )
            u32_value = err_code.arr[ u32_index ] & ( ~ERROR2_TYPE_MASK );  // 过滤掉类型位
        else
            u32_value = err_code.arr[ u32_index ];
        if ( u32_value == 0 )
            continue;  // 无错误，跳过
        // step2: 根据索引选择对应的错误列表
        const ErrorList* p_error_list       = nullptr;
        int              parsed_code_offset = 0;
        if ( u32_index == 0 ) {
            p_error_list = error_list1;
        }
        else if ( u32_index == 1 ) {
            if ( error2_type == ERROR2_ENCODER1_TYPE ) {
                parsed_code_offset = ERROR2_ENCODER1_TYPE << 5;
                p_error_list       = error_list2_encoder1;
            }
            else if ( error2_type == ERROR2_LINER_HALL1_TYPE ) {
                parsed_code_offset = ERROR2_LINER_HALL1_TYPE << 5;
                p_error_list       = error_list2_liner_hall1;
            }
            else if ( error2_type == ERROR2_ENCODER2_TYPE ) {
                parsed_code_offset = ERROR2_ENCODER2_TYPE << 5;
                p_error_list       = error_list2_encoder2;
            }
            else if ( error2_type == ERROR2_RESERVE_TYPE ) {
                parsed_code_offset = ERROR2_RESERVE_TYPE << 5;
                p_error_list       = error_list2_reserve;
            }
            else {
                continue;  // 未知类型，跳过
            }
        }
        else if ( u32_index == 2 ) {
            p_error_list = error_list3;
        }
        else if ( u32_index == 7 ) {
            p_error_list = error_list8;
        }
        else {
            continue;  // 其他为预留，跳过
        }
        // step3: 解析当前的错误码
        while ( u32_value ) {
            int bit_index = find_first_set_bit_lsb( u32_value );
            u32_value &= ~( 1 << bit_index );  // 清除最低位的1
            parsed_err_code_item_t item;
            item.err_level     = p_error_list[ bit_index ].error_level;
            item.err_u32_index = u32_index;
            item.err_bit_index = bit_index;
            item.parsed_code   = 0x3000U + ( ( u32_index + 1 ) << 8 ) + parsed_code_offset + bit_index;
            item.str_for_log   = p_error_list[ bit_index ].str;
            parsed_err_code.vec.push_back( item );
        }
    }
    return FSA::ret_e::SUCCESS;
}

FSA::ret_e FSA::GetTypeSubversion( type_subversion_t& type_subversion, double timeout_ms, int max_retry ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_ARG_INVALID( timeout_ms, max_retry )
    CHECK_INVALID_FP( timeout_ms )
    fsa_type_subversion_t interface_type_subversion;
    int                   interface_ret = fsa_interface_get_type_subversion( fsa_dev, &interface_type_subversion, timeout_ms, max_retry, cnt++ );
    memcpy( type_subversion.type.data(), interface_type_subversion.type, sizeof( type_subversion.type ) );
    memcpy( type_subversion.sub_version.data(), interface_type_subversion.sub_version, sizeof( type_subversion.sub_version ) );
    return ( ret_e )interface_ret;
}

FSA::ret_e FSA::GetCommConfig( comm_req_get_t& comm_req_get, comm_resp_get_t& comm_resp_get, double timeout_ms, int max_retry ) {
    CHECK_FSA_DEV( fsa_dev )
    CHECK_ARG_INVALID( timeout_ms, max_retry )
    CHECK_INVALID_FP( timeout_ms )

    int        net_ret    = 0;
    FSA::ret_e ret        = FSA::ret_e::SUCCESS;
    cJSON*     cjson_send = NULL;
    cJSON*     cjson_recv = NULL;
    char*      cjson_str  = NULL;
    uint8_t    recv_buf[ 1400 ];

    cjson_send = cJSON_CreateObject();
    if ( cjson_send == NULL ) {
        ret = FSA::ret_e::INTERFACE_HANDLE_ERR;
        goto end;
    }
    // 生成通用部分
    if ( cJSON_AddStringToObject( cjson_send, "method", "GET" ) == nullptr )
        goto end;
    if ( cJSON_AddStringToObject( cjson_send, "reqTarget", "/custom" ) == nullptr )
        goto end;
// 生成请求参数
#define REQ_GET_GEN( obj, req, key )                             \
    do {                                                         \
        if ( req.key )                                           \
            if ( cJSON_AddTrueToObject( obj, #key ) == nullptr ) \
                goto end;                                        \
    } while ( 0 )
    REQ_GET_GEN( cjson_send, comm_req_get, name );
    REQ_GET_GEN( cjson_send, comm_req_get, type );
    REQ_GET_GEN( cjson_send, comm_req_get, mcu_fw_version );
    REQ_GET_GEN( cjson_send, comm_req_get, mac_address );
    REQ_GET_GEN( cjson_send, comm_req_get, uid );
    REQ_GET_GEN( cjson_send, comm_req_get, sn );
    REQ_GET_GEN( cjson_send, comm_req_get, PCBA_sn );
    REQ_GET_GEN( cjson_send, comm_req_get, gearbox_sn );
    REQ_GET_GEN( cjson_send, comm_req_get, static_IP );
    REQ_GET_GEN( cjson_send, comm_req_get, gateway );
    REQ_GET_GEN( cjson_send, comm_req_get, subnet_mask );
    REQ_GET_GEN( cjson_send, comm_req_get, dns_1 );
    REQ_GET_GEN( cjson_send, comm_req_get, dns_2 );
    REQ_GET_GEN( cjson_send, comm_req_get, DHCP_enable );
#undef REQ_GET_GEN
    cjson_str = cJSON_PrintUnformatted( cjson_send );
    if ( cjson_str == NULL ) {
        ret = FSA::ret_e::INTERFACE_HANDLE_ERR;
        goto end;
    }

    net_ret = fsa_net_send_recv( fsa_dev->net.comm_sock_fd,
                                 fsa_dev->net.net_recv_mode,
                                 ( uint8_t* )cjson_str,
                                 strlen( cjson_str ),
                                 recv_buf,
                                 sizeof( recv_buf ),
                                 timeout_ms,
                                 max_retry );
    if ( net_ret < 0 ) {
        ret = FSA::ret_e::TIMEOUT;
        goto end;
    }

    cjson_recv = cJSON_Parse( ( const char* )recv_buf );
    if ( cjson_recv == NULL ) {
        ret = FSA::ret_e::INTERFACE_HANDLE_ERR;
        goto end;
    }
// 定义宏来简化获取字符串和布尔值的代码
#define RESP_GET_STR( obj, resp, key )                                     \
    do {                                                                   \
        cJSON* temp_cjson = cJSON_GetObjectItem( obj, #key );              \
        if ( temp_cjson != nullptr && temp_cjson->type == cJSON_String ) { \
            resp.key##_valid = true;                                       \
            resp.key         = temp_cjson->valuestring;                    \
        }                                                                  \
        else                                                               \
            resp.key##_valid = false;                                      \
    } while ( 0 )
#define RESP_GET_BOOL( obj, resp, key )                                                                         \
    do {                                                                                                        \
        cJSON* temp_cjson = cJSON_GetObjectItem( obj, #key );                                                   \
        if ( temp_cjson != nullptr && ( temp_cjson->type == cJSON_True || temp_cjson->type == cJSON_False ) ) { \
            resp.key##_valid = true;                                                                            \
            resp.key         = reinterpret_cast< const bool* >( &temp_cjson->valueint );                        \
        }                                                                                                       \
        else                                                                                                    \
            resp.key##_valid = false;                                                                           \
    } while ( 0 )
    RESP_GET_STR( cjson_recv, comm_resp_get, name );
    RESP_GET_STR( cjson_recv, comm_resp_get, type );
    RESP_GET_STR( cjson_recv, comm_resp_get, mcu_fw_version );
    RESP_GET_STR( cjson_recv, comm_resp_get, mac_address );
    RESP_GET_STR( cjson_recv, comm_resp_get, uid );
    RESP_GET_STR( cjson_recv, comm_resp_get, sn );
    RESP_GET_STR( cjson_recv, comm_resp_get, PCBA_sn );
    RESP_GET_STR( cjson_recv, comm_resp_get, gearbox_sn );
    RESP_GET_STR( cjson_recv, comm_resp_get, static_IP );
    RESP_GET_STR( cjson_recv, comm_resp_get, gateway );
    RESP_GET_STR( cjson_recv, comm_resp_get, subnet_mask );
    RESP_GET_STR( cjson_recv, comm_resp_get, dns_1 );
    RESP_GET_STR( cjson_recv, comm_resp_get, dns_2 );
    RESP_GET_BOOL( cjson_recv, comm_resp_get, DHCP_enable );
#undef RESP_GET_STR
#undef RESP_GET_BOOL
    ret = FSA::ret_e::SUCCESS;

end:
    if ( cjson_str != NULL ) {
        cJSON_free( cjson_str );
    }
    if ( cjson_recv != NULL ) {
        cJSON_Delete( cjson_recv );
    }
    if ( cjson_send != NULL ) {
        cJSON_Delete( cjson_send );
    }
    return ret;
}

}  // namespace AC3
