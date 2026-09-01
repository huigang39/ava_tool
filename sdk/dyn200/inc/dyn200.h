/*
 * dyn200.h — DYN-200 动态扭矩传感器 SDK（RS-485 / Modbus RTU）。
 *
 * 这个头文件同时提供两套接口：
 *
 *  1. 扁平 C API（dyn200_open / dyn200_read / ...）：给普通 C/C++/Python
 *     调用方，也可以把 dyn200_read_torque_nm 之类的标量接口直接绑定到
 *     ava_tool 的 LOCAL 变量读取函数上。
 *  2. ava_tool SDK 插件数据表：dyn200_port_param / dyn200_param / dyn200_rt。
 *     ava_tool 的 SDK 面板按命名约定自动生成可编辑表格——以 "param"/"rt"
 *     结尾的结构体，加上名称同样以 "param"/"rt" 结尾的 read_/write_ 函数，
 *     就自动成为一张表：
 *
 *       dyn200_port_param  串口表：枚举电脑上的串口，选端口 + 配串口参数，
 *                          action = CONNECT 时自动调用 dyn200_open_ex() 打开
 *       dyn200_param       参数表：传感器自身参数（小数位、滤波、上电清零、
 *                          方向），编辑哪一项就只下发哪一项
 *       dyn200_rt          实时表：扭矩/转速/功率实时值 + 一次性命令
 *
 *  结构体全部自然对齐（没有 #pragma pack、位域、半精度或指针字段），
 *  ava_tool 的头文件解析器可以直接算出每个字段的偏移。
 *
 *  带点的字段名（"filter."、"device."、"command." 等）在面板里显示为可折叠
 *  的子分组；编辑分组里的任一字段会自动置位同名的 write_<组名> 门控字段，
 *  只有门控被置位时 write_* 才会下发这一组，写完立即清零。因此新建一张表、
 *  或绑定自动同步，都不会重放已确认的一次性动作。
 *
 *  表或字段拖入变量管理器/监视器后自动持续读取，绑定值修改后自动下发。
 *  表格不再需要字段更新勾选框或自动读取/自动写入开关；数值变化时仍会高亮。
 */
#ifndef DYN200_H
#define DYN200_H

#include <stddef.h>
#include <stdint.h>

#if defined(DYN200_STATIC)
#define DYN200_API
#elif defined(_WIN32)
#if defined(DYN200_BUILD)
#define DYN200_API __declspec(dllexport)
#else
#define DYN200_API __declspec(dllimport)
#endif
#else
#define DYN200_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// 返回值; 0 为成功，其余为负
enum Dyn200Error {
        DYN200_OK = 0,
        DYN200_ERR_ARGUMENT = -1,
        DYN200_ERR_OPEN = -2,
        DYN200_ERR_CONFIGURE = -3,
        DYN200_ERR_NOT_OPEN = -4,
        DYN200_ERR_WRITE = -5,
        DYN200_ERR_TIMEOUT = -6,
        DYN200_ERR_READ = -7,
        DYN200_ERR_CRC = -8,
        DYN200_ERR_PROTOCOL = -9,
        DYN200_ERR_MODBUS = -10
};

// Byte addresses from the DYN-200 manual. Each value occupies two registers
enum Dyn200Register {
        DYN200_REG_TORQUE = 0x0000,
        DYN200_REG_SPEED = 0x0002,
        DYN200_REG_POWER = 0x0004,
        DYN200_REG_FILTER = 0x0006,
        DYN200_REG_TORQUE_DECIMALS = 0x0008,
        DYN200_REG_ZERO_ON_BOOT = 0x000a,
        DYN200_REG_TRANSMIT_ZERO = 0x000c,
        DYN200_REG_TRANSMIT_FULL = 0x000e,
        DYN200_REG_RANGE = 0x0010,
        DYN200_REG_TORQUE_DIRECTION = 0x0012,
        DYN200_REG_BAUD_RATE = 0x0014,
        DYN200_REG_DEVICE_ADDRESS = 0x0016,
        DYN200_REG_STOP_BITS = 0x0018,
        DYN200_REG_COEFFICIENT = 0x001a,
        DYN200_REG_COMMUNICATION_MODE = 0x001c,
        DYN200_REG_SPEED_FILTER = 0x001e,
        DYN200_REG_SPEED_DECIMALS = 0x0020
};

typedef struct Dyn200Measurement {
        int32_t torque_raw;
        int32_t speed_raw;
        int32_t power_raw;
        double torque_nm;
        double speed_rpm;
        double power_kw;
} Dyn200Measurement;

// ═══ ava_tool 数据表用枚举 ═══════════════════════════════════════════════

/*
 * dyn200_port_param.action：一次性动作，执行后自动回到 NONE。
 * CONNECT 会直接调用 dyn200_open_ex() 打开选中的串口，不需要用户再调函数。
 */
typedef enum dyn200_port_action {
        DYN200_PORT_NONE = 0,
        DYN200_PORT_CONNECT = 1,    // 用本表参数打开选中的串口
        DYN200_PORT_DISCONNECT = 2, // 关闭串口
        DYN200_PORT_RESCAN = 3      // 重新枚举串口列表
} dyn200_port_action;

// 通讯模式，取值与传感器菜单一致
typedef enum dyn200_protocol {
        DYN200_PROTOCOL_MODBUS = 1,  // 通信模式 1：Modbus RTU 轮询（需要机码）
        DYN200_PROTOCOL_ACTIVE6 = 6, // 通信模式 0：HEX 6 字节主动上传
        DYN200_PROTOCOL_ACTIVE8 = 8  // 通信模式 3：HEX 8 字节主动上传
} dyn200_protocol;

/*
 * 串口表：搜索（枚举）电脑上的串口设备，并把选中的那一个连上。这张表在面板
 * 里是**按钮式操作表**：没有"下发"、没有"自动读取"，只有一排按钮
 * （搜索 / 连接 / 断开），按下哪个就通过读函数执行哪个动作。用法：
 *
 *   1. 按"搜索"：count 是电脑上的串口数量，name/desc 是 index 那一个
 *   2. 选口：改 index 逐步看列表（看到哪个就是选中哪个），或者直接在 port
 *      里手填 "COM7" / "/dev/ttyUSB0"（手填优先，port 空了才会用 index 那个）
 *   3. 填好 baud_rate/protocol/device_address/stop_bits/timeout_ms
 *   4. 按"连接"即按这些参数把口打开（改完参数再按一次就是重连）
 *   5. action = DISCONNECT 先断开（之后不再自动连）；action = RESCAN 强制
 *      重新枚举——也就是上面的"断开""搜索"两个按钮
 *
 * 这张表里"选中的口"（port 或 index 指向的那个）加上串口参数会被 SDK 记住：
 * 之后 rt 表/参数表要用串口时会自动把连接建起来，所以不必每次手动 CONNECT。
 * 一次都没搜过、没配过这张表时，rt 会返回 DYN200_ERR_NOT_OPEN(-4)。
 *
 * 表名以 "param" 结尾，所以这些参数会随 ava_tool 会话保存。0 表示用默认值。
 * 程序里想显式执行一次"搜索/连接"，用下面的 dyn200_port_apply()。
 */
typedef struct dyn200_port_param {
        uint32_t index;     // 输入：浏览/选择第几个串口，0 起
        uint32_t count;     // 只读：枚举到的串口数量
        char     name[64];  // 只读：例如 "COM7"、"/dev/ttyUSB0"
        char     desc[128]; // 只读：例如 "USB-SERIAL CH340 (COM7)"
        char     port[64];  // 输入：要连接的口；空则用 index 指向的那一个

        dyn200_protocol protocol;          // 0 -> Modbus（1）
        uint32_t        baud_rate;         // 0 -> 9600
        uint32_t        device_address;    // Modbus 机码 1..120；0 -> 1；主动上传模式忽略
        uint32_t        stop_bits;         // 1 或 2；0 -> 1（8N1）
        uint32_t        timeout_ms;        // 0 -> 200（9600 下一次完整读只要 ~26 ms）
        dyn200_port_action action;         // 一次性动作，见 dyn200_port_action

        // 只读反馈
        uint32_t connected;             // 1 = 串口已打开
        char     active_port[64];       // 当前打开的口
        uint32_t active_protocol;       // 当前通讯模式
        uint32_t active_baud_rate;      // 当前波特率
        uint32_t active_device_address; // 当前机码
        int32_t  last_error_code;
        char     last_error[64];
} dyn200_port_param;

/*
 * 定点小数位，必须与传感器菜单里的设置一致，用来把原始值换算成工程量。
 * 0 表示用默认值：扭矩 1 位（原始值 / 10，单位 N·m）、转速 0 位、功率 0 位。
 * 小数位只在本地换算，不写进传感器；read/write_param 都会应用，改完即生效。
 */
typedef struct dyn200_scaling {
        uint32_t torque_decimals; // 0 -> 1；1..6 按菜单；torque_nm = torque_raw / 10^小数位
        uint32_t speed_decimals;  // 0 -> 0；1..6 按菜单
        uint32_t power_decimals;  // 0 -> 0；1..6 按菜单
} dyn200_scaling;

/*
 * 传感器自身设置（写寄存器，掉电保存）。每个寄存器单独一组、单独门控：
 * 编辑某一组里的字段只会置位它自己的 write_<组名>，改一项不会顺手把别的项
 * 复位。三个寄存器都要连接且是 Modbus 模式才能写。
 */
typedef struct dyn200_filter {
        int32_t val; // 滤波强度 1..100；0 -> 不修改
} dyn200_filter;

typedef struct dyn200_zero_on_boot {
        int32_t enable; // 0/1：上电是否自动清零
} dyn200_zero_on_boot;

typedef struct dyn200_direction {
        int32_t reversed; // 0/1：扭矩是否反向
} dyn200_direction;

// 只读反馈：传感器里当前存着的设置值（连上以后自动读回来）
typedef struct dyn200_device_state {
        int32_t filter;       // 当前滤波值
        int32_t zero_on_boot; // 当前上电清零设置
        int32_t reversed;     // 当前扭矩方向
} dyn200_device_state;

/*
 * 连接的只读回显：当前实际在用的串口/通讯模式/波特率。放在参数表里，因为它
 * 不需要跟着每帧刷新（实时表只放高频数据）。
 */
typedef struct dyn200_link_state {
        char     port[64];  // 当前串口
        uint32_t protocol;  // 当前通讯模式
        uint32_t baud_rate; // 当前波特率
} dyn200_link_state;

/*
 * 参数表。device.* 只读，串口打开后每隔约 2 秒自动读回一次，方便对照；
 * 编辑 filter/zero_on_boot/direction 里的值并下发就会写进传感器，
 * 写成功后 device.* 立即刷新。link.* 是当前连接的回显（只读）。
 */
typedef struct dyn200_param {
        struct dyn200_scaling scaling;
        struct dyn200_filter filter;
        struct dyn200_zero_on_boot zero_on_boot;
        struct dyn200_direction direction;

        uint32_t write_filter;       // 编辑 filter.* 时自动置 1
        uint32_t write_zero_on_boot; // 编辑 zero_on_boot.* 时自动置 1
        uint32_t write_direction;    // 编辑 direction.* 时自动置 1

        // 只读反馈
        struct dyn200_device_state device;
        struct dyn200_link_state   link;
        uint32_t connected;
        int32_t  last_error_code;
        char     last_error[64];
} dyn200_param;

// dyn200_rt.command：一次性命令，由 write_command 门控
typedef struct dyn200_command {
        int32_t zero;          // 1 -> 扭矩清零
        int32_t factory_reset; // 1 -> 恢复出厂设置
} dyn200_command;

/*
 * 实时表：只放**需要高频刷新的东西**——测量值、采样计数、一次性命令，以及
 * 本次读取的结果（错误码/错误文本，日志要打）。串口/波特率/通讯模式这类连接
 * 信息在参数表 link.* 里（只读），不跟着每帧刷新。
 *
 * read_rt 采集一帧并回填 measurement.* 与 sample_count；串口没打开时返回
 * DYN200_ERR_NOT_OPEN。write_rt 只在 write_command 置位时下发 command.*。
 */
typedef struct dyn200_rt {
        uint32_t write_command; // 编辑 command.* 时自动置 1
        /*
         * 输入：主动上传模式专用（Modbus 无影响）。
         *   0（默认）= 不等新帧：有就先取最新一帧，没有就沿用上一帧立刻返回。
         *              面板/曲线可以按自己的频率（几十~几百 Hz）刷新，曲线连续；
         *              数据永远是最新一帧、不会滞后（不会像"消费积压"那样回放旧数据）。
         *              如果流超过 1.5 s 没有新帧，
         *              会返回 DYN200_ERR_TIMEOUT 而不是一直沿用，避免看着像卡死。
         *   1        = 按到达顺序逐帧读取；队列为空时等待，适合完整采集。
         *              调用足够快且驱动未溢出时，长期新帧率才等于传感器上传率。
         * sample_count 只在收到新帧时 +1，fresh 告诉这次是不是新帧。
         */
        uint32_t wait_fresh;

        // 只读反馈
        uint64_t sample_count; // 累计成功采样次数
        uint32_t fresh;        // 只读：1 = 本次是新帧，0 = 沿用上一帧
        int32_t  last_error_code;
        char     last_error[64];

        struct dyn200_command command;
        Dyn200Measurement     measurement;
} dyn200_rt;

// ─── ava_tool 数据表接口 ──────────────────────────────────────────────────

DYN200_API int dyn200_read_port_param(dyn200_port_param *param);
/*
 * 显式执行一次串口表的动作（搜索/连接/断开）。面板不用它——面板读这张表就会
 * 搜索并连接（函数名不带 param 后缀，所以不会在面板里变成"下发"）。
 */
DYN200_API int dyn200_port_apply(dyn200_port_param *param);
DYN200_API int dyn200_read_param(dyn200_param *param);
DYN200_API int dyn200_write_param(dyn200_param *param);
DYN200_API int dyn200_read_rt(dyn200_rt *rt);
DYN200_API int dyn200_write_rt(dyn200_rt *rt);

// ─── 扁平 C API ──────────────────────────────────────────────────────────

/*
 * Open an RS-485 serial adapter for Modbus RTU (8N1). Typical arguments are
 * COM7/9600/1 on Windows or /dev/ttyUSB0/9600/1 on Unix. Modbus polling uses
 * device addresses 1..120; address zero is reserved for broadcast.
 */
DYN200_API int dyn200_open(const char *port, int baud_rate, int device_address, int timeout_ms);
// Variant for sensors configured for 8N2 instead of 8N1
DYN200_API int dyn200_open_ex(const char *port, int baud_rate, int device_address, int timeout_ms, int stop_bits);
// Open mode 0/3 active-upload streams; no Modbus device address is involved
DYN200_API int dyn200_open_active(const char *port, int baud_rate, int timeout_ms, int stop_bits);
DYN200_API int dyn200_open_active8(const char *port, int baud_rate, int timeout_ms, int stop_bits);
DYN200_API int dyn200_close(void);
DYN200_API int dyn200_is_open(void);

/*
 * 当前连接信息（串口名/波特率/通讯模式），方便把状态显示在自己的界面上。
 * 串口没打开时返回 DYN200_ERR_NOT_OPEN，几个出参仍然会被填好。
 */
DYN200_API int dyn200_connection(char *port, size_t port_size, int *baud_rate, int *protocol);
// 本机串口数量与第 index 个串口的名字; 等价于串口表的一次读取
DYN200_API uint32_t dyn200_port_count(void);
DYN200_API int      dyn200_port_name(uint32_t index, char *name, size_t name_size, char *desc, size_t desc_size);

// Fixed-point scaling defaults to torque=2, speed=0, power=0 decimals
DYN200_API int dyn200_set_decimals(int torque_decimals, int speed_decimals, int power_decimals);
DYN200_API int dyn200_read(Dyn200Measurement *measurement);

// Generic Modbus access. Register count is limited to 12 by the device manual
DYN200_API int dyn200_read_registers(uint16_t address, uint16_t count, uint16_t *values);
DYN200_API int dyn200_read_i32(uint16_t address, int32_t *val);
DYN200_API int dyn200_write_i32(uint16_t address, int32_t val);

DYN200_API int dyn200_zero(void);
DYN200_API int dyn200_factory_reset(void);
DYN200_API int dyn200_set_filter(int val); // 1..100
DYN200_API int dyn200_set_zero_on_boot(int enabled);
DYN200_API int dyn200_set_torque_direction(int reversed);

// Helpers for communication modes 0 and 3 (active upload)
DYN200_API uint16_t dyn200_crc16(const uint8_t *data, size_t size);
DYN200_API int dyn200_decode_active6(const uint8_t frame[6], int32_t *torque, int32_t *speed);
DYN200_API int dyn200_decode_active8(const uint8_t frame[8], int32_t *torque, uint32_t *speed);
/*
 * 在一段字节流里找**最新**的一帧主动上传数据（从末尾往前找第一个 CRC 正确的帧，
 * 中间夹着噪声/半帧也没关系）。frame_bytes 传 6 或 8。找到返回 1 并把帧起点写进
 * *offset；没有完整帧返回 0。SDK 的非阻塞最新值读取使用它。
 */
DYN200_API int dyn200_scan_active_frame(const uint8_t *data, size_t size, int frame_bytes, size_t *offset);
// Read and CRC-resynchronize one frame from communication mode 0 or 3
DYN200_API int dyn200_read_active6(Dyn200Measurement *measurement);
DYN200_API int dyn200_read_active8(Dyn200Measurement *measurement);

/*
 * Scalar accessors for ava_tool function-bound LOCAL variables.
 * read_* acquires a new frame and returns NAN on failure. get_* only returns
 * the most recently acquired frame, so several channels can share one sample.
 */
DYN200_API int dyn200_update(void);
DYN200_API double dyn200_read_torque_nm(void);
DYN200_API double dyn200_read_speed_rpm(void);
DYN200_API double dyn200_read_power_kw(void);
DYN200_API double dyn200_get_torque_nm(void);
DYN200_API double dyn200_get_speed_rpm(void);
DYN200_API double dyn200_get_power_kw(void);
DYN200_API float dyn200_read_torque_f32(void);
DYN200_API float dyn200_read_speed_f32(void);
DYN200_API float dyn200_read_power_f32(void);
DYN200_API float dyn200_get_torque_f32(void);
DYN200_API float dyn200_get_speed_f32(void);
DYN200_API float dyn200_get_power_f32(void);
DYN200_API int32_t dyn200_read_torque_raw(void);
DYN200_API int32_t dyn200_read_speed_raw(void);
DYN200_API int32_t dyn200_read_power_raw(void);
DYN200_API int32_t dyn200_get_torque_raw(void);
DYN200_API int32_t dyn200_get_speed_raw(void);
DYN200_API int32_t dyn200_get_power_raw(void);

DYN200_API int dyn200_last_error_code(void);
DYN200_API const char *dyn200_last_error(void);

#ifdef __cplusplus
}
#endif
#endif
