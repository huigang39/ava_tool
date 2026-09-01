#pragma once

#include <array>
#include <vector>

namespace ui::guide
{
enum class Topic {
        Overview,
        JLink,
        Variables,
        Recorder,
        Parameters,
        Safety,
        Devices,
        Library,
        Flash,
        RTT,
        Trace,
        RingCapture,
        Motor,
        Calculator,
        Bode,
        DSO,
        Sequence,
        MIDI,
        Inverter,
        Assembly,
        MuJoCo,
        Rewrite,
        Count
};

struct Step {
        const char *target;
        const char *titleEn, *titleZh;
        const char *bodyEn, *bodyZh;
};

struct Chapter {
        const char       *key;
        const char       *titleEn, *titleZh;
        const char       *locationEn, *locationZh;
        std::vector<Step> steps;
};

inline const auto &
Chapters()
{
        static const std::array<Chapter, static_cast<int>(Topic::Count)> chapters = {
            {{"overview",
              "Getting started",
              "快速入门",
              "Help > Getting started",
              "帮助 → 新手引导",
              {{"menu_window",
                "Build a workspace",
                "创建工作区",
                "Add panel creates variable managers and recorders. Drag a tab to dock or detach a panel; drag dividers to "
                "resize it. Open loads a session, Save keeps its layout and settings.",
                "“添加面板”创建变量管理器和变量记录器。拖动标签可停靠或独立悬浮，拖动分隔线调整大小。“打开”加载会话，“保存”保存"
                "布局和配置。"},
               {"jlink_entry",
                "Connect your target",
                "连接设备",
                "Open J-Link in the top toolbar. Select the MCU and SWD speed, then Connect. Connect and reset also restarts "
                "the target program. You can browse panels without connecting hardware.",
                "点击顶栏 J-Link，选择芯片和 SWD "
                "速度，再点击“连接”。“连接并复位”还会重新启动目标程序。没有硬件也可以浏览面板和引导。"},
               {"variable_browser",
                "Find a variable",
                "找到变量",
                "In Variables, open Symbols and load the firmware ELF/AXF. Double-click a symbol to add it to the variable "
                "list. Search locates names, members, array elements and addresses.",
                "在变量管理器中打开符号文件入口，加载与固件匹配的 "
                "ELF/AXF。双击符号加入变量列表；搜索可以定位名称、结构体成员、数组元素和地址。"},
               {"monitor_window",
                "Record a waveform",
                "记录波形",
                "In a recorder, Add Scope creates a plot. Drag a variable name into it. More switches waveform/table, hides "
                "the scope or opens analysis. Export CSV saves samples.",
                "在变量记录器中点击“添加示波器”，将变量名称拖入绘图区。“更多”可切换波形／表格、隐藏示波器或打开分析；“导出 "
                "CSV”保存采样数据。"},
               {"pause_btn",
                "Pause the right thing",
                "区分采集与显示",
                "Pause acquisition stops new samples; Freeze display keeps acquisition running. A recorder and an individual "
                "scope also have their own pause controls. Check each state when resuming.",
                "“暂停采集”停止增加新样本；“冻结画面”只冻结显示，后台采集继续。记录器和单个示波器还有各自的暂停控制，恢复时请检"
                "查对应状态。"},
               {"menu_help",
                "Find help for any panel",
                "查看每个面板的操作",
                "Focus a panel and press F1 for its button-by-button guide. Help > Panel guides lists every chapter. The guide "
                "can be closed, resumed or restarted without changing device state.",
                "点击任意面板后按 F1，可查看它的功能和按钮。“帮助 → "
                "面板指南”提供完整目录。引导可随时关闭、继续或重看，不会代替你执行设备操作。"}}},
             {"jlink",
              "J-Link connection",
              "J-Link 连接与采集",
              "Workspace toolbar > J-Link",
              "工作区顶栏 → J-Link",
              {{"jlink_entry",
                "Open settings",
                "打开设置",
                "Click J-Link to open settings; click outside or click it again to dismiss. Drag panel edges or its corner to "
                "resize. Escape "
                "cancels an active resize.",
                "点击 J-Link 打开设置，点击弹窗外部或再次点击入口关闭。拖动浮窗边缘或右下角调整大小；调整过程中按 Esc "
                "可恢复原尺寸。"},
               {"device_btn",
                "Target and SWD speed",
                "芯片与 SWD 速度",
                "Select device chooses the MCU and core. SWD speed controls the debug connection, not sample frequency. Lower "
                "it when the debug link is unstable.",
                "“选择设备”指定 MCU 型号和内核。“SWD 速度”控制调试链路速度，不等于采样频率；链路不稳定时可降低。"},
               {"connect_btn",
                "Connect, reset and disconnect",
                "连接、复位与断开",
                "Connect attaches to the target. Connect and reset restarts it before sampling. After connection, Disconnect "
                "releases the probe and Reset MCU restarts the program.",
                "“连接”接入目标；“连接并复位”在接入后重启程序。连接成功后，“断开”释放探针，“复位 MCU”重启程序。"},
               {"pause_btn",
                "Acquisition and terminal",
                "采集频率与终端",
                "HSS rate limits sample frequency; Ctrl-click the slider to type. Pause acquisition controls monitor sampling. "
                "RTT terminal opens target text output and command input.",
                "“采集频率”限制 HSS 采样速率，Ctrl+点击滑块可输入数值。“暂停采集”控制监视器采样；“RTT "
                "终端”打开目标文本输出和命令输入。"}}},
             {"variables",
              "Variable Manager",
              "变量管理器",
              "Window > Add Variable Manager",
              "窗口 → 添加变量管理器",
              {{"variable_browser",
                "Load symbols",
                "加载符号",
                "The symbol-file entry loads/replaces a local ELF/AXF or opens a remote file. Double-click a symbol to add it. "
                "Reload after rebuilding firmware so names, types and addresses match.",
                "符号文件入口可加载／替换本地 "
                "ELF/AXF，也可打开远程文件。双击符号加入列表；固件重新编译后应重载符号，保持名称、类型和地址一致。"},
               {"variables.add",
                "Add and refresh variables",
                "添加变量与刷新",
                "The plus button adds a variable with its source, address and type. The refresh interval controls table "
                "polling. Import/export .var shares a variable list; session Save keeps the full workspace.",
                "加号按钮添加变量，可设置来源、地址和类型。刷新间隔控制表格读取频率；导入／导出 .var "
                "用于共享变量列表，保存会话则保留整个工作区。"},
               {"variables.search",
                "Search and focus",
                "搜索与聚焦",
                "Search finds names, aliases, nested members and addresses. Choose a result to expand ancestors and scroll to "
                "it. Arrows expand groups; click Focus to enter a group. Tree lines show parent-child relationships, and "
                "hovering a row highlights its ancestor path.",
                "搜索支持名称、别名、嵌套成员和地址。点击结果展开上级并定位；箭头展开分组，点击“聚焦”进入分组。树线连接父子变量"
                "，悬停行可高亮上级路径。"},
               {"variables.values",
                "Inspect and edit values",
                "查看与修改数值",
                "Read-only rows cannot write. Writable value editors submit the edited value; row actions expose format, "
                "properties, copy and unit conversion. Pointer addresses open a read-only inspector.",
                "只读行不能写入。可写数值框提交修改值；行操作中可设置格式、属性、复制和单位换算。点击指针地址打开只读检查窗口"
                "。"},
               {"variables.tools",
                "Plot and configure",
                "绘图与工具",
                "Drag a variable name into a recorder to plot it. Parameter configuration edits structured settings; Safety "
                "decodes status and fault fields. These tools use the current manager's variables and symbols.",
                "将变量名称拖入变量记录器可绘图。“参数配置”编辑结构化配置，“功能安全”解析状态与故障字段；这些工具使用当前管理器"
                "的变量和符号。"}}},
             {"recorder",
              "Variable Recorder",
              "变量记录器",
              "Window > Add Variable Recorder",
              "窗口 → 添加变量记录器",
              {{"monitor_window",
                "Add scopes and channels",
                "添加示波器与通道",
                "Add Scope creates a plotting area. Drag a variable name into it to create a channel. Scope and channel names "
                "can be renamed; dragging a scope title changes its order.",
                "“添加示波器”创建绘图区，将变量名称拖入可新增通道。示波器和通道均可重命名；拖动示波器标题调整顺序。"},
               {"recorder.search",
                "Search and layout",
                "搜索与布局",
                "Search by variable name, path or address, then click a result to reveal it, including a hidden scope. "
                "Stacked/grid changes arrangement without stopping acquisition.",
                "按变量名称、路径或地址搜索，点击结果定位，隐藏的示波器也会被展开。“纵向排列／网格排列”调整布局，不停止采集。"},
               {"recorder.focus",
                "Focus, hide and restore",
                "聚焦、隐藏与恢复",
                "Hover Focus for a temporary preview; leave to restore the previous view. Click to keep focus, including "
                "revealing a hidden scope. More contains waveform/table, hide, rename and delete actions.",
                "悬停“聚焦”临时预览，移开恢复原视图；点击固定聚焦，并展开原本隐藏的示波器。“更多”包含波形／表格、隐藏、重命名和"
                "删除。"},
               {"recorder.statistics",
                "Statistics and frequency analysis",
                "统计与频域分析",
                "Statistics shows current/min/max and other metrics for each channel. More > Frequency analysis enables FFT; "
                "FFT settings choose resolution and peak count. Axis and plot menus control fitting and time format.",
                "“统计”查看通道当前值、最小值、最大值等指标。“更多 → 频域分析”启用 FFT，“FFT "
                "设置”调整分辨率和峰值数；坐标轴和绘图菜单设置显示范围与时间格式。"},
               {"recorder.capture",
                "Pause and export",
                "暂停与导出",
                "Pause acquisition affects this recorder; a scope's pause affects that scope. Export CSV saves samples. "
                "Options configures sampling, history and display. Deleting samples is different from hiding a scope.",
                "记录器的“暂停采集”作用于当前记录器，单个示波器的暂停只影响该示波器。“导出 "
                "CSV”保存数据，“选项”设置采样、历史和显示；隐藏示波器不会删除数据。"}}},
             {"parameters",
              "Parameter Configuration",
              "参数配置",
              "Tools > Parameter Configuration",
              "工具 → 参数配置",
              {{"parameters.target",
                "Choose the target",
                "选择目标",
                "Target searches variables and loaded symbols from the manager. Select the structure whose fields you want to "
                "edit; changing targets changes the parameter layout and draft history.",
                "“目标”从变量管理器和已加载符号中搜索结构体。选择后显示对应字段；切换目标会切换参数布局和草稿历史。"},
               {"parameters.presets",
                "Presets and configuration",
                "预设与配置",
                "Presets and configuration opens the JSON and preset directory controls. Choose mechanical, hardware and "
                "software presets independently. A star marks changes relative to a saved preset.",
                "“预设与组合配置”打开 "
                "JSON、预设目录及选择控件。机械、硬件、软件可分别选择预设；星号表示相对已保存预设存在修改。"},
               {"parameters.fields",
                "Browse and edit fields",
                "查找与编辑字段",
                "Common, Mechanical, Hardware and Software split the structure into sections. Search locates a field; "
                "expand/collapse controls the tree. Editors follow field types, including enum choices.",
                "“公共／机械／硬件／软件”按字段归属分组。搜索定位字段，展开／折叠控制树结构；数值和枚举使用对应编辑器。"},
               {"parameters.save",
                "Save a file or write the device",
                "保存文件与写入设备",
                "Save and Save as update configuration files. Restore default parameters loads defaults. Read parameters loads "
                "current device values; Write parameters sends the edited configuration to the selected target.",
                "“保存／另存为”保存配置文件，“恢复默认参数”载入默认配置。“读取参数”读取设备当前值，“写入参数”将编辑后的配置下发"
                "到所选目标；文件保存和设备写入是不同操作。"}}},
             {"safety",
              "Safety diagnostics",
              "功能安全",
              "Tools > Safety",
              "工具 → 功能安全",
              {{"safety.config",
                "Load a decoder",
                "加载解析配置",
                "Configuration lets you select your own directory and JSON, or open a file directly. The session remembers "
                "their paths without copying the file. Load/reload rereads it; a parse failure keeps the last valid "
                "configuration.",
                "“配置”中选择自己的目录和 "
                "JSON，也可直接打开文件。会话只记住路径，不复制配置。“加载／重载”重新读取文件；解析失败"
                "保留上一次有效配置。"},
               {"safety.system",
                "Inspect system variables",
                "查看系统变量",
                "System variables groups diagnostic objects. Reidentify objects updates matches against loaded symbols. Search "
                "locates objects and fields; status/error and all-fields views choose the level of detail.",
                "“系统变量”按诊断对象分组。“重新识别对象”根据符号更新匹配；搜索定位对象和字段，“状态与错误／全部字段”控制显示范"
                "围。"},
               {"safety.scalar",
                "Decode a single value",
                "解析单个数值",
                "Single value/offline decoding accepts a manual value, or selects a scalar from Variable Manager for live "
                "reading. Unbind returns to manual input without deleting the source variable.",
                "“单值／离线解析”支持手动输入，或从变量管理器选择标量自动读取。“解除绑定”返回手动输入，不删除源变量。"},
               {"safety.report",
                "Understand and copy results",
                "查看与复制结果",
                "The report explains matched bits, causes, checks and clearing conditions from the JSON. Copy report exports "
                "the text. Stale data and read failures are marked separately from decoded status.",
                "结果按 JSON "
                "解释匹配位、原因、排查和清除条件。“复制解析报告”复制文本。读取失败和最后有效值会单独标注，不能当作当前实时状态"
                "。"}}},
             {"devices",
              "Device Manager",
              "设备管理器",
              "Tools > Device Manager",
              "工具 → 设备管理器",
              {{"devices.overview",
                "Devices and templates",
                "设备与模板",
                "The overview lists configured devices, discovered instances and SDK devices. Add device creates an instance "
                "from a template. Search locates the relevant device or property.",
                "总览显示已配置设备、发现的实例和 SDK 设备。“添加设备”从模板创建实例，搜索可定位相关设备或属性。"},
               {"devices.properties",
                "Properties",
                "属性",
                "Properties shows each mapped value, source and access mode. Add property extends a template; View properties "
                "opens an instance. Writes are available only for writable properties.",
                "“属性”显示映射值、来源和读写权限。“添加属性”扩展模板，“查看属性”打开实例；仅可写属性支持下发。"},
               {"devices.actions",
                "Actions and binding",
                "操作与绑定",
                "Actions lists device methods. Bind action connects a template action to its implementation; Execute action "
                "invokes it. Check parameters before running a method that changes hardware state.",
                "“操作”列出设备方法。“绑定操作”关联具体实现，“执行操作”调用方法；会改变硬件状态的方法需要先检查参数。"},
               {"devices.templates",
                "SDK and reusable templates",
                "SDK 与复用模板",
                "SDK and settings loads libraries and headers. Open SDK console inspects lower-level functions. Import/export "
                ".avadev shares templates; deleting a template and its devices removes the configured instances.",
                "“SDK 与设置”加载动态库和头文件，“打开 SDK 控制台”查看底层函数。导入／导出 .avadev "
                "共享模板；“删除模板及设备”会同时移除相关实例。"}}},
             {"library",
              "Dynamic Library Caller",
              "动态库调用器",
              "Window > Add Dynamic Library Caller",
              "窗口 → 添加动态库调用器",
              {{"library.load",
                "Load interfaces",
                "加载接口",
                "C/C++ loads a dynamic library and parses its header. Reparse refreshes declarations; Unload releases the "
                "library. Python has separate script, function and class controls.",
                "“C/C++”加载动态库并解析头文件，“重新解析”更新声明，“卸载”释放库；“Python”使用独立的脚本、函数和类控件。"},
               {"library.tables",
                "Data tables",
                "数据表",
                "Data tables displays structured SDK values. Read refreshes values and Write submits editable fields. Table "
                "controls and source selection determine which instance is accessed.",
                "“数据表”显示 SDK 的结构化数据。“读取”刷新数值，“写入”提交可编辑字段；通过表格控件和来源选择确认操作的实例。"},
               {"library.calls",
                "Functions and classes",
                "函数与类",
                "C functions and C++ classes expose typed arguments and return values. Call executes the selected function; "
                "object controls create or remove class instances. Variable selectors can bind input or return values.",
                "“C 函数／C++ "
                "类”展示参数和返回值。“调用”执行所选函数，对象控件创建或移除类实例；变量选择控件可关联输入或返回值。"},
               {"library.logs",
                "Symbols and call logs",
                "符号与调用日志",
                "Symbols searches exported names and locates a result. Call log records outcomes; Auto scroll follows new "
                "entries and Clear log clears the displayed history. Logs can be selected and copied.",
                "“符号”搜索导出名称并定位结果。“调用日志”记录调用结果，“自动滚动”跟随新增记录，“清空日志”清除显示历史；日志文本"
                "可选择复制。"}}},
             {"flash",
              "J-Flash",
              "J-Flash",
              "Tools > J-Flash",
              "工具 → J-Flash",
              {{"flash.file",
                "Choose firmware",
                "选择固件",
                "Firmware programming > Browse selects a local file. Refresh firmware list queries the configured remote "
                "source; Download and select prepares a remote file for programming.",
                "“固件烧录 → 浏览”选择本地固件。“刷新固件列表”查询已配置的远程来源，“下载并选用”准备远程固件用于烧录。"},
               {"flash.program",
                "Program and verify",
                "烧录与校验",
                "Program and verify writes the selected image. Full erase before programming and Reset and run after "
                "programming change the operation. Confirm the target and file before starting; progress and failures appear "
                "in the panel.",
                "“烧录并校验”写入所选文件。“烧录前全片擦除”和“烧录后复位并运行”改变执行行为；开始前确认目标和固件，执行进度与失"
                "败原因显示在面板中。"},
               {"flash.read",
                "Read and inspect Flash",
                "读取与查看 Flash",
                "Flash read loads a target range or opens a local BIN. Jump changes the viewed offset; Save BIN exports bytes. "
                "Array type and byte order determine how those bytes are interpreted.",
                "“Flash 读取”读取目标范围或打开本地 BIN。“跳转”定位偏移，“保存 "
                "BIN”导出字节；数组类型和字节序决定数据的解释方式。"},
               {"flash.erase",
                "Erase selected sectors",
                "擦除扇区",
                "The sector list lets you select sectors, select all or clear the selection. Erase Flash requires confirmation "
                "and permanently erases the selected target data.",
                "扇区列表可勾选、全选或清空选择。“擦除 Flash”需要确认，会实际擦除所选目标数据。"}}},
             {"rtt",
              "J-Link RTT Terminal",
              "J-Link RTT 终端",
              "J-Link > RTT terminal",
              "J-Link → RTT 终端",
              {{"rtt.connection",
                "Target text channel",
                "目标文本通道",
                "RTT uses the active J-Link target. Configure the RTT channel and control-block search/address to match "
                "firmware, then start receiving. A disconnected probe cannot provide new target output.",
                "RTT 使用当前 J-Link 目标。按固件配置 RTT "
                "通道和控制块搜索／地址，再开启接收；探针断开后无法获得新的目标输出。"},
               {"rtt.receive",
                "Receive and inspect",
                "接收与查看",
                "The log area displays incoming target text. Scroll to inspect history and select text to copy it. Clear "
                "removes the displayed log, not target memory.",
                "日志区显示目标输出，可滚动检查历史、选择文字复制。“清空”清除显示日志，不会清除目标内存。"},
               {"rtt.send",
                "Send a command",
                "发送命令",
                "Use the terminal input and its send control for firmware-supported commands. Select the correct channel and "
                "line ending; sending text only has an effect if the firmware handles it.",
                "在终端输入框填写固件支持的命令并发送。选择正确通道与换行方式；固件实现了对应命令处理才会产生效果。"}}},
             {"trace",
              "SWO / ITM Trace",
              "SWO / ITM 跟踪",
              "Tools > SWO / ITM Trace",
              "工具 → SWO / ITM 跟踪",
              {{"trace.setup",
                "Configure tracing",
                "配置跟踪",
                "Choose the target clock, SWO bitrate and ITM stimulus ports to match firmware. These settings describe the "
                "trace stream and are separate from the SWD clock.",
                "按固件配置目标时钟、SWO 码率和 ITM 激励端口。这些参数用于解析跟踪流，与 SWD 调试时钟不同。"},
               {"trace.capture",
                "Start and stop",
                "启动与停止",
                "The capture controls start or stop trace reception through J-Link. Connection and error status show whether "
                "data can currently arrive; changing options alone does not prove reception works.",
                "采集控制启动或停止通过 J-Link "
                "接收跟踪数据。连接状态和错误信息反映当前是否能接收数据，修改参数本身不代表已经成功接收。"},
               {"trace.inspect",
                "Read trace output",
                "查看跟踪输出",
                "Inspect the decoded output and counters, use the display options to narrow what is shown, and clear the log "
                "when starting a new investigation. Select log text to copy it.",
                "查看解码输出和计数，通过显示选项缩小查看范围；开始新一轮排查时可清空日志，日志文本支持选择复制。"}}},
             {"ring",
              "Ring Buffer Capture",
              "环形数据采集",
              "Tools > Ring Buffer Capture",
              "工具 → 环形数据采集",
              {{"ring.config",
                "Configure the buffer",
                "配置缓冲区",
                "Set the ring-buffer address/layout and channels to match target firmware. Apply configuration transfers the "
                "selected configuration; Add channel defines another decoded signal.",
                "按目标固件设置环形缓冲区地址、布局与通道。“应用配置”应用所选配置，“添加通道”增加需要解码的信号。"},
               {"ring.capture",
                "Live and retained capture",
                "实时采集与保留数据",
                "Start and Stop control capture. Read retained data retrieves a buffer preserved by the target. Processing "
                "status indicates when a read or conversion is still running.",
                "“启动／停止”控制采集，“读取保留数据”读取目标保留的缓冲数据；“处理中”表示读取或转换尚未完成。"},
               {"ring.plot",
                "Display and export",
                "显示与导出",
                "Show and marker controls change individual channel rendering; Auto fits the plot. Export CSV saves captured "
                "samples. Clear data removes local captured data, and Delete channel removes its configuration.",
                "“显示”和标记点控制单个通道的绘制，“自动”适配坐标。“导出 "
                "CSV”保存采样，“清空数据”清除本地采集结果，“删除通道”移除该通道配置。"}}},
             {"motor",
              "Motor Control",
              "电机控制",
              "Tools > Motor Control",
              "工具 → 电机控制",
              {{"motor.connection",
                "Select a connection",
                "选择连接",
                "Connection chooses the transport, device and runtime. Connect (without enabling) establishes communication; "
                "it does not enable motor output. Disconnect releases the connection.",
                "“连接”页选择通信方式、设备和运行库。“连接（不使能）”只建立通信，不使能电机输出；“断开连接”释放连接。"},
               {"motor.control",
                "Control the motor",
                "控制电机",
                "Control contains the available drive modes and command targets. Enabling or sending a target can move real "
                "hardware. Check mode, units, limits and the connected axis before applying commands.",
                "“控制”提供驱动模式与目标指令。使能或下发目标可能使实际设备运动；操作前确认模式、单位、限值和连接轴。"},
               {"motor.dashboard",
                "Read instruments and faults",
                "仪表与诊断",
                "Dashboard presents live drive feedback. Diagnostics, Communication diagnostics and Operation log show faults, "
                "link state and command outcomes so a failed action can be traced.",
                "“仪表”显示实时反馈。“诊断”“通信诊断”和“操作记录”分别查看故障、链路状态与命令结果，用于定位失败操作。"},
               {"motor.parameters",
                "Write selected parameters",
                "写入所选参数",
                "Parameter editors prepare values locally. Write selected parameters opens the write action/confirmation for "
                "the selected fields; a local edit alone is not a successful device write.",
                "参数编辑器先修改本地数值。“写入所选参数”对选定字段执行写入及确认；本地编辑完成并不代表设备已经写入成功。"}}},
             {"calculator",
              "Motor Parameter Calculator",
              "电机参数计算器",
              "Tools > Motor Parameter Calculator",
              "工具 → 电机参数计算器",
              {{"calculator.inputs",
                "Choose the calculation",
                "选择计算项目",
                "Motor Parameters selects a motor profile; Add creates a profile and Delete removes the selected one. Enter "
                "resistance, inductance, pole pairs and torque/back-EMF inputs in the displayed units.",
                "“电机参数”选择电机档案，“添加”新建档案，“删除”移除当前档案。按标注单位填写电阻、电感、极对数及转矩／反电动势输"
                "入。"},
               {"calculator.values",
                "Inspect results",
                "检查计算结果",
                "Review the computed values alongside their units. Invalid or missing inputs must be corrected before the "
                "result is useful. Copy the needed result into the corresponding configuration deliberately.",
                "结合单位查看计算值；缺失或无效输入需要先修正。需要用于配置时，再将结果填写到对应参数。"},
               {"calculator.efficiency",
                "Efficiency data",
                "效率数据",
                "Efficiency selects torque, speed, voltage and current sources and their units. Record/Stop stores efficiency "
                "points; Clear removes them. Export table and Export map save the data and efficiency map.",
                "“效率计算”选择转矩、转速、电压和电流来源及单位。“开始记录／停止记录”采集效率点，“清除”移除已记录点；“导出表格"
                "／导出效率图”保存结果。"}}},
             {"bode",
              "Bode Plot",
              "伯德图",
              "Tools > Bode Plot",
              "工具 → 伯德图",
              {{"bode.source",
                "Select response data",
                "选择响应数据",
                "Configure the excitation/response sources and sweep settings. Read obtains configured data; Calculate derives "
                "the frequency response from available samples.",
                "设置激励／响应来源及扫频参数。“读取”获取配置的数据，“计算”根据已有样本计算频率响应。"},
               {"bode.run",
                "Run a sweep",
                "执行扫频",
                "Start and Stop control the configured sweep. Confirm the connected target and excitation settings before "
                "starting a sweep that writes a physical output.",
                "“开始／停止”控制所配置的扫频。如果扫频会写入实际输出，开始前应确认连接目标和激励设置。"},
               {"bode.analysis",
                "Analyze and measure",
                "分析与测量",
                "Automatic analysis and Analysis mode choose the report. Copy analysis report exports it. Show all fits the "
                "curves; Measure adds measurements and Clear measurements removes them.",
                "“自动分析／分析模式”控制报告分析，“复制分析报告”导出文本。“显示全部”适配曲线，“测量”添加测量，“清除测量”移除测"
                "量标记。"}}},
             {"dso",
              "DSOX2024A oscilloscope",
              "DSOX2024A 示波器",
              "Dynamic Library Caller > DSOX2024A library > Oscilloscope GUI",
              "动态库调用器 → DSOX2024A 动态库 → 示波器界面",
              {{"dso.connect",
                "Connect an instrument",
                "连接仪器",
                "Connection method chooses the supported instrument transport. Set its address and Connect; Disconnect "
                "releases it. Reset instrument changes the instrument state and requires confirmation.",
                "“连接方式”选择仪器通信方式，设置地址后点击“连接”；“断开连接”释放仪器。“复位仪器”会改变仪器状态，需要确认。"},
               {"dso.channels",
                "Channels and trigger",
                "通道与触发",
                "Channel selects the channel to configure. Show channel controls visibility, Coupling chooses the input "
                "coupling, and Edge trigger source/edge set the trigger condition.",
                "“通道”选择配置对象，“显示通道”控制可见性，“耦合”设置输入耦合；“边沿触发源／触发边沿”设置触发条件。"},
               {"dso.waveform",
                "Read waveforms",
                "读取波形",
                "Read waveform retrieves instrument samples. Auto refresh waveform repeats retrieval. Compare displayed scale "
                "and trigger settings with the physical instrument when interpreting a capture.",
                "“读取波形”获取仪器样本，“自动刷新波形”重复读取。分析采集结果时，应同时检查显示的量程与仪器触发设置。"}}},
             {"sequence",
              "Sequence Editor",
              "序列编辑器",
              "Tools > Sequence Editor",
              "工具 → 序列编辑器",
              {{"sequence.files",
                "Create and share a sequence",
                "创建与共享序列",
                "Import loads a sequence and Export saves it; Folder opens its storage location. Objects and variable "
                "selectors bind operations to the intended values or SDK calls.",
                "“导入”加载序列，“导出”保存序列，“文件夹”打开存储位置。对象与变量选择控件将操作关联到指定数值或 SDK 调用。"},
               {"sequence.steps",
                "Build the steps",
                "编排步骤",
                "Add Execute, Delay, Condition, Loop, Count or Exit steps. Type changes a step's operation; branch controls "
                "add Else actions. Drag the step handle to change order.",
                "用“+ 执行／延时／判断／循环／计数／退出”添加步骤。“类型”切换步骤动作，分支控件添加 Else "
                "操作；拖动步骤手柄调整顺序。"},
               {"sequence.run",
                "Play and stop",
                "播放与停止",
                "Play runs the sequence and Stop requests it to stop. Per-step run executes the chosen step. A sequence can "
                "call real device operations, so review its targets and arguments first.",
                "“播放”运行序列，“停止”请求停止；步骤上的运行按钮执行所选步骤。序列可能调用实际设备操作，运行前先检查目标和参数"
                "。"},
               {"sequence.logs",
                "Inspect execution",
                "查看执行结果",
                "The execution log records step outcomes. Clear log removes log text; Delete step and Clear all change the "
                "sequence itself. Export the sequence to keep reusable work.",
                "执行日志记录步骤结果。“清空日志”只清除日志，“删除步骤／全部清空”修改序列本身。需要复用的编排可通过导出保留"
                "。"}}},
             {"midi",
              "MIDI Player",
              "MIDI 播放器",
              "Tools > MIDI Player",
              "工具 → MIDI 播放器",
              {{"midi.file",
                "Load MIDI",
                "加载 MIDI",
                "Open MIDI imports a file. Inspect its tracks and notes, then choose the polyphony behavior. Clear removes the "
                "current imported content.",
                "“打开 MIDI”导入文件。查看轨道和音符，再通过“复音选择”确定处理方式；“清空”移除当前导入内容。"},
               {"midi.bind",
                "Bind the output",
                "绑定输出",
                "Select the target variables used for playback. Clear binding removes that association. Start HFI playback can "
                "control real hardware through those targets; confirm the binding before starting.",
                "选择播放所需的目标变量，“清除绑定”解除关联。“开始 HFI 播放”可能通过这些目标控制实际硬件，开始前应核对绑定。"},
               {"midi.play",
                "Playback and export",
                "播放与导出",
                "Pause, Continue and Stop control playback. Export CSV saves decoded note data; Export C header generates "
                "source data for firmware integration.",
                "“暂停／继续／停止”控制播放。“导出 CSV”保存解码音符数据，“导出 C 头文件”生成可供固件使用的数据。"}}},
             {"inverter",
              "Inverter Simulation",
              "逆变器仿真",
              "Tools > Inverter Simulation",
              "工具 → 逆变器仿真",
              {{"inverter.model",
                "Choose models and solver",
                "选择模型与求解器",
                "Select ngspice sets the solver executable. Import SPICE model loads device models; package and circuit fields "
                "describe the simulated half bridge and test conditions.",
                "“选择 ngspice”设置求解器程序，“导入 SPICE 模型”加载器件模型；封装与电路字段描述半桥和测试条件。"},
               {"inverter.run",
                "Run and inspect the solve",
                "运行与查看求解",
                "Run ngspice starts the simulation and Cancel stops the pending run. Solver log/netlist directory contains "
                "diagnostics for missing models or failed solves.",
                "“运行 ngspice”开始仿真，“取消”停止当前运行。“求解日志／网表目录”用于检查模型缺失或求解失败原因。"},
               {"inverter.plots",
                "Compare waveforms",
                "对比波形",
                "Second turn-on selects the corresponding event; Overlay previous waveform compares runs. Clear probes removes "
                "measurement probes, and Export CSV saves simulated data.",
                "“第二次开通”查看对应事件，“叠加上一次波形”对比前后运行结果。“清除探针”移除测量探针，“导出 "
                "CSV”保存仿真数据。"}}},
             {"assembly",
              "Assembly Viewer",
              "汇编查看器",
              "Tools > Assembly Viewer",
              "工具 → 汇编查看器",
              {{"assembly.source",
                "Choose code to inspect",
                "选择要查看的代码",
                "Load an ELF/AXF in Variable Manager first. The ELF selector chooses which loaded firmware file to inspect; no "
                "target connection is needed to inspect that file.",
                "先在变量管理器加载 ELF/AXF，再通过 ELF 下拉框选择要查看的固件文件。检查文件本身不需要连接目标。"},
               {"assembly.decode",
                "Disassemble",
                "反汇编",
                "Disassemble decodes the selected firmware into addresses and instructions. The filter searches function names "
                "or instruction text; scroll horizontally to inspect long lines. This does not execute or patch code.",
                "“反汇编”将所选固件解码为地址和指令。过滤框搜索函数名或指令文本，横向滚动可查看长行；不会执行或修改这些代码"
                "。"}}},
             {"mujoco",
              "MuJoCo simulation",
              "MuJoCo 仿真",
              "Tools > MuJoCo",
              "工具 → MuJoCo",
              {{"mujoco.model",
                "Model and simulation link",
                "模型与仿真连接",
                "The MuJoCo workspace combines model rendering, simulation controls and recorder scopes. Select the model and "
                "matching simulation/shared-memory connection in its controls.",
                "MuJoCo 工作区组合模型画面、仿真控制和记录器示波器。通过控制区选择模型及匹配的仿真／共享内存连接。"},
               {"mujoco.targets",
                "Targets and PD parameters",
                "目标与 PD 参数",
                "Reference Targets and PD Parameters are editable tables linked to simulation fields. Confirm field units "
                "before changing a target, gain or feedforward value.",
                "“目标设置”和“PD 参数”是关联到仿真字段的可编辑表格。修改目标、增益或前馈值前，应先确认字段单位。"},
               {"mujoco.scopes",
                "Compare feedback",
                "对比仿真反馈",
                "Position, velocity, torque and error scopes use the recorder's search, focus, statistics and export controls. "
                "Drag the model/control divider to adjust the workspace proportions.",
                "位置、速度、转矩与误差示波器共用记录器的搜索、聚焦、统计和导出功能。拖动模型与控制区分隔线可调整显示比例。"}}},
             {"rewrite",
              "Variable editor",
              "变量修改窗口",
              "Variable row > Modify variable",
              "变量行操作 → 修改变量",
              {{"rewrite.fields",
                "Edit a local draft",
                "编辑本地草稿",
                "The editor expands a structured variable into typed fields. Edit values locally and use Changed only to "
                "inspect differences; this does not immediately write the target.",
                "编辑器将结构体展开为带类型的字段。本地修改后可用“只看改动”检查差异，不会立即写入目标。"},
               {"rewrite.apply",
                "Restore or write changes",
                "恢复或写入改动",
                "Restore original values discards the draft changes. Write changes submits changed fields to the current "
                "target. A disconnected target or changed symbol layout prevents using a stale draft.",
                "“恢复原值”撤销草稿修改，“写入改动”将变化字段下发到当前目标。断开连接或符号布局变化后，旧草稿不能继续写入。"},
               {"rewrite.trace",
                "Trace writes",
                "跟踪写入",
                "Use DWT to track writes configures supported target watchpoints for investigation. It is a debugging action "
                "separate from editing a value; review the target's debug support and current connection.",
                "“使用 DWT "
                "跟踪写入”配置目标支持的观察点，用于排查写入来源。这是独立的调试操作，需要确认目标调试支持与连接状态。"}}}}};
        return chapters;
}
} // namespace ui::guide
