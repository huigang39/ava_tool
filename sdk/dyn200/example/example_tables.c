/*
 * example_tables.c — 用 ava_tool 数据表接口读 DYN-200。
 *
 * 这里做的事和 SDK 面板里点鼠标完全一样：
 *   1. 串口表（dyn200_port_param）：监听/搜索串口 + 选口 + 配波特率。
 *      面板里这张表是只读表，读一次就完成"搜索 + 连接"；这里用显式入口
 *      dyn200_port_apply() 做同一件事
 *   2. 参数表（dyn200_param）：可选，下发滤波等传感器参数
 *   3. 实时表（dyn200_rt）：循环读扭矩/转速/功率
 *
 * 接传感器之前先用它验证串口参数最省事。
 */
#include "dyn200.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void port_selector(const char *wanted, dyn200_port_param *ports)
{
        uint32_t count = dyn200_port_count();
        uint32_t index;

        for (index = 0; index < count; ++index) {
                char name[64], desc[128];
                if (dyn200_port_name(index, name, sizeof(name), desc, sizeof(desc)) != DYN200_OK)
                        continue;
                printf("port[%u] %s  %s\n", index, name, desc);
                if (strcmp(name, wanted) == 0)
                        ports->index = index;
        }
        if (index == 0)
                printf("no serial port found by the SDK; continuing with the given name\n");
        // 手填的口名优先于 index，跟面板里在 port 里输入一样
        snprintf(ports->port, sizeof(ports->port), "%s", wanted);
}

int main(int argc, char **argv)
{
        dyn200_port_param ports;
        dyn200_param      param;
        dyn200_rt         rt;
        int               rc;

        if (argc < 2) {
                fprintf(stderr, "usage: %s <serial-port> [baud] [address] [filter 1..100]\n", argv[0]);
                return 2;
        }

        // 1. 串口表：选口 + 配参数 + 自动打开
        memset(&ports, 0, sizeof(ports));
        port_selector(argv[1], &ports);
        ports.baud_rate = argc > 2 ? (uint32_t)atoi(argv[2]) : 9600;
        ports.device_address = argc > 3 ? (uint32_t)atoi(argv[3]) : 1;
        ports.action = DYN200_PORT_CONNECT;
        if ((rc = dyn200_port_apply(&ports)) != DYN200_OK) {
                fprintf(stderr, "connect %s: %s\n", ports.port, ports.last_error);
                return 1;
        }
        printf("connected %s, %u bps, address %u\n",
               ports.active_port,
               ports.active_baud_rate,
               ports.active_device_address);

        // 2. 参数表：给滤波器设一个值（0 表示不改）
        memset(&param, 0, sizeof(param));
        dyn200_read_param(&param);
        printf("device now: filter=%d zero_on_boot=%d reversed=%d\n",
               param.device.filter,
               param.device.zero_on_boot,
               param.device.reversed);
        if (argc > 4) {
                param.filter.val = atoi(argv[4]);
                param.write_filter = 1; // 面板里编辑 filter.* 会自动置位
                if ((rc = dyn200_write_param(&param)) != DYN200_OK) {
                        fprintf(stderr, "write filter: %s\n", param.last_error);
                        dyn200_close();
                        return 1;
                }
                printf("filter set to %d\n", param.device.filter);
        }

        // 3. 实时表：循环读
        printf("Reading through dyn200_rt; press Ctrl+C to stop.\n");
        memset(&rt, 0, sizeof(rt));
        for (;;) {
                if ((rc = dyn200_read_rt(&rt)) != DYN200_OK) {
                        fprintf(stderr, "read: %s\n", rt.last_error);
                        dyn200_close();
                        return 1;
                }
                printf("torque=%.2f Nm, speed=%.0f rpm, power=%.3f kW (samples=%llu)\n",
                       rt.measurement.torque_nm,
                       rt.measurement.speed_rpm,
                       rt.measurement.power_kw,
                       (unsigned long long)rt.sample_count);
                fflush(stdout);
        }
}
