#ifndef VARIABLE_HPP
#define VARIABLE_HPP

#include "timeops.h"
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

enum class PortType { JLINK, UDP, SHM, MANUAL };

struct VarEntry {
        std::string name;
        DataType    type;
        PortType    port;
        u64         addr;     // address or offset
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

        u64  typeOff  = 0; // For DWARF nested display
        bool selected = false;

        // User-editable enum label definitions (overrides DWARF when non-empty)
        struct EnumDef {
                std::string name;
                i64         value;
        };
        std::vector<EnumDef> enumDefs;
        // Per-member enum overrides for struct/array sub-variables (keyed by full member path)
        std::unordered_map<std::string, std::vector<EnumDef>> memberEnumDefs;
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
        std::atomic<bool>               isElfLoading_{false};
        mutable std::mutex              mtxElf_{};
        std::shared_ptr<ElfLoadingTask> currentLoadingTask_;

        // For JSON/BIN tree display
        DataTree dataTree_;

        f32 watchListHeight_ = 300.0f;

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

        bool                                 isModified_{false};
        std::unordered_map<u64, std::string> memberValueCache_;
        char                                 searchBuf_[128]{};
        std::vector<SearchEntry>             searchResults_;
        i32                                  lastSelectedIndex_{-1};
        i32                                  enumEditIdx_{-1};
        bool                                 pendingDeleteFromSubVar_{false};
        i32                                  enumSubEditParentIdx_{-1};
        std::string                          enumSubEditMemberPath_;
        u64                                  enumSubEditMemberTypeOff_{0};

        void rebuildSearchPool();
        void flattenDwarfType(std::vector<SearchEntry> &pool,
                              const dwarf::Info        &info,
                              const std::string        &parentPath,
                              u64                       parentAddr,
                              u64                       typeOff,
                              int                       depth);
        void flattenDataTree(std::vector<SearchEntry> &pool, const std::string &parentPath, const DataTree &node);

        enum class WindowState { None, LoadCfg, LoadBin, LoadElf, AddVariable };
        WindowState state_ = WindowState::None;
        bool        open_  = true;

        // Manual Variable Entry State
        struct {
                char     name[64]{};
                DataType type        = DataType::U32;
                PortType port        = PortType::JLINK;
                u64      addr        = 0;
                bool     writable    = true;
                char     udpIp[16]   = "127.0.0.1";
                int      udpPort     = 8080;
                char     shmName[64] = "GlobalVariable";
                char     addrBuf[32] = "0"; // To avoid static persistence issues
        } newVar_;

      public:
        void save(void *node) const; // node is cJSON*
        void load(const void *node);
        bool isModified() const { return isModified_; }
        void clearModified() { isModified_ = false; }
        void addRecursive(const std::string &fullPath, u64 addr, u64 typeOff, PortType port);
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
        void drawSymbolLeaf(const std::string &displayName, const std::string &fullPath, u64 addr, u64 typeOff, i32 depth);
        void drawDataTreeLeaf(DataTree &node, const int indentLevel = 0);

        void drawVariableList();
        void drawAddVariableDialog();
        void drawEnumEditPopup();
        void drawSubEnumEditPopup();

        void handleDroppedFile(const std::string &path);

        // Port interaction
        void updateVariables();
        void writeVariable(const VarEntry &v, const std::string &newVal);

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
        const ElfInfo     &getElfInfo() const { return elfInfo_; }
        const dwarf::Info &getDwarfInfo() const { return dwarfInfo_; }
        bool               isPendingDelete() const { return !open_; }
};

#endif // !VARIABLE_HPP
