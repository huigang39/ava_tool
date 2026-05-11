#include <stdio.h>
#include <string.h>

#include "jlinkport.h"

#ifdef _WIN32
#include <windows.h>

// J-Link DLL 原始函数签名
typedef const char *(__cdecl *jlinkarm_open_fn)(void);
typedef void(__cdecl *jlinkarm_close_fn)(void);
typedef char(__cdecl *jlinkarm_isopen_fn)(void);
typedef i32(__cdecl *jlinkarm_connect_fn)(void);
typedef i32(__cdecl *jlinkarm_writememex_fn)(u32 Addr, u32 NumBytes, const void *p, u32 Flags);
typedef i32(__cdecl *jlinkarm_readmemex_fn)(u32 Addr, u32 NumBytes, void *p, u32 Flags);
typedef i32(__cdecl *jlinkarm_emu_select_by_usb_sn_fn)(u32 SerialNo);
typedef i32(__cdecl *jlinkarm_execcommand_fn)(const char *pIn, char *pOut, i32 BufferSize);
typedef i32(__cdecl *jlinkarm_tif_select_fn)(i32 interface_id);
typedef void(__cdecl *jlinkarm_setspeed_fn)(u32 Speed);
typedef void(__cdecl *jlinkarm_reset_fn)(void);

struct jlink_api_min {
        HMODULE                          dll;
        jlinkarm_open_fn                 Open;
        jlinkarm_close_fn                Close;
        jlinkarm_isopen_fn               IsOpen;
        jlinkarm_connect_fn              Connect;
        jlinkarm_writememex_fn           WriteMemEx;
        jlinkarm_readmemex_fn            ReadMemEx;
        jlinkarm_emu_select_by_usb_sn_fn EMU_SelectByUSBSN;
        jlinkarm_execcommand_fn          ExecCommand;
        jlinkarm_tif_select_fn           TIF_Select;
        jlinkarm_setspeed_fn             SetSpeed;
        jlinkarm_reset_fn                Reset;
};

static struct jlink_api_min s_api;
static bool                 s_api_loaded = false;

static i32
load_symbol(HMODULE dll, FARPROC *out, const char *name)
{
        if (out == NULL)
                return -1;
        *out = GetProcAddress(dll, name);
        return (*out != NULL) ? 0 : -1;
}

static i32
jlink_dll_load(const char *dll_path)
{
        static const char *candidates[] = {
            "JLink_x64.dll",
            "JLinkARM.dll",
            "JLink.dll",
        };

        HMODULE dll = NULL;

        if (s_api_loaded)
                return 0;

        if (dll_path != NULL && dll_path[0] != '\0') {
                dll = LoadLibraryA(dll_path);
        } else {
                for (usize i = 0; i < (sizeof(candidates) / sizeof(candidates[0])); ++i) {
                        dll = LoadLibraryA(candidates[i]);
                        if (dll != NULL)
                                break;
                }
        }

        if (dll == NULL)
                return -1;

        memset(&s_api, 0, sizeof(s_api));
        s_api.dll = dll;

        if (load_symbol(dll, (FARPROC *)&s_api.Open, "JLINKARM_Open") < 0 ||
            load_symbol(dll, (FARPROC *)&s_api.Close, "JLINKARM_Close") < 0 ||
            load_symbol(dll, (FARPROC *)&s_api.IsOpen, "JLINKARM_IsOpen") < 0 ||
            load_symbol(dll, (FARPROC *)&s_api.Connect, "JLINKARM_Connect") < 0 ||
            load_symbol(dll, (FARPROC *)&s_api.WriteMemEx, "JLINKARM_WriteMemEx") < 0 ||
            load_symbol(dll, (FARPROC *)&s_api.ReadMemEx, "JLINKARM_ReadMemEx") < 0 ||
            load_symbol(dll, (FARPROC *)&s_api.EMU_SelectByUSBSN, "JLINKARM_EMU_SelectByUSBSN") < 0 ||
            load_symbol(dll, (FARPROC *)&s_api.ExecCommand, "JLINKARM_ExecCommand") < 0 ||
            load_symbol(dll, (FARPROC *)&s_api.TIF_Select, "JLINKARM_TIF_Select") < 0 ||
            load_symbol(dll, (FARPROC *)&s_api.SetSpeed, "JLINKARM_SetSpeed") < 0 ||
            load_symbol(dll, (FARPROC *)&s_api.Reset, "JLINKARM_Reset") < 0) {
                FreeLibrary(dll);
                memset(&s_api, 0, sizeof(s_api));
                return -1;
        }

        s_api_loaded = true;
        return 0;
}

static i32
jlink_exec(const char *cmd)
{
        char out[1024];
        if (!s_api_loaded || s_api.ExecCommand == NULL)
                return -1;
        out[0] = '\0';
        return (s_api.ExecCommand(cmd, out, (i32)sizeof(out)) >= 0) ? 0 : -1;
}

i32
jlink_port_init(const char *dll_path, const char *device, u32 speed_khz, u32 serial_no, bool use_sn)
{
        char cmd[256];

        if (device == NULL || device[0] == '\0')
                return -1;
        if (jlink_dll_load(dll_path) < 0)
                return -1;

        (void)jlink_exec("SuppressGUI 1");
        (void)jlink_exec("SuppressInfoUpdateFW");
        (void)jlink_exec("HideDeviceSelection 1");

        s_api.Open();
        if (s_api.IsOpen() == 0)
                goto init_err;

        if (use_sn && s_api.EMU_SelectByUSBSN(serial_no) < 0)
                goto init_err;

        snprintf(cmd, sizeof(cmd), "device = %s", device);
        if (jlink_exec(cmd) < 0)
                goto init_err;

        if (s_api.TIF_Select(1 /* JLINKARM_TIF_SWD */) < 0)
                goto init_err;

        s_api.SetSpeed(speed_khz == 0u ? 4000u : speed_khz);

        if (s_api.Connect() < 0)
                goto init_err;

        return 0;

init_err:
        jlink_port_deinit();
        return -1;
}

void
jlink_port_deinit(void)
{
        if (!s_api_loaded)
                return;

        if (s_api.IsOpen != NULL && s_api.IsOpen() != 0) {
                s_api.Close();
        }
        if (s_api.dll != NULL) {
                FreeLibrary(s_api.dll);
        }
        memset(&s_api, 0, sizeof(s_api));
        s_api_loaded = false;
}

i32
jlink_port_reset(void)
{
        if (!s_api_loaded || s_api.Reset == NULL)
                return -1;

        s_api.Reset();
        return 0;
}

i32
jlink_port_write_mem(u32 addr, u32 len, const void *data)
{
        if (!s_api_loaded || s_api.WriteMemEx == NULL)
                return -1;
        i32 ret = s_api.WriteMemEx(addr, len, data, 0u);
        return (ret >= 0) ? (i32)ret : -1;
}

i32
jlink_port_read_mem(u32 addr, u32 len, void *data)
{
        if (!s_api_loaded || s_api.ReadMemEx == NULL)
                return -1;
        i32 ret = s_api.ReadMemEx(addr, len, data, 0u);
        return (ret >= 0) ? (i32)ret : -1;
}

#else
/* Linux 或其他平台空实现占位，避免编译报错 */
i32
jlink_port_init(const char *dll_path, const char *device, u32 speed_khz, u32 serial_no, bool use_sn)
{
        return -1;
}

void
jlink_port_deinit(void)
{
}

i32
jlink_port_reset(void)
{
        return -1;
}

i32
jlink_port_write_mem(u32 addr, u32 len, const void *data)
{
        return -1;
}

i32
jlink_port_read_mem(u32 addr, u32 len, void *data)
{
        return -1;
}

#endif
