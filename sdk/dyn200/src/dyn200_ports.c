/*
 * dyn200_ports.c — 串口枚举。
 *
 * Windows 走设备管理器的 Ports 设备类（SetupAPI），这样能同时拿到 "COM7"
 * 和 "USB-SERIAL CH340 (COM7)" 这样的描述，用户不用自己去设备管理器里对照；
 * Unix 直接扫描 /dev 下的常见串口设备名。
 *
 * SetupAPI 是运行时按需加载的（LoadLibrary + GetProcAddress），因此本 SDK
 * 不增加任何链接依赖：只调用 Win32 基础 API，静态库使用者也不用加 setupapi.lib。
 */
#include "dyn200_ports.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <setupapi.h> // 只取类型与常量，函数一律动态解析

typedef HDEVINFO(WINAPI *setup_get_class_devs_fn)(const GUID *, PCSTR, HWND, DWORD);
typedef BOOL(WINAPI *setup_enum_device_info_fn)(HDEVINFO, DWORD, PSP_DEVINFO_DATA);
typedef HKEY(WINAPI *setup_open_dev_reg_key_fn)(HDEVINFO, PSP_DEVINFO_DATA, DWORD, DWORD, DWORD, REGSAM);
typedef BOOL(WINAPI *setup_get_dev_reg_property_fn)(HDEVINFO, PSP_DEVINFO_DATA, DWORD, PDWORD, PBYTE, DWORD, PDWORD);
typedef BOOL(WINAPI *setup_destroy_device_info_list_fn)(HDEVINFO);

struct setup_api {
        HMODULE                            module;
        setup_get_class_devs_fn            get_class_devs;
        setup_enum_device_info_fn          enum_device_info;
        setup_open_dev_reg_key_fn          open_dev_reg_key;
        setup_get_dev_reg_property_fn      get_dev_reg_property;
        setup_destroy_device_info_list_fn  destroy_device_info_list;
};

static int setup_api_load(struct setup_api *api)
{
        memset(api, 0, sizeof(*api));
        api->module = LoadLibraryA("setupapi.dll");
        if (!api->module)
                return 0;
        api->get_class_devs = (setup_get_class_devs_fn)(void *)GetProcAddress(api->module, "SetupDiGetClassDevsA");
        api->enum_device_info = (setup_enum_device_info_fn)(void *)GetProcAddress(api->module, "SetupDiEnumDeviceInfo");
        api->open_dev_reg_key = (setup_open_dev_reg_key_fn)(void *)GetProcAddress(api->module, "SetupDiOpenDevRegKey");
        api->get_dev_reg_property =
            (setup_get_dev_reg_property_fn)(void *)GetProcAddress(api->module, "SetupDiGetDeviceRegistryPropertyA");
        api->destroy_device_info_list =
            (setup_destroy_device_info_list_fn)(void *)GetProcAddress(api->module, "SetupDiDestroyDeviceInfoList");
        if (!api->get_class_devs || !api->enum_device_info || !api->open_dev_reg_key || !api->get_dev_reg_property ||
            !api->destroy_device_info_list) {
                FreeLibrary(api->module);
                memset(api, 0, sizeof(*api));
                return 0;
        }
        return 1;
}

uint32_t dyn200_ports_enumerate(char (*names)[DYN200_PORT_NAME_MAX],
                                char (*descs)[DYN200_PORT_DESC_MAX],
                                uint32_t capacity)
{
        // GUID_DEVCLASS_PORTS：串口和并口都在这一类里
        static const GUID ports_class = {0x4d36e978, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
        struct setup_api  api;
        HDEVINFO          set;
        uint32_t          count = 0;
        DWORD             index = 0;

        if (!names || !descs || capacity == 0)
                return 0;
        if (!setup_api_load(&api))
                return 0;

        set = api.get_class_devs(&ports_class, NULL, NULL, DIGCF_PRESENT);
        if (set == INVALID_HANDLE_VALUE) {
                FreeLibrary(api.module);
                return 0;
        }

        while (count < capacity) {
                SP_DEVINFO_DATA info;
                char            port[128];
                char            desc[DYN200_PORT_DESC_MAX];
                DWORD           size;
                DWORD           type;
                HKEY            key;

                memset(&info, 0, sizeof(info));
                info.cbSize = sizeof(info);
                if (!api.enum_device_info(set, index++, &info))
                        break;

                // 端口名只在设备注册表键的 "PortName" 里，例如 "COM7"
                port[0] = '\0';
                key     = api.open_dev_reg_key(set, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
                if (key != INVALID_HANDLE_VALUE) {
                        size = sizeof(port) - 1;
                        type = 0;
                        if (RegQueryValueExA(key, "PortName", NULL, &type, (LPBYTE)port, &size) != ERROR_SUCCESS)
                                port[0] = '\0';
                        RegCloseKey(key);
                }
                // 这一类里还有 LPT 口，本 SDK 只关心 COM
                if (_strnicmp(port, "COM", 3) != 0)
                        continue;

                desc[0] = '\0';
                type    = 0;
                if (!api.get_dev_reg_property(set, &info, SPDRP_FRIENDLYNAME, &type, (PBYTE)desc, sizeof(desc) - 1, NULL))
                        api.get_dev_reg_property(set, &info, SPDRP_DEVICEDESC, &type, (PBYTE)desc, sizeof(desc) - 1, NULL);

                snprintf(names[count], DYN200_PORT_NAME_MAX, "%s", port);
                snprintf(descs[count], DYN200_PORT_DESC_MAX, "%s", desc);
                ++count;
        }
        api.destroy_device_info_list(set);
        FreeLibrary(api.module);
        return count;
}

#else // Unix

#include <dirent.h>
#include <sys/stat.h>

static const char *const k_prefixes[] = {
    "ttyUSB", "ttyACM", "ttyS", "tty.usb", "cu.usb", "serial",
};

static int matches_serial_prefix(const char *name)
{
        size_t i;
        for (i = 0; i < sizeof(k_prefixes) / sizeof(k_prefixes[0]); ++i) {
                if (strncmp(name, k_prefixes[i], strlen(k_prefixes[i])) == 0)
                        return 1;
        }
        return 0;
}

uint32_t dyn200_ports_enumerate(char (*names)[DYN200_PORT_NAME_MAX],
                                char (*descs)[DYN200_PORT_DESC_MAX],
                                uint32_t capacity)
{
        DIR           *dir;
        struct dirent *entry;
        uint32_t       count = 0;
        uint32_t       i;

        if (!names || !descs || capacity == 0)
                return 0;
        dir = opendir("/dev");
        if (!dir)
                return 0;

        while (count < capacity && (entry = readdir(dir)) != NULL) {
                char        path[256];
                struct stat status;
                if (!matches_serial_prefix(entry->d_name))
                        continue;
                snprintf(path, sizeof(path), "/dev/%s", entry->d_name);
                if (stat(path, &status) != 0 || !S_ISCHR(status.st_mode))
                        continue;
                snprintf(names[count], DYN200_PORT_NAME_MAX, "%s", path);
                snprintf(descs[count], DYN200_PORT_DESC_MAX, "%s", path);
                ++count;
        }
        closedir(dir);

        // readdir 的顺序不保证稳定，排一下序，让 index 在多次读取间对应同一个口
        for (i = 1; i < count; ++i) {
                char     name[DYN200_PORT_NAME_MAX];
                char     desc[DYN200_PORT_DESC_MAX];
                uint32_t j = i;
                memcpy(name, names[i], sizeof(name));
                memcpy(desc, descs[i], sizeof(desc));
                while (j > 0 && strcmp(names[j - 1], name) > 0) {
                        memcpy(names[j], names[j - 1], sizeof(name));
                        memcpy(descs[j], descs[j - 1], sizeof(desc));
                        --j;
                }
                memcpy(names[j], name, sizeof(name));
                memcpy(descs[j], desc, sizeof(desc));
        }
        return count;
}

#endif
