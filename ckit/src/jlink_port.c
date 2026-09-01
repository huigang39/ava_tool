#include <stdio.h>
#include <string.h>

#include "jlink_port.h"
#include "macrodef.h"
#include "platdef.h"

#if OS(WIN)
#include <windows.h>
typedef HMODULE jlink_lib_t;
typedef FARPROC jlink_sym_t;
#define JLINK_LIB_NULL NULL
#define JLINK_LOAD(p) LoadLibraryA(p)
#define JLINK_SYM(h, n) (jlink_sym_t) GetProcAddress((h), (n))
#define JLINK_FREE(h) FreeLibrary(h)
#elif OS(POSIX)
#include <dlfcn.h>
typedef void *jlink_lib_t;
typedef void *jlink_sym_t;
#define JLINK_LIB_NULL NULL
#define JLINK_LOAD(p) dlopen((p), RTLD_LAZY | RTLD_LOCAL)
#define JLINK_SYM(h, n) dlsym((h), (n))
#define JLINK_FREE(h) dlclose(h)
#endif

#if OS(WIN) || OS(POSIX)

#if OS(WIN)
#define JLINK_CC __cdecl
#else
#define JLINK_CC
#endif

typedef const char *(JLINK_CC *jlinkarm_open_fn)(void);
typedef void(JLINK_CC *jlinkarm_close_fn)(void);
typedef char(JLINK_CC *jlinkarm_isopen_fn)(void);
typedef int32_t(JLINK_CC *jlinkarm_connect_fn)(void);
typedef int32_t(JLINK_CC *jlinkarm_writememex_fn)(uint32_t Addr,
                                                  uint32_t NumBytes,
                                                  const void *p,
                                                  uint32_t Flags);
typedef int32_t(JLINK_CC *jlinkarm_readmemex_fn)(uint32_t Addr,
                                                 uint32_t NumBytes,
                                                 void *p,
                                                 uint32_t Flags);
typedef int32_t(JLINK_CC *jlinkarm_emu_select_by_usb_sn_fn)(uint32_t SerialNo);
typedef int32_t(JLINK_CC *jlinkarm_execcommand_fn)(const char *pIn,
                                                   char *pOut,
                                                   int32_t BufferSize);
typedef int32_t(JLINK_CC *jlinkarm_tif_select_fn)(int32_t interface_id);
typedef void(JLINK_CC *jlinkarm_setspeed_fn)(uint32_t Speed);
typedef void(JLINK_CC *jlinkarm_reset_fn)(void);

struct jlink_api_min {
    jlink_lib_t dll;
    jlinkarm_open_fn Open;
    jlinkarm_close_fn Close;
    jlinkarm_isopen_fn IsOpen;
    jlinkarm_connect_fn Connect;
    jlinkarm_writememex_fn WriteMemEx;
    jlinkarm_readmemex_fn ReadMemEx;
    jlinkarm_emu_select_by_usb_sn_fn EMU_SelectByUSBSN;
    jlinkarm_execcommand_fn ExecCommand;
    jlinkarm_tif_select_fn TIF_Select;
    jlinkarm_setspeed_fn SetSpeed;
    jlinkarm_reset_fn Reset;
};

static struct jlink_api_min s_api;
static bool s_api_loaded = false;

/**
 * @brief              加载动态库函数地址
 * @param[in]     dll  动态库句柄
 * @param[in,out] out  输出数据地址
 * @param[in]     name 符号名称
 */
static int load_symbol(jlink_lib_t dll, jlink_sym_t *out, const char *name);

static int load_symbol(jlink_lib_t dll, jlink_sym_t *out, const char *name)
{
    if (out == NULL || name == NULL) {
        return MERR(MENULLPTR);
    }
    *out = JLINK_SYM(dll, name);
    return (*out != NULL) ? MOK : MERR(MENOTFOUND);
}

/**
 * @brief              加载 J-Link 动态库
 * @param[in] dll_path 动态库文件路径
 */
static int jlink_dll_load(const char *dll_path);

static int jlink_dll_load(const char *dll_path)
{
    static const char *candidates[] = {
#if OS(WIN)
        "JLink_x64.dll",
        "JLinkARM.dll",
        "JLink.dll",
        "lib/jlink/JLink_x64.dll",
        "ckit/lib/jlink/JLink_x64.dll",
#elif OS(MAC)
        "libjlinkarm.dylib",
        "lib/jlink/libjlinkarm.dylib",
        "ckit/lib/jlink/libjlinkarm.dylib",
        "/Applications/SEGGER/JLink/libjlinkarm.dylib",
#elif OS(LINUX)
        "libjlinkarm.so",
        "libjlinkarm.so.8",
        "lib/jlink/libjlinkarm.so.8",
        "lib/jlink/libjlinkarm.so",
        "ckit/lib/jlink/libjlinkarm.so.8",
        "ckit/lib/jlink/libjlinkarm.so",
        "/opt/SEGGER/JLink/libjlinkarm.so",
        "/opt/SEGGER/JLink/libjlinkarm.so.8",
#endif
    };

    jlink_lib_t dll = JLINK_LIB_NULL;

    if (s_api_loaded) {
        return 0;
    }

    if (dll_path != NULL && dll_path[0] != '\0') {
        dll = JLINK_LOAD(dll_path);
    } else {
        for (size_t i = 0; i < (sizeof(candidates) / sizeof(candidates[0]));
             ++i) {
            dll = JLINK_LOAD(candidates[i]);
            if (dll != JLINK_LIB_NULL) {
                break;
            }
        }
    }

    if (dll == JLINK_LIB_NULL) {
        return MERR(MENOTFOUND);
    }

    memset(&s_api, 0, sizeof(s_api));
    s_api.dll = dll;

    if (load_symbol(dll, (jlink_sym_t *)&s_api.Open, "JLINKARM_Open") < 0 ||
        load_symbol(dll, (jlink_sym_t *)&s_api.Close, "JLINKARM_Close") < 0 ||
        load_symbol(dll, (jlink_sym_t *)&s_api.IsOpen, "JLINKARM_IsOpen") < 0 ||
        load_symbol(dll, (jlink_sym_t *)&s_api.Connect, "JLINKARM_Connect") <
            0 ||
        load_symbol(
            dll, (jlink_sym_t *)&s_api.WriteMemEx, "JLINKARM_WriteMemEx") < 0 ||
        load_symbol(
            dll, (jlink_sym_t *)&s_api.ReadMemEx, "JLINKARM_ReadMemEx") < 0 ||
        load_symbol(dll,
                    (jlink_sym_t *)&s_api.EMU_SelectByUSBSN,
                    "JLINKARM_EMU_SelectByUSBSN") < 0 ||
        load_symbol(dll,
                    (jlink_sym_t *)&s_api.ExecCommand,
                    "JLINKARM_ExecCommand") < 0 ||
        load_symbol(
            dll, (jlink_sym_t *)&s_api.TIF_Select, "JLINKARM_TIF_Select") < 0 ||
        load_symbol(dll, (jlink_sym_t *)&s_api.SetSpeed, "JLINKARM_SetSpeed") <
            0 ||
        load_symbol(dll, (jlink_sym_t *)&s_api.Reset, "JLINKARM_Reset") < 0) {
        JLINK_FREE(dll);
        memset(&s_api, 0, sizeof(s_api));
        return MERR(MENOTFOUND);
    }

    s_api_loaded = true;
    return 0;
}

/**
 * @brief         执行 J-Link 命令
 * @param[in] cmd 命令字符串
 */
static int jlink_exec(const char *cmd);

static int jlink_exec(const char *cmd)
{
    char out[1024];
    if (!s_api_loaded || s_api.ExecCommand == NULL) {
        return MERR(MESTATE);
    }
    out[0] = '\0';
    return (s_api.ExecCommand(cmd, out, (int32_t)sizeof(out)) >= 0)
               ? MOK
               : MERR(MEIO);
}

int jlink_port_init(const char *dll_path,
                    const char *dev,
                    uint32_t vel_khz,
                    uint32_t serial_no,
                    bool use_sn)
{
    char cmd[256];

    if (dev == NULL) {
        return MERR(MENULLPTR);
    }
    if (dev[0] == '\0') {
        return MERR(MEINVAL);
    }
    const int load_ret = jlink_dll_load(dll_path);
    if (load_ret < 0) {
        return load_ret;
    }

    (void)jlink_exec("SuppressGUI 1");
    (void)jlink_exec("SuppressInfoUpdateFW");
    (void)jlink_exec("HideDeviceSelection 1");

    s_api.Open();
    if (s_api.IsOpen() == 0) {
        goto init_err;
    }

    if (use_sn && s_api.EMU_SelectByUSBSN(serial_no) < 0) {
        goto init_err;
    }

    snprintf(cmd, sizeof(cmd), "device = %s", dev);
    if (jlink_exec(cmd) < 0) {
        goto init_err;
    }

    if (s_api.TIF_Select(1) < 0) {
        goto init_err;
    }

    s_api.SetSpeed(vel_khz == 0U ? 4000U : vel_khz);

    if (s_api.Connect() < 0) {
        goto init_err;
    }

    return 0;

init_err:
    jlink_port_deinit();
    return MERR(MECREATE);
}

int jlink_port_deinit(void)
{
    if (!s_api_loaded) {
        return MOK;
    }

    if (s_api.IsOpen != NULL && s_api.IsOpen() != 0) {
        s_api.Close();
    }
    int ret = MOK;
    if (s_api.dll != JLINK_LIB_NULL) {
#if OS(WIN)
        if (JLINK_FREE(s_api.dll) == 0)
#else
        if (JLINK_FREE(s_api.dll) != 0)
#endif
            ret = MERR(MEIO);
    }
    memset(&s_api, 0, sizeof(s_api));
    s_api_loaded = false;
    return ret;
}

int jlink_port_reset(void)
{
    if (!s_api_loaded || s_api.Reset == NULL) {
        return MERR(MESTATE);
    }

    s_api.Reset();
    return 0;
}

int jlink_port_write_mem(uint32_t addr, uint32_t len, const void *data)
{
    if (!data) {
        return MERR(MENULLPTR);
    }
    if (!s_api_loaded || s_api.WriteMemEx == NULL) {
        return MERR(MESTATE);
    }
    int32_t ret = s_api.WriteMemEx(addr, len, data, 0U);
    return (ret >= 0) ? (int32_t)ret : MERR(MEIO);
}

int jlink_port_read_mem(uint32_t addr, uint32_t len, void *data)
{
    if (!data) {
        return MERR(MENULLPTR);
    }
    if (!s_api_loaded || s_api.ReadMemEx == NULL) {
        return MERR(MESTATE);
    }
    int32_t ret = s_api.ReadMemEx(addr, len, data, 0U);
    return (ret >= 0) ? (int32_t)ret : MERR(MEIO);
}

#else
/**
 * @brief MCU 等无法加载动态库的平台空实现占位
 */
int jlink_port_init(const char *dll_path,
                    const char *dev,
                    uint32_t vel_khz,
                    uint32_t serial_no,
                    bool use_sn)
{
    ARG_UNUSED(dll_path);
    ARG_UNUSED(dev);
    ARG_UNUSED(vel_khz);
    ARG_UNUSED(serial_no);
    ARG_UNUSED(use_sn);
    return MERR(MENOTSUP);
}

int jlink_port_deinit(void)
{
    return MERR(MENOTSUP);
}

int jlink_port_reset(void)
{
    return MERR(MENOTSUP);
}

int jlink_port_write_mem(uint32_t addr, uint32_t len, const void *data)
{
    ARG_UNUSED(addr);
    ARG_UNUSED(len);
    ARG_UNUSED(data);
    return data ? MERR(MENOTSUP) : MERR(MENULLPTR);
}

int jlink_port_read_mem(uint32_t addr, uint32_t len, void *data)
{
    ARG_UNUSED(addr);
    ARG_UNUSED(len);
    ARG_UNUSED(data);
    return data ? MERR(MENOTSUP) : MERR(MENULLPTR);
}

#endif
