#ifndef VARIABLE_HPP
#define VARIABLE_HPP

#include <string>
#include <utility>
#include <variant>
#include <vector>
#include "timeops.h"
#include <filesystem>
#include <mutex>
#include <atomic>
#include <thread>
#include <unordered_map>

#include "module.h"
#include "app_log.hpp"
#include "core/dwarf_parser.hpp"
#include "core/elf_parser.hpp"
#include "core/json_parser.hpp"
#include "core/bin_parser.hpp"
#include "core/parser.hpp"

enum class PortType {
    JLINK,
    UDP,
    SHM,
    MANUAL
};

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
        u16 port;
    } udp{};
    struct {
        char name[64];
        shm_t handle;
        bool  inited = false;
    } shm{};

    u64  typeOff = 0; // For DWARF nested display
    bool selected = false;
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
        std::string name_{};
        std::string cfgPath_{}, binPath_{}, elfPath_{};
        
      public:
        // The list of active variables being monitored/modified
        std::vector<VarEntry> vars_;
        // Flattened symbols for global search
        std::vector<SearchEntry> searchPool_;

      private:

        i32 toastDismissTime_{2000};

        ElfInfo        elfInfo_{};
        dwarf::Info    dwarfInfo_{};
        char           elfFilter_[128]{};
        bool           elfFilterObjectsOnly_{true};
        i32            elfArrayMaxElems_{64};
        std::filesystem::file_time_type elfLastWriteTime_{};
        bool                            elfReloaded_{false};
        std::atomic<bool>               isElfLoading_{false};
        mutable std::recursive_mutex    mtxElf_{};
        std::shared_ptr<ElfLoadingTask> currentLoadingTask_;

        // For JSON/BIN tree display
        DataTree dataTree_;

        f32 watchListHeight_ = 300.0f;

        u64                      lastUpdateTs_{0};
        u32                      updateIntervalMs_{200}; 
        bool                     isModified_{false};
        std::unordered_map<u64, std::string> memberValueCache_;
        char                     searchBuf_[128]{};
        std::vector<SearchEntry> searchResults_;
        i32                      lastSelectedIndex_{-1};

        void rebuildSearchPool();
        void flattenDwarfType(std::vector<SearchEntry> &pool, const dwarf::Info &info, const std::string &parentPath, u64 parentAddr, u64 typeOff, int depth);
        void flattenDataTree(std::vector<SearchEntry> &pool, const std::string &parentPath, const DataTree &node);

        enum class WindowState {
            None,
            LoadCfg,
            LoadBin,
            LoadElf,
            AddVariable
        };
        WindowState state_ = WindowState::None;
        bool open_ = true;

        // Manual Variable Entry State
        struct {
            char name[64]{};
            DataType type = DataType::U32;
            PortType port = PortType::JLINK;
            u64 addr = 0;
            bool writable = true;
            char udpIp[16] = "127.0.0.1";
            int  udpPort = 8080;
            char shmName[64] = "GlobalVariable";
            char addrBuf[32] = "0"; // To avoid static persistence issues
        } newVar_;

      public:
        void               save(void *node) const; // node is cJSON*
        void               load(const void *node);
        bool               isModified() const { return isModified_; }
        void               clearModified() { isModified_ = false; }
        void               addRecursive(const std::string &fullPath, u64 addr, u64 typeOff, PortType port);
        void               draw();
        void               drawVarVarTreeRow(const std::string &name, u64 addr, u64 typeOff, i32 depth, PortType port = PortType::JLINK, const std::string &shmRegionName = {});

        void               drawSymbolBrowser();
        void               drawSymbolTree();
        void               drawSymbolLeaf(const std::string &displayName, const std::string &fullPath, u64 addr, u64 typeOff, i32 depth);
        void               drawDataTreeLeaf(DataTree &node, const int indentLevel = 0);
        
        void               drawVariableList();
        void               drawAddVariableDialog();

        void handleDroppedFile(const std::string &path);

        // Port interaction
        void updateVariables();
        void writeVariable(const VarEntry &v, const std::string &newVal);

      public:
        explicit Variable(std::string name) : name_(std::move(name)) { LOG_I("Variable Window Created: %s", name_.c_str()); }
        Variable() { LOG_I("Variable Window Created (default)"); };
        ~Variable() { 
            LOG_I("Variable Window Destroyed: %s", name_.c_str()); 
            if (currentLoadingTask_) {
                currentLoadingTask_->aborted = true;
            }
        };

        bool loadCfg(const std::string &cfgPath);
        bool loadBin(const std::string &binPath);
        bool loadElf(const std::string &elfPath);

        void updateDisplay();

        const std::string &getName() const { return name_; }
        const std::string &getCfgPath() const { return cfgPath_; }
        const std::string &getBinPath() const { return binPath_; }
        const std::string &getElfPath() const { return elfPath_; }
        bool               consumeElfReloaded() { bool r = elfReloaded_; elfReloaded_ = false; return r; }
        const ElfInfo     &getElfInfo() const { return elfInfo_; }
        const dwarf::Info &getDwarfInfo() const { return dwarfInfo_; }
        bool               isPendingDelete() const { return !open_; }
};

#endif // !VARIABLE_HPP
