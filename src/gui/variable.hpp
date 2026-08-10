#ifndef VARIABLE_HPP
#define VARIABLE_HPP

#include "timeops.h"
#include <array>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "app_log.hpp"
#include "core/bin_parser.hpp"
#include "core/dwarf_parser.hpp"
#include "core/elf_parser.hpp"
#include "core/json_parser.hpp"
#include "core/parser.hpp"
#include "module.h"

class MonitorChannel;
struct ChannelDropPayload;
struct StructChannelPayload;
struct WatchMirrorRequest;

enum class PortType { JLINK, UDP, SHM, MANUAL, LOCAL, AUDIO };

struct VarEntry {
        std::string name;
        DataType    type;
        PortType    port;
        u64         addr; // address or offset
        bool        isStruct{false};
        u32         bitOffset{0};
        u32         bitSize{0};

        std::string typeStr;  // Full type description (e.g. "struct my_struct")
        std::string valueStr; // Current value as string for display
        bool        writable;
        bool        is_editing = false;
        char        editBuf[64]{};

        // Port specific config
        struct {
                char ip[16];
                u16  port;
        } udp{};
        struct {
                char  name[64];
                shm_t handle;
                bool  inited = false;
        } shm{};
        struct {
                int  deviceIndex = -1;
                char deviceName[128]{};
        } audio{};

        u64  typeOff     = 0; // For DWARF nested display
        bool selected    = false;
        bool addrUnknown = false; // symbol missing from the (reloaded) ELF → show "UNKNOWN"

        // User-editable enum label definitions (overrides DWARF when non-empty)
        struct EnumDef {
                std::string name;
                i64         value;
        };
        std::vector<EnumDef> enumDefs;
        // Manually defined struct fields (LOCAL port only; non-empty → tree display)
        struct StructField {
                char     name[64]{};
                DataType type{DataType::U32};
                u32      byteOffset{0};
        };
        std::vector<StructField> structFields;

        // Per-member enum overrides for struct/array sub-variables (keyed by full member path)
        std::unordered_map<std::string, std::vector<EnumDef>> memberEnumDefs;
        struct MemberOverride {
                std::string alias;
                bool        hasAddress{false};
                u64         address{0};
                bool        hasType{false};
                DataType    type{DataType::UNKNOWN};
                bool        hasWritable{false};
                bool        writable{true};
        };
        // Per-member display/access overrides, keyed by the canonical full
        // DWARF path. The canonical key remains unchanged for ELF re-resolution.
        std::unordered_map<std::string, MemberOverride> memberOverrides;
        // Paths of sub-variables hidden by the user (right-click → Delete)
        std::set<std::string> hiddenMembers;
        // Paths of struct/array nodes the user has expanded; persisted so the
        // tree restores its open state across sessions. Key = full member path
        // (same format as hiddenMembers / memberEnumDefs). The top-level row
        // is keyed by `name`.
        std::set<std::string> expandedMembers;
};

struct SearchEntry {
        std::string path;
        u64         addr;
        DataType    type;
        PortType    defaultPort;
        u64         typeOff; // For DWARF types
        u32         bitOffset{0};
        u32         bitSize{0};
};

struct ElfLoadingTask {
        std::atomic<bool> aborted{false};
};

class Variable
{
      private:
        std::string name_{};  // Stable internal id: map key + ImGui window id (never changes)
        std::string title_{}; // User-facing window title; falls back to name_ when empty
        char        renameBuf_[64]{};
        std::string cfgPath_{}, binPath_{}, elfPath_{};

      public:
        // The list of active variables being monitored/modified
        std::vector<VarEntry> vars_;
        // Flattened symbols for global search
        std::vector<SearchEntry> searchPool_;

      private:
        i32 toastDismissTime_{2000};

        ElfInfo                         elfInfo_{};
        dwarf::Info                     dwarfInfo_{};
        char                            elfFilter_[128]{};
        bool                            elfFilterObjectsOnly_{true};
        i32                             elfArrayMaxElems_{64};
        std::filesystem::file_time_type elfLastWriteTime_{};
        bool                            elfReloaded_{false};
        std::atomic<bool>               propertiesChanged_{false};
        std::atomic<bool>               isElfLoading_{false};
        mutable std::mutex              mtxElf_{};
        std::shared_ptr<ElfLoadingTask> currentLoadingTask_;

        // For JSON/BIN tree display
        DataTree dataTree_;

        f32  watchListHeight_     = 300.0f;
        bool symBrowserCollapsed_ = false;

        u64 lastUpdateTs_{0};
        u32 updateIntervalMs_{200};

        // Background J-Link polling. Reading target memory is a blocking USB
        // round-trip; doing it on the render thread (and contending with the
        // sampler on the shared J-Link lock) made the watch-list refresh rate
        // drag down the GUI frame rate. The worker thread performs the reads at
        // `updateIntervalMs_`; the render thread only consumes cached bytes.
        //
        // The shared state lives in a separately heap-allocated PollState owned
        // by a shared_ptr that the worker thread *also* holds. The worker never
        // touches `this`, so even in the unlikely event the thread outlives the
        // Variable (e.g. a stuck J-Link call delaying the join), it can only ever
        // dereference the still-alive PollState — never freed Variable memory.
        struct PollReq {
                u64 addr;
                u32 sz;
        };
        struct PollVal {
                u8   buf[8];
                u32  sz;
                bool ok;
        };
        struct PollState {
                std::mutex                       mtx;
                std::vector<PollReq>             reqs; // GUI → worker (what to read)
                std::unordered_map<u64, PollVal> vals; // worker → GUI (latest bytes)
                std::atomic<bool>                running{true};
                std::atomic<u32>                 intervalMs{200};
        };
        std::shared_ptr<PollState> poll_{};
        std::thread                pollThread_{};
        void                       startPollThread();
        void                       stopPollThread();

        // LOCAL port: variable-size in-process buffers keyed by variable name.
        // Buffers are pre-allocated (see drawAddVariableDialog OK handler) and
        // never resized after creation, so data() pointers remain stable for SDK threads.
        mutable std::mutex                                    mtxLocal_;
        std::unordered_map<std::string, std::vector<uint8_t>> localBufs_;
        std::unordered_map<std::string, std::vector<uint8_t>> shmShadowBufs_;
        std::unordered_map<u64, u64>                          memberRefreshTs_; // addr → last refresh ms

        bool                                         isModified_{false};
        std::unordered_map<std::string, std::string> memberValueCache_;
        std::unordered_map<u64, PollVal>             syncReadCache_;
        std::unordered_map<u64, u32>                 memberPollReqs_; // expanded J-Link leaves, addr -> size

        char                     searchBuf_[128]{};
        std::vector<SearchEntry> searchResults_;
        i32                      lastSelectedIndex_{-1};
        // Index of the row whose "=" grip is being dragged. Lets the "=" handle carry the
        // monitor (CHANNEL/STRUCT_CHANNEL) payload while still driving in-window reordering:
        // a drop target reorders when this is >= 0, and the symbol-browser "add" path ignores
        // such internal drags. -1 when no row grip drag is active.
        i32 rowDragSrc_{-1};
        // Parallel grip-drag state for struct sub-items (manual struct fields): packed
        // (parentVarIdx, fieldIdx) of the dragged field, or -1/-1 when inactive.
        i32         fieldDragParent_{-1};
        i32         fieldDragSrc_{-1};
        i32         enumEditIdx_{-1};
        bool        pendingDeleteFromSubVar_{false};
        i32         enumSubEditParentIdx_{-1};
        std::string enumSubEditMemberPath_;
        u64         enumSubEditMemberTypeOff_{0};

        struct {
                i32         parentIdx{-1};
                std::string path;
                char        alias[1024]{};
                char        addrBuf[32]{};
                DataType    type{DataType::UNKNOWN};
                bool        scalar{false};
                bool        writable{true};
                u64         defaultAddr{0};
                DataType    defaultType{DataType::UNKNOWN};
                bool        defaultWritable{true};
        } memberPropEdit_;

        i32 editPropIdx_{-1};
        struct {
                char                               name[1024]{};
                DataType                           type     = DataType::U32;
                PortType                           port     = PortType::JLINK;
                bool                               writable = true;
                char                               addrBuf[32]{};
                char                               udpIp[16]{};
                int                                udpPort = 8080;
                char                               shmName[64]{};
                int                                audioDeviceIndex{-1};
                char                               audioDeviceName[128]{};
                bool                               structMode{false};
                std::vector<VarEntry::StructField> structFields;
        } editPropBuf_;

        void rebuildSearchPool();
        void flattenDwarfType(std::vector<SearchEntry> &pool,
                              const dwarf::Info        &info,
                              const std::string        &parentPath,
                              u64                       parentAddr,
                              u64                       typeOff,
                              int                       depth,
                              u32                       bitOffset = 0,
                              u32                       bitSize   = 0);
        void flattenDataTree(std::vector<SearchEntry> &pool, const std::string &parentPath, const DataTree &node);

        enum class WindowState { None, LoadCfg, LoadBin, LoadElf, AddVariable };
        WindowState state_ = WindowState::None;
        bool        open_  = true;

        // Manual Variable Entry State
        struct {
                char                               name[1024]{};
                DataType                           type        = DataType::U32;
                PortType                           port        = PortType::JLINK;
                u64                                addr        = 0;
                bool                               writable    = true;
                char                               udpIp[16]   = "127.0.0.1";
                int                                udpPort     = 8080;
                char                               shmName[64] = "GlobalVariable";
                int                                audioDeviceIndex{-1};
                char                               audioDeviceName[128]{};
                char                               addrBuf[32] = "0"; // To avoid static persistence issues
                bool                               structMode  = false;
                std::vector<VarEntry::StructField> structFields;
        } newVar_;

      public:
        void save(void *node) const; // node is cJSON*
        void load(const void *node);
        bool isModified() const { return isModified_; }
        void clearModified() { isModified_ = false; }
        void
        addRecursive(const std::string &fullPath, u64 addr, u64 typeOff, PortType port, u32 bitOffset = 0, u32 bitSize = 0);
        bool watchHasName(const std::string &name) const; // true if a watch entry already uses this name
        void draw();
        void drawVarVarTreeRow(const std::string &name,
                               u64                addr,
                               u64                typeOff,
                               i32                depth,
                               PortType           port          = PortType::JLINK,
                               const std::string &shmRegionName = {},
                               i32                parentVarIdx  = -1);

        void drawSymbolBrowser();
        void drawSymbolTree();
        void drawSymbolLeaf(const std::string &displayName,
                            const std::string &fullPath,
                            u64                addr,
                            u64                typeOff,
                            i32                depth,
                            u32                bitOffset = 0,
                            u32                bitSize   = 0);
        void drawDataTreeLeaf(DataTree &node, const int indentLevel = 0);

        void drawVariableList();
        void drawAddVariableDialog();
        void drawEnumEditPopup();
        void drawSubEnumEditPopup();
        void drawEditPropertiesPopup();
        void drawMemberPropertiesPopup();
        void beginMemberProperties(
            i32 parentIdx, const std::string &path, u64 defaultAddr, DataType defaultType, bool scalar, bool defaultWritable);

        // Export / import the whole watch list to a .var file (JSON).
        void exportVarFile(const std::string &path);
        void importVarFile(const std::string &path);

        void handleDroppedFile(const std::string &path);

        // Port interaction
        void updateVariables();
        void writeVariable(const VarEntry &v, const std::string &newVal);

        // LOCAL port: returns a stable 8-byte buffer pointer for the named variable.
        // Creates the entry on first call. Thread-safe. Used by SDK sequences to pass
        // pointer arguments that write back into a Variable's local storage.
        void *getLocalBuf(const std::string &name);
        // Called after an SDK call writes to the buffer; nudges the display to refresh.
        void notifyLocalWrite(const std::string &name);
        // Programmatically add a LOCAL scalar variable (used by sequence editor "create output vars").
        // No-op if a variable with this name already exists.
        void addLocalVar(const std::string &name, DataType type, size_t bufSize);
        // Write a numeric value into a LOCAL scalar variable's buffer, encoding it
        // per the variable's DataType. No-op if `name` is not a LOCAL variable
        // (so a user's manually-configured non-LOCAL entry is left untouched).
        void setLocalScalar(const std::string &name, double value);
        // Programmatically add a LOCAL struct variable with predefined fields.
        // No-op if a variable with this name already exists.
        void addLocalStructVar(const std::string &name, const std::vector<VarEntry::StructField> &fields, size_t totalSize);
        // Read one LOCAL struct field as float. Returns false if var/field not found.
        bool readLocalFieldAsFloat(const std::string &varName, const std::string &fieldName, float &out) const;
        // Read a LOCAL scalar variable's current value as double. False if not a LOCAL scalar.
        bool readLocalScalar(const std::string &name, double &out) const;
        // Snapshot all LOCAL values as (channelName, value) pairs — "<var>" for a
        // scalar and "<var>.<field>" for each manual-struct field. The GUI feeds
        // these into matching scope channels each frame so LOCAL data plots live.
        std::vector<std::pair<std::string, float>> collectLocalChannelValues() const;

        // Popup support: immediate members of a DWARF struct VarEntry.
        // Pass typeOff=0/baseAddr=0 to start from the top-level variable.
        struct PopupMember {
                std::string name;
                std::string valStr; // cached display value, "..." if not yet read
                bool        isStruct{false};
                u64         typeOff{0};
                u64         addr{0};
        };
        std::vector<PopupMember>
        getPopupMembers(const std::string &varName, const std::string &pathPrefix, u64 typeOff = 0, u64 baseAddr = 0) const;
        // Read a DWARF struct member from the display-value cache as float.
        bool getDwarfMemberAsFloat(const std::string &memberPath, float &out) const;
        // Proactively refresh a DWARF member value via JLink (rate-limited ~100ms).
        // Populates memberValueCache_ so getDwarfMemberAsFloat returns up-to-date data.
        void refreshDwarfMember(const std::string &memberPath);

        // After ELF reload: refresh `ch`'s enum entries from this Variable's
        // current DWARF info + user overrides (enumDefs / memberEnumDefs).
        // Returns true if the channel's symbol path belongs to this Variable
        // (i.e. is present in searchPool_), false otherwise — callers should
        // try other Variable instances when false.
        bool refreshChannelEnums(MonitorChannel *ch);

      public:
        explicit Variable(std::string name) : name_(std::move(name)) { LOG_I("Variable Window Created: %s", name_.c_str()); }
        Variable() { LOG_I("Variable Window Created (default)"); };
        ~Variable()
        {
                LOG_I("Variable Window Destroyed: %s", name_.c_str());
                stopPollThread();
                if (currentLoadingTask_) {
                        currentLoadingTask_->aborted = true;
                }
        };

        bool loadCfg(const std::string &cfgPath);
        bool loadBin(const std::string &binPath);
        bool loadElf(const std::string &elfPath);

        void updateDisplay();

        // Add a dragged symbol to this window's watch list. Shared by the watch-list
        // drop target and the "mirror from a monitor drop" path. Both dedup by name.
        void addScalarToWatch(const ChannelDropPayload &p);
        void addStructToWatch(const StructChannelPayload &s);
        // Dispatch a queued mirror request (symbol-browser drag dropped onto a monitor).
        void mirrorFromMonitorDrop(const WatchMirrorRequest &req);

        const std::string &getName() const { return name_; }
        const std::string &getTitle() const { return title_.empty() ? name_ : title_; }
        void               setTitle(const std::string &t)
        {
                title_      = t;
                isModified_ = true;
        }
        const std::string &getCfgPath() const { return cfgPath_; }
        const std::string &getBinPath() const { return binPath_; }
        const std::string &getElfPath() const { return elfPath_; }
        bool               consumeElfReloaded()
        {
                bool r       = elfReloaded_;
                elfReloaded_ = false;
                return r;
        }
        bool consumePropertiesChanged()
        {
                bool expected = true;
                return propertiesChanged_.compare_exchange_strong(expected, false);
        }
        const ElfInfo     &getElfInfo() const { return elfInfo_; }
        bool               findElfSymbolAddress(const std::string &name, u32 &address) const;
        const dwarf::Info &getDwarfInfo() const { return dwarfInfo_; }
        std::string        resolveFunctionAddress(u32 address) const;
        bool               isPendingDelete() const { return !open_; }
};

#endif // !VARIABLE_HPP
