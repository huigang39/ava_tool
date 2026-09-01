/*
 * test_tables.c — dyn200_port_param / dyn200_param / dyn200_rt 的无硬件自检。
 *
 * 两部分：
 *
 *  1. 布局契约。ava_tool 的头文件解析器按“自然对齐、枚举占 4 字节、没有
 *     #pragma pack”计算字段偏移，下面的静态断言把这个约定钉死。字段增删时
 *     这些数字要一起改，改动本身就是在提醒：偏移变了，ava_tool 那边也要
 *     重新解析头文件。
 *  2. 行为契约。在没有传感器的情况下检查错误处理、默认值归一化、一次性门控，
 *     以及"串口表 = 搜索 + 连接"这条链路：串口表读一次就会按选中的口去连接，
 *     连不上时报打开失败，之后 rt 表自动用记住的参数重连（带 2 秒节流）。
 *
 * 所有用例都用不存在的口名（_MISSING），不会真的打开电脑上的串口。
 */
#include "dyn200.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DYN200_STATIC_CHECK(cond) _Static_assert(cond, #cond)

#if defined(_WIN32)
#define DYN200_MISSING_PORT "COM_DYN200_MISSING"
#else
#define DYN200_MISSING_PORT "/dev/dyn200-missing"
#endif

// ── 布局契约 ───────────────────────────────────────────────────────────────

DYN200_STATIC_CHECK(sizeof(dyn200_port_action) == 4);
DYN200_STATIC_CHECK(sizeof(dyn200_protocol) == 4);

DYN200_STATIC_CHECK(sizeof(Dyn200Measurement) == 40);
DYN200_STATIC_CHECK(sizeof(dyn200_command) == 8);
DYN200_STATIC_CHECK(sizeof(dyn200_device_state) == 12);

DYN200_STATIC_CHECK(offsetof(dyn200_port_param, index) == 0);
DYN200_STATIC_CHECK(offsetof(dyn200_port_param, count) == 4);
DYN200_STATIC_CHECK(offsetof(dyn200_port_param, name) == 8);
DYN200_STATIC_CHECK(offsetof(dyn200_port_param, desc) == 72);
DYN200_STATIC_CHECK(offsetof(dyn200_port_param, port) == 200);
DYN200_STATIC_CHECK(offsetof(dyn200_port_param, protocol) == 264);
DYN200_STATIC_CHECK(offsetof(dyn200_port_param, baud_rate) == 268);
DYN200_STATIC_CHECK(offsetof(dyn200_port_param, action) == 284);
DYN200_STATIC_CHECK(offsetof(dyn200_port_param, connected) == 288);
DYN200_STATIC_CHECK(offsetof(dyn200_port_param, active_port) == 292);
DYN200_STATIC_CHECK(offsetof(dyn200_port_param, last_error) == 372);
DYN200_STATIC_CHECK(sizeof(dyn200_port_param) == 436);

DYN200_STATIC_CHECK(offsetof(dyn200_param, scaling) == 0);
DYN200_STATIC_CHECK(offsetof(dyn200_param, filter) == 12);
DYN200_STATIC_CHECK(offsetof(dyn200_param, zero_on_boot) == 16);
DYN200_STATIC_CHECK(offsetof(dyn200_param, direction) == 20);
DYN200_STATIC_CHECK(offsetof(dyn200_param, write_filter) == 24);
DYN200_STATIC_CHECK(offsetof(dyn200_param, write_zero_on_boot) == 28);
DYN200_STATIC_CHECK(offsetof(dyn200_param, write_direction) == 32);
DYN200_STATIC_CHECK(offsetof(dyn200_param, device) == 36);
DYN200_STATIC_CHECK(offsetof(dyn200_param, link.port) == 48);
DYN200_STATIC_CHECK(offsetof(dyn200_param, link.protocol) == 112);
DYN200_STATIC_CHECK(offsetof(dyn200_param, link.baud_rate) == 116);
DYN200_STATIC_CHECK(offsetof(dyn200_param, connected) == 120);
DYN200_STATIC_CHECK(offsetof(dyn200_param, last_error) == 128);
DYN200_STATIC_CHECK(sizeof(dyn200_param) == 192);

DYN200_STATIC_CHECK(offsetof(dyn200_rt, sample_count) == 8);
DYN200_STATIC_CHECK(offsetof(dyn200_rt, wait_fresh) == 4);
DYN200_STATIC_CHECK(offsetof(dyn200_rt, fresh) == 16);
DYN200_STATIC_CHECK(offsetof(dyn200_rt, command) == 88);
DYN200_STATIC_CHECK(offsetof(dyn200_rt, measurement) == 96);
DYN200_STATIC_CHECK(sizeof(dyn200_rt) == 136);

// ── 行为契约 ───────────────────────────────────────────────────────────────

/**
 * @brief 串口表：读一次 = 搜索 + 连接，并把选中的口回填到 port
 */
static void test_port_table(void)
{
        dyn200_port_param ports;
        uint32_t          count;

        dyn200_close(); // 忘掉之前记住的连接

        memset(&ports, 0, sizeof(ports));
        snprintf(ports.port, sizeof(ports.port), "%s", DYN200_MISSING_PORT);
        assert(dyn200_read_port_param(&ports) == DYN200_ERR_OPEN);
        // 0 归一化成默认值，面板里看到的就是实际参数
        assert(ports.protocol == DYN200_PROTOCOL_MODBUS);
        assert(ports.baud_rate == 9600);
        assert(ports.stop_bits == 1);
        assert(ports.timeout_ms == 200);
        assert(ports.device_address == 1);
        assert(ports.action == DYN200_PORT_NONE); // 一次性动作读一次就清掉
        assert(ports.connected == 0);
        assert(ports.active_port[0] == '\0');
        assert(ports.last_error_code == DYN200_ERR_OPEN);
        assert(ports.last_error[0] != '\0');
        // 手填的 port 优先，不会因为 index 被改成别的东西
        assert(strcmp(ports.port, DYN200_MISSING_PORT) == 0);

        count = dyn200_port_count();
        assert(count == ports.count);

        if (count > 0) {
                // index 在范围内时 name/desc 有值；翻过头只是变空，port 保留
                ports.index = 0;
                dyn200_read_port_param(&ports);
                assert(ports.name[0] != '\0');
                ports.index = count;
                dyn200_read_port_param(&ports);
                assert(ports.name[0] == '\0');
                assert(ports.desc[0] == '\0');
                assert(strcmp(ports.port, DYN200_MISSING_PORT) == 0);
        } else {
                assert(ports.name[0] == '\0');
        }

        // port 为空而且 index 越界：没有可连的口，报参数错误
        memset(&ports, 0, sizeof(ports));
        ports.index = count;
        assert(dyn200_read_port_param(&ports) == DYN200_ERR_ARGUMENT);
        assert(ports.connected == 0);

        // RESCAN 只重新枚举，不去连接
        memset(&ports, 0, sizeof(ports));
        ports.index = count;
        ports.action = DYN200_PORT_RESCAN;
        assert(dyn200_read_port_param(&ports) == DYN200_OK);
        assert(ports.action == DYN200_PORT_NONE);
        assert(ports.count == count);

        // DISCONNECT 之后保持断开，普通读取也不会自动重连
        snprintf(ports.port, sizeof(ports.port), "%s", DYN200_MISSING_PORT);
        ports.action = DYN200_PORT_DISCONNECT;
        assert(dyn200_read_port_param(&ports) == DYN200_OK);
        assert(ports.action == DYN200_PORT_NONE);
        assert(ports.connected == 0);
        assert(dyn200_is_open() == 0);
        // 断开之后普通读取不会自动重连（但也不报错）
        assert(dyn200_read_port_param(&ports) == DYN200_OK);
        assert(ports.connected == 0);

        // 显式入口 dyn200_port_apply：CONNECT 会真的去打开这个口
        memset(&ports, 0, sizeof(ports));
        snprintf(ports.port, sizeof(ports.port), "%s", DYN200_MISSING_PORT);
        ports.action = DYN200_PORT_CONNECT;
        assert(dyn200_port_apply(&ports) == DYN200_ERR_OPEN);
        assert(ports.action == DYN200_PORT_NONE);
        assert(ports.connected == 0);
        assert(ports.last_error_code == DYN200_ERR_OPEN);

}

/**
 * @brief 实时表：串口没配过就报 -4，串口表配过之后会自动尝试连接（2 秒节流）
 */
static void test_rt_auto_connect(void)
{
        dyn200_port_param ports;
        dyn200_rt         rt;

        dyn200_close(); // 彻底关闭：忘掉记住的连接

        memset(&rt, 0, sizeof(rt));
        assert(dyn200_read_rt(&rt) == DYN200_ERR_NOT_OPEN);
        assert(strstr(rt.last_error, "dyn200_port_param") != NULL); // 提示先去串口表选口

        // 串口表里填一个不存在的口：读取即搜索+连接，打不开这个口
        memset(&ports, 0, sizeof(ports));
        snprintf(ports.port, sizeof(ports.port), "%s", DYN200_MISSING_PORT);
        ports.baud_rate = 9600;
        assert(dyn200_read_port_param(&ports) == DYN200_ERR_OPEN);
        assert(ports.connected == 0);

        // rt 表会记住这套参数自己重连；刚失败过，节流窗口内先不重复敲
        memset(&rt, 0, sizeof(rt));
        assert(dyn200_read_rt(&rt) == DYN200_ERR_NOT_OPEN);
        assert(rt.last_error_code == DYN200_ERR_NOT_OPEN);

        // 彻底关闭后回到"没配过口"的状态
        dyn200_close();
        memset(&rt, 0, sizeof(rt));
        assert(dyn200_read_rt(&rt) == DYN200_ERR_NOT_OPEN);
        assert(strstr(rt.last_error, "dyn200_port_param") != NULL);
}

/**
 * @brief 参数表：只回填反馈；门控置位但没有串口时拒绝下发，且门控是一次性的
 */
static void test_param_table(void)
{
        dyn200_param param;

        dyn200_close();
        memset(&param, 0, sizeof(param));

        assert(dyn200_read_param(&param) == DYN200_OK);
        assert(param.scaling.torque_decimals == 1);
        assert(param.scaling.speed_decimals == 0);
        assert(param.scaling.power_decimals == 0);
        assert(param.connected == 0);
        assert(param.last_error_code == DYN200_OK);

        // 没有门控的写入是空操作
        assert(dyn200_write_param(&param) == DYN200_OK);

        // 越界值在连接之前就会被拦下
        param.write_filter = 1;
        param.filter.val = 200;
        assert(dyn200_write_param(&param) == DYN200_ERR_ARGUMENT);
        assert(param.write_filter == 0);
        assert(param.filter.val == 200);

        // 合法值 + 没有串口：报没打开，门控同样清掉
        param.write_filter = 1;
        param.filter.val = 50;
        assert(dyn200_write_param(&param) == DYN200_ERR_NOT_OPEN);
        assert(param.write_filter == 0);
        assert(param.last_error_code == DYN200_ERR_NOT_OPEN);
}

/**
 * @brief 实时表：一次性命令在串口没开时失败，门控和命令都会被清掉
 */
static void test_rt_table(void)
{
        dyn200_rt rt;

        dyn200_close();
        memset(&rt, 0, sizeof(rt));

        assert(dyn200_read_rt(&rt) == DYN200_ERR_NOT_OPEN);
        assert(rt.sample_count == 0);
        assert(rt.last_error_code == DYN200_ERR_NOT_OPEN);
        assert(rt.last_error[0] != '\0');

        assert(dyn200_write_rt(&rt) == DYN200_OK); // 没有门控 = 空操作

        rt.write_command = 1;
        rt.command.zero = 1;
        assert(dyn200_write_rt(&rt) == DYN200_ERR_NOT_OPEN);
        assert(rt.write_command == 0);
        assert(rt.command.zero == 0);
        assert(rt.command.factory_reset == 0);
}

static void test_flat_api(void)
{
        char name[64];
        int  baud = -1;
        int  protocol = -1;

        dyn200_close();
        assert(dyn200_connection(name, sizeof(name), &baud, &protocol) == DYN200_ERR_NOT_OPEN);
        assert(strcmp(name, "") == 0);
        assert(baud == 9600);
        assert(protocol == DYN200_PROTOCOL_MODBUS);

        assert(dyn200_set_decimals(0, 0, 0) == DYN200_OK);
        assert(dyn200_set_decimals(7, 0, 0) == DYN200_ERR_ARGUMENT);
        assert(dyn200_set_decimals(2, 0, 0) == DYN200_OK);
}

int main(void)
{
        test_port_table();
        test_rt_auto_connect();
        test_param_table();
        test_rt_table();
        test_flat_api();

        puts("table tests passed");
        return 0;
}
