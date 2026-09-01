#include "core/firmware_identity.hpp"
#include "core/firmware_match.hpp"
#include "core/parameter_csv.hpp"
#include "core/parameter_file.hpp"
#include "core/parameter_json.hpp"
#include "core/parameter_tree.hpp"
#include "gui/hover_focus.hpp"
#include "gui/hover_reveal.hpp"
#include "gui/interaction_history.hpp"
#include "gui/value_flash.hpp"
#ifndef VARIABLE_HPP
#define VARIABLE_HPP

#include "timeops.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "app_log.hpp"
#include "ckit.h"
#include "core/bin_parser.hpp"
#include "core/c_header_parser.hpp"
#include "core/diagnostic_value.hpp"
#include "core/dwarf_parser.hpp"
#include "core/elf_parser.hpp"
#include "core/flash_array.hpp"
#include "core/json_parser.hpp"
#include "core/parser.hpp"
#include "core/sdk_loader.hpp"
#include "core/unit_conversion.hpp"
#include "gui/remote_files.hpp"

class MonitorChannel;
struct ChannelDropPayload;
struct StructChannelPayload;
struct WatchMirrorRequest;

// Persisted numeric values stay stable. Value 3 used to mean MANUAL and is
// migrated to LOCAL when old sessions or .var files are loaded
enum class PortType { JLINK = 0, UDP = 1, SHM = 2, LOCAL = 4, AUDIO = 5 };

struct VarEntry {
        struct FunctionBinding {
                std::string sourcePath;
                std::string functionName;
                std::string signature;
                bool        empty() const { return functionName.empty(); }
        };

        std::shared_ptr<const flasharray::Snapshot> flashArray;
        std::string                                 name;
        DataType                                    type;
        PortType                                    port;
        u64                                         addr; // address or offset
        bool                                        isStruct{false};
        bool                                        isUnion{false};
        bool                                        isArray{false};
        u32                                         bitOffset{0};
        u32                                         bitSize{0};
        int                                         displayRadix{10};
        std::unordered_map<std::string, int>        memberDisplayRadices;
        int                                         radixForPath(const std::string &path = {}) const
        {
                int    radix = displayRadix == 2 || displayRadix == 8 || displayRadix == 16 ? displayRadix : 10;
                size_t match = 0;
                for (const auto &[prefix, value] : memberDisplayRadices)
                        if (prefix.size() >= match &&
                            (path == prefix || (path.size() > prefix.size() && path.starts_with(prefix) &&
                                                (path[prefix.size()] == '.' || path[prefix.size()] == '[')))) {
                                radix = value;
                                match = prefix.size();
                        }
                return radix;
        }
        unit_conversion::Settings                                  unitConversion;
        std::unordered_map<std::string, unit_conversion::Settings> memberUnitConversions;
        // Runtime-only numeric snapshot of the displayed scalar; never a write value.
        std::optional<double> displayNumericValue;
        std::array<u8, 8>     displayBytes{};
        u32                   displayByteSize{0};

        std::string typeStr;          // Full type description (e.g. "struct my_struct")
        std::string valueStr;         // Current value as string for display
        std::string prevGoodValueStr; // Runtime cache retained across J-Link disconnects
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

        // Optional C accessors for LOCAL scalar variables. Stable names and
        // signatures are stored instead of process-local function pointers
        FunctionBinding readFunction;
        FunctionBinding writeFunction;
        std::string     functionError; // runtime-only status
        std::string     readFunctionError;
        std::string     writeFunctionError;

        // User-editable enum label definitions (overrides DWARF when non-empty)
        struct EnumDef {
                std::string name;
                i64         val;
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
        // DWARF path. The canonical key remains unchanged for ELF re-resolution
        std::unordered_map<std::string, MemberOverride> memberOverrides;
        // Paths of sub-variables hidden by the user (right-click → Delete)
        std::set<std::string> hiddenMembers;
        // Paths of struct/array nodes the user has expanded; persisted so the
        // tree restores its open state across sessions. Key = full member path
        // (same format as hiddenMembers / memberEnumDefs). The top-level row
        // is keyed by `name`
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
        friend struct VariableRewriteRegression;

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

        struct RewriteState {
                bool                                                   open{};
                bool                                                   requestOpen{};
                bool                                                   isFocCfg{};
                bool                                                   guardedFoc{};
                bool                                                   pending{};
                bool                                                   changedOnly{};
                int                                                    expandMode{-1};
                VarEntry                                               source{};
                std::string                                            ownerName;
                u64                                                    ownerAddress{};
                dwarf::Info                                            info; // immutable type snapshot for this editing session
                std::vector<u8>                                        original;
                std::unordered_map<std::string, int>                   arrayPages;
                std::unordered_map<std::string, parameter_csv::Row>    csvRows;
                std::unordered_map<std::string, parameter_json::Field> jsonFields;
                std::string                                            profileName;
                std::vector<u8>                                        nameComparedBytes;
                u64                                                    address{};
                u64                                                    typeOff{};
                u64                                                    stateAddress{};
                u32                                                    stateSize{};
                u64                                                    targetEpoch{};
                u64                                                    layoutGeneration{};
                bool                                                   targetLoaded{};
                int                                                    preset{-1};
                char                                                   search[128]{};
                std::vector<u8>                                        bytes;
                std::string                                            variableName;
                std::string                                            status;
                bool                                                   statusOk{};
        } rewrite_;

        ElfInfo                         elfInfo_{};
        dwarf::Info                     dwarfInfo_{};
        u64                             elfLayoutGeneration_{};
        char                            elfFilter_[128]{};
        bool                            elfFilterObjectsOnly_{true};
        i32                             elfArrayMaxElems_{64};
        std::filesystem::file_time_type elfLastWriteTime_{};
        bool                            elfReloaded_{false};
        std::atomic<bool>               propertiesChanged_{false};
        std::atomic<bool>               isElfLoading_{false};
        mutable std::mutex              mtxElf_{};
        std::shared_ptr<ElfLoadingTask> currentLoadingTask_;
        std::thread                     elfLoadThread_;
        // The loader only publishes a completed result; live symbols and watch
        // rows belong to the GUI thread, including during automatic reloads
        struct ElfLoadResult {
                std::optional<firmware_match::Image> buildImage;
                ElfInfo                              info;
                dwarf::Info                          dwarf;
                std::vector<SearchEntry>             pool;
                std::string                          localPath;
                std::string                          error;
                std::filesystem::file_time_type      writeTime{};
        };
        std::unique_ptr<ElfLoadResult>       pendingElf_;       // guarded by mtxElf_
        void                                 applyPendingElf(); // GUI thread, before reading/drawing symbols
        std::string                          elfLocalPath_;     // guarded by mtxElf_
        std::string                          elfLoadError_;     // guarded by mtxElf_
        std::optional<firmware_match::Image> firmwareBuildImage_;
        std::string                          firmwareSearchId_;
        std::vector<std::string>             firmwareMatches_;
        void                                 unregisterFirmwareSource();
        void                                 drawFirmwareStatus();
        RemoteFileMenu                       remoteSymbols_;

        // For JSON/BIN tree display
        DataTree dataTree_;

        ui::HoverReveal symbolBrowserHover_;
        ui::HoverReveal watchSearchHover_;
        char            watchSearchText_[128]{};
        std::string     watchSearchQuery_, watchSearchSelectedOwner_, watchSearchSelectedPath_;
        size_t          watchSearchSignature_{};
        bool            watchSearchLimited_{false}, watchSearchScroll_{false};
        struct WatchSearchResult {
                std::string              owner, path;
                PortType                 port;
                u64                      address;
                std::vector<std::string> ancestors;
        };
        std::vector<WatchSearchResult> watchSearchResults_;
        void                           drawWatchSearch(bool activated);
        void                           markWatchSearchRow(int owner, const std::string &path);

        struct TableUiState {
                int                  sortColumn{0};
                int                  sortDirection{1}; // ImGuiSortDirection_Ascending
                std::array<float, 6> widths{};
                bool                 restoreWidths{false};
                bool                 restoreSort{false};
                bool                 initialized{false};
        };
        TableUiState watchTableUi_{};
        TableUiState symbolTableUi_{};
        TableUiState binTableUi_{};

        // Temporary view only. Keep canonical paths so the original watch entry
        // continues to own values, overrides and expansion state
        struct WatchFocusNode {
                std::string path, label;
                u64         addr{}, typeOff{};
        };
        struct WatchFocus {
                std::string owner;
                PortType    port{PortType::JLINK};
                std::string path;
                u64         ownerAddress{};
        };
        WatchFocus                 watchFocus_;
        ui::HoverFocus<WatchFocus> watchFocusPreview_;
        ui::ValueFlash             valueFlash_;
        void                       drawWatchFocusButton(int parent, const std::string &path);
        enum class PointerRead { Ready, Waiting, Error, Disconnected, Unsupported };
        struct PointerView {
                bool                                                 open{};
                std::string                                          owner, path;
                PortType                                             port{PortType::JLINK};
                u64                                                  ownerAddress{}, address{}, typeOff{}, generation{};
                std::unordered_map<std::string, std::pair<u64, u64>> targets;
        } pointerView_;
        PointerRead readPointerMemory(const VarEntry &owner, u64 address, u32 size, u8 *bytes, u64 notBefore = 0);
        void        drawPointerButton(int owner, u64 address, u64 typeOff, const std::string &path);
        void        drawPointerInspector();
        bool        watchFocusScrollReset_{false};
        void        focusWatch(int parent, const std::string &path);
        int         resolveWatchFocus(std::vector<WatchFocusNode> &nodes) const;
        void        drawWatchFocusNavigation();
        bool        drawWatchFocusTable();

        void restoreTableWidths(TableUiState &state, int columnCount);
        void restoreTableSort(TableUiState &state);
        void captureTableWidths(TableUiState &state, int columnCount);
        void captureTableSort(TableUiState &state, int column, int dir);

        u64                          prevUpdateTs_{0};
        u32                          updateIntervalMs_{200};
        int                          displayRadix_{10};
        int                          variableRadix(const VarEntry &v) const { return v.radixForPath(); }
        void                         drawWatchValueActions(int index, const std::string &path);
        std::string                  watchClipboardText(int index, const std::string &path, bool *waiting = nullptr);
        void                         requestWatchCopy(int index, const std::string &path);
        void                         updateWatchCopy();
        bool                         readWatchBytes(const VarEntry &owner, u64 address, DataType type, u8 *bytes) const;
        std::string                  pendingCopyOwner_, pendingCopyPath_;
        u64                          pendingCopyDeadline_{0};
        u64                          pendingCopyNextPoll_{0};
        std::unordered_map<u64, u32> pendingCopyRequests_;

        // Background J-Link polling. Reading target memory is a blocking USB
        // round-trip; doing it on the render thread (and contending with the
        // sampler on the shared J-Link lock) made the watch-list refresh rate
        // drag down the GUI frame rate. The worker thread performs the reads at
        // `updateIntervalMs_`; the render thread only consumes cached bytes
        //
        // The shared state lives in a separately heap-allocated PollState owned
        // by a shared_ptr that the worker thread *also* holds. The worker never
        // touches `this`, so even in the unlikely event the thread outlives the
        // Variable (e.g. a stuck J-Link call delaying the join), it can only ever
        // dereference the still-alive PollState — never freed Variable memory
        struct PollReq {
                u64 epoch{};
                u64 addr;
                u32 sz;
        };
        struct PollVal {
                u8   buf[8];
                u32  sz;
                bool ok;
                u64  epoch{}, timestamp{};
        };
        struct PollState {
                std::mutex                       mtx;
                std::vector<PollReq>             reqs; // GUI → worker (what to read)
                std::unordered_map<u64, PollVal> vals; // worker → GUI (latest bytes)
                std::atomic<bool>                running{true};
                std::atomic<u32>                 intervalMs{200};
                std::atomic<u64>                 sweepMs{0}; // 最近一轮读取耗时, 用于判断缓存是否过期
        };
        std::shared_ptr<PollState> poll_{};
        std::thread                pollThread_{};
        void                       startPollThread();
        void                       stopPollThread();

        struct FunctionSource {
                std::string libraryPath;
                std::string headerPath;
                SdkLoader   loader;
                ParseResult declarations;
                std::string status;
                bool        statusError{false};
                bool        statusWarning{false};
        };
        struct AccessReadReq {
                std::string     name;
                DataType        type{DataType::UNKNOWN};
                CFuncDecl       function;
                FunctionSource *source{nullptr};
        };
        struct AccessWriteReq {
                std::string            name;
                DataType               type{DataType::UNKNOWN};
                CFuncDecl              function;
                FunctionSource        *source{nullptr};
                std::array<uint8_t, 8> val{};
                u32                    size{0};
                std::string            valueText;
        };
        struct AccessResult {
                std::array<uint8_t, 8> val{};
                u32                    size{0};
                bool                   hasValue{false};
                bool                   ok{false};
                std::string            error;
        };
        struct AccessState {
                std::mutex                                    mtx;
                std::condition_variable                       cv;
                std::vector<AccessReadReq>                    reads;
                std::deque<AccessWriteReq>                    writes;
                std::unordered_map<std::string, AccessResult> results;
                std::unordered_map<std::string, AccessResult> writeResults;
                std::atomic<u32>                              intervalUs{200000};
                bool                                          readsPending{false};
                bool                                          running{true};
        };
        std::shared_ptr<AccessState>                 accessor_{};
        std::thread                                  accessorThread_{};
        std::atomic<u32>                             monitorFunctionHz_{0};
        std::mutex                                   sdkCallMtx_{};
        std::vector<std::unique_ptr<FunctionSource>> functionSources_{};
        bool                                         functionPanelOpen_{false};
        i32                                          functionBindIdx_{-1};
        void                                         startAccessorThread();
        void                                         stopAccessorThread();
        bool                                         loadFunctionLibrary(const std::string &path);
        bool                                         parseFunctionHeader(const std::string &path);
        void                                         drawFunctionPanel();
        void                                         drawFunctionBindingPopup();
        const CFuncDecl   *findFunction(const VarEntry::FunctionBinding &binding, FunctionSource **source = nullptr) const;
        FunctionSource    *sourceForLibrary(const std::string &path);
        FunctionSource    *sourceForHeader(const std::string &path);
        static std::string functionSourceKey(const FunctionSource &source);

        // LOCAL port: variable-size in-process buffers keyed by variable name
        // Buffers are pre-allocated (see drawAddVariableDialog OK handler) and
        // never resized after creation, so data() pointers remain stable for SDK threads
        mutable std::mutex                                    mtxLocal_;
        std::unordered_map<std::string, std::vector<uint8_t>> localBufs_;
        std::set<std::string>                                 localSampleSources_;
        struct LocalSample {
                std::string          name;
                std::vector<uint8_t> bytes;
                double               time{};
                u64                  epoch{};
        };
        std::deque<LocalSample>                               localSamples_;
        size_t                                                localSampleBytes_{};
        std::unordered_map<std::string, std::vector<uint8_t>> shmShadowBufs_;
        std::unordered_map<u64, u64>                          memberRefreshTs_; // addr → prev refresh ms

        bool                                         isModified_{false};
        std::unordered_map<std::string, std::string> memberValueCache_;
        std::unordered_map<std::string, double>      memberNumericValueCache_;
        std::unordered_map<u64, PollVal>             syncReadCache_;
        std::unordered_map<u64, u32>                 memberPollReqs_; // expanded J-Link leaves, addr -> size
        std::optional<double>
        unitConversionMemberValue(const VarEntry &owner, u64 address, DataType type, u32 bitOffset = 0, u32 bitSize = 0) const;
        bool                      unitConversionEditRequested_{false};
        std::string               unitConversionEditOwner_, unitConversionEditPath_;
        unit_conversion::Settings unitConversionEditDraft_;
        void                      drawUnitConversionEditor();
        struct DiagnosticRequest {
                u32 size;
                u64 touched;
        };
        std::unordered_map<u64, DiagnosticRequest> diagnosticPollReqs_;

        char                     searchBuf_[128]{};
        std::vector<SearchEntry> searchResults_;
        std::string              searchAppliedQuery_;
        u64                      searchPoolGeneration_{};
        u64                      searchAppliedGeneration_{~u64{0}};
        i32                      prevSelectedIndex_{-1};
        // Index of the row whose "=" grip is being dragged. Lets the "=" handle carry the
        // monitor (CHANNEL/STRUCT_CHANNEL) payload while still driving in-window reordering:
        // a drop target reorders when this is >= 0, and the symbol-browser "add" path ignores
        // such internal drags. -1 when no row grip drag is active
        i32 rowDragSrc_{-1};
        // Parallel grip-drag state for struct sub-items (manual struct fields): packed
        // (parentVarIdx, fieldIdx) of the dragged field, or -1/-1 when inactive
        i32         fieldDragParent_{-1};
        i32         fieldDragSrc_{-1};
        i32         enumEditIdx_{-1};
        bool        pendingDeleteFromSubVar_{false};
        i32         enumSubEditParentIdx_{-1};
        std::string enumSubEditMemberPath_;
        u64         enumSubEditMemberTypeOff_{0};

        struct {
                i32             parentIdx{-1};
                std::string     path;
                u64             defaultAddr{};
                dwarf::TypeKind aggregateKind{dwarf::TypeKind::UNKNOWN};
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
                bool                               unionMode{false};
                bool                               arrayMode{false};
                bool                               typeChanged{false};
                std::vector<VarEntry::StructField> structFields;
        } editPropBuf_;

        void rebuildSearchPool();
        bool addSymbolToWatch(
            const std::string &fullPath, u64 address, u64 typeOffset, PortType port, u32 bitOffset = 0, u32 bitSize = 0);
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
                bool                               unionMode   = false;
                bool                               arrayMode   = false;
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
        bool watchHasIdentity(const std::string &name, u64 address, DataType type) const;
        void openParameterEditor()
        {
                isModified_           = true;
                parameterReadPending_ = true;
                parameterFileOpen_ = parameterFileRequestOpen_ = true;
        }
        void draw();
        void drawVarVarTreeRow(const std::string &name,
                               u64                addr,
                               u64                typeOff,
                               i32                depth,
                               PortType           port          = PortType::JLINK,
                               const std::string &shmRegionName = {},
                               i32                parentVarIdx  = -1,
                               i32                colorDepth    = 0);

        bool refreshSymbolSearch();
        void drawSymbolBrowserPanel(bool activated);
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

        void                                    drawVariableList();
        void                                    addFlashArray(std::shared_ptr<const flasharray::Snapshot> snapshot);
        bool                                    drawFlashArrayRow(int index, int &moveSrc, int &moveDst);
        static void                             restoreFlashArray(VarEntry &entry, const cJSON *node);
        void                                    drawAddVariableDialog();
        void                                    drawEnumEditPopup();
        void                                    drawSubEnumEditPopup();
        void                                    drawEditPropertiesPopup();
        void                                    openRewrite(const VarEntry &variable);
        void                                    openRewriteMember(int                parent,
                                                                  u64                addr,
                                                                  u64                typeOff,
                                                                  const std::string &name,
                                                                  DataType           overrideType = DataType::UNKNOWN,
                                                                  u32                bitOffset    = 0,
                                                                  u32                bitSize      = 0);
        void                                    prepareRewrite();
        bool                                    readRewrite(std::vector<u8> &bytes);
        const firmware_identity::Image         &parameterFirmwareIdentity();
        void                                    validateParameterManifest();
        void                                    verifyRewriteFirmware(bool forWrite);
        std::optional<firmware_identity::Image> parameterFirmwareImage_;
        u64                                     parameterFirmwareGeneration_{UINT64_MAX};
        bool                                    writeRewrite(const std::vector<u8> &bytes, const std::vector<u8> &masks);
        void                                    reloadRewrite(bool preserveEdits = false);
        void                                    drawRewriteDialog();
        std::string                             exportRewriteCsv();
        bool                                    importRewriteCsv(const std::string &content);
        bool                                    validateRewriteCsv();
        void
        drawRewriteNode(u64 typeOff, size_t offset, const std::string &path, int depth, u32 bitOffset = 0, u32 bitSize = 0);
        bool                            importRewriteJson(const std::string &content);
        std::string                     exportRewriteJson(std::vector<u8> *candidate = nullptr);
        bool                            syncRewriteName();
        void                            addParameterBaselines(parameter_json::Document &document);
        const parameter_json::Document *parameterPreset(const std::string &name);
        void                            ensureParameterCache();
        std::filesystem::path           parameterSchemaDirectory() const;
        std::filesystem::path           parameterCacheDirectory_, parameterCacheSchemaDirectory_;
        std::filesystem::file_time_type parameterCacheSchemaTime_{}, parameterCacheManifestTime_{};
        std::filesystem::file_time_type parameterCacheDirectoryTime_{}, parameterCacheSchemaDirectoryTime_{};
        std::unordered_map<std::string, parameter_json::Document>        parameterPresetCache_;
        std::set<std::string>                                            parameterMissingPresets_;
        std::string                                                      parameterManifestCache_;
        std::unordered_map<std::string, std::filesystem::file_time_type> parameterPresetTimes_;
        const std::vector<std::filesystem::path>                        &parameterPresetFiles();
        std::vector<std::filesystem::path>                               parameterFileList_;
        std::filesystem::path                                            parameterFileListDirectory_;
        std::chrono::steady_clock::time_point                            parameterFileListChecked_{};
        bool                                                             loadParameterFile(const std::string &path);
        bool selectParameterComponent(const std::string &section, const std::string &path);
        void openParameterComposition();
        void drawParameterComponents();
        /**
         * @brief 设置参数 JSON 对应的目标变量, 切换后清除旧读数
         */
        void                                         setParameterTarget(const std::string &target);
        bool                                         prepareParameterDevice(const std::string &target);
        bool                                         updateParameterDevice(bool apply);
        bool                                         commitRewrite();
        RewriteState                                 parameterDevice_;
        std::unordered_map<std::string, std::string> parameterDeviceValues_;
        void                                         drawParameterFileEditor();
        parameter_json::FileEditor                   parameterFile_;
        ui::HoverReveal                              parameterPresetHover_;
        ui::HoverReveal                              parameterTargetHover_;
        char                                         parameterTargetSearch_[128]{};
        void                                         drawParameterTargetPicker();
        using ParameterDraft = std::vector<std::pair<std::string, std::string>>;
        ui::interaction::UndoHistory<ParameterDraft> parameterHistory_;
        std::optional<ParameterDraft>                parameterEditBefore_;
        ImGuiID                                      parameterEditActive_{};
        ui::interaction::Action                      parameterAction_      = ui::interaction::Action::None;
        int                                          parameterNavigateTab_ = -1;
        ParameterDraft                               captureParameterDraft() const;
        void                                         restoreParameterDraft(const ParameterDraft &draft);
        void                                         clearParameterHistory();
        bool                                         parameterFileOpen_ = false, parameterFileRequestOpen_ = false;
        bool                                         parameterFileVisible_     = false;
        bool                                         parameterReadPending_     = false;
        bool                                         parameterFileCombination_ = false;
        std::string                                  parameterFilePath_, parameterFileStatus_;
        bool                                         parameterFileStatusError_   = false;
        bool                                         parameterFileStatusSuccess_ = false;
        void setParameterFileStatus(std::string message, bool error = false, bool success = false)
        {
                parameterFileStatus_        = std::move(message);
                parameterFileStatusError_   = error;
                parameterFileStatusSuccess_ = success && !error;
        }
        char                  parameterTarget_[512]{};
        std::string           parameterTargetSource_, parameterTargetOverride_;
        std::string           parameterSearchSelected_;
        bool                  parameterSearchScroll_ = false;
        char                  parameterFileSearch_[256]{};
        int                   parameterComponentTab_ = 0;
        int                   parameterTreeExpand_   = -1;
        std::filesystem::path parameterDirectory_;
        void                  beginMemberProperties(i32                parentIdx,
                                                    const std::string &path,
                                                    u64                defaultAddr,
                                                    DataType           defaultType,
                                                    bool               scalar,
                                                    bool               defaultWritable,
                                                    dwarf::TypeKind    kind = dwarf::TypeKind::STRUCT);

        /**
         * @brief Export / import the whole watch list to a .var file (JSON)
         */
        void exportVarFile(const std::string &path);
        void importVarFile(const std::string &path);

        void handleDroppedFile(const std::string &path);

        // Port interaction
        void updateVariables();
        void writeVariable(const VarEntry &v, const std::string &newVal);

        /**
         * @brief LOCAL port: returns a stable 8-byte buffer pointer for the named variable
         * Creates the entry on first call. Thread-safe. Used by SDK sequences to pass
         * pointer arguments that write back into a Variable's local storage
         */
        void *getLocalBuf(const std::string &name);
        /**
         * @brief Called after an SDK call writes to the buffer; nudges the display to refresh
         */
        void notifyLocalWrite(const std::string &name);
        /**
         * @brief Typed byte snapshots and conditional publication for bidirectional SDK tables
         */
        std::unordered_map<std::string, std::vector<uint8_t>> snapshotLocalBuffers() const;
        bool                                                  compareExchangeLocalBuffer(const std::string          &name,
                                                                                         const std::vector<uint8_t> &expected,
                                                                                         const std::vector<uint8_t> &desired);
        /**
         * @brief Programmatically add a LOCAL scalar variable (used by sequence editor "create output vars")
         * No-op if a variable with this name already exists
         */
        void addLocalVar(const std::string &name, DataType type, size_t bufSize);
        /**
         * @brief Write a numeric value into a LOCAL scalar variable's buffer, encoding it
         * per the variable's DataType. No-op if `name` is not a LOCAL variable
         * (so a user's manually-configured non-LOCAL entry is left untouched)
         */
        void setLocalScalar(const std::string &name, double val);
        /**
         * @brief Apply a Monitor TABLE edit to a LOCAL scalar and invoke its optional
         * DLL write accessor through the same path as the variable table
         */
        bool writeLocalScalarFromMonitor(const std::string &name, double val);
        bool openFunctionBindingFor(const std::string &name);
        /**
         * @brief Programmatically add a LOCAL struct variable with predefined fields
         * No-op if a variable with this name already exists
         */
        void addLocalStructVar(const std::string &name, const std::vector<VarEntry::StructField> &fields, size_t totalSize);
        /**
         * @brief Read one LOCAL struct field as float. Returns false if var/field not found
         */
        bool readLocalFieldAsFloat(const std::string &varName, const std::string &fieldName, float &out) const;
        /**
         * @brief Read a LOCAL scalar variable's current value as double. False if not a LOCAL scalar
         */
        bool readLocalScalar(const std::string &name, double &out) const;
        struct RawSample {
                std::array<u8, 8> bytes{};
                u32               size{};
        };
        bool readLocalChannelBytes(const std::string &name, RawSample &out) const;
        /**
         * @brief GUI-thread read-only diagnostics; resolve current symbols and reject stale target samples
         */
        bool resolveDiagnosticVariable(const std::string &name, ChannelDropPayload &out) const;
        bool readDiagnosticWord(const std::string &name, u32 &word, std::string &status);
        bool hasDiagnosticWord(const std::string &name) const;
        struct DiagnosticPath {
                u64              address{};
                std::vector<u64> pointerOffsets;
                u32              size{}, bitOffset{}, bitSize{};
        };
        bool resolveDiagnosticPath(const std::string &name, DiagnosticPath &path) const;
        bool readDiagnosticMemory(u64 address, u32 size, u32 &word, std::string &status);
        /**
         * @brief Expand one configured object; unsupported types and limits remain visible as status rows
         */
        std::vector<diagnostics::Field> diagnosticFields(const std::string &root) const;
        bool readDiagnosticValue(const std::string &name, diagnostics::Value &val, std::string &status);
        u64  diagnosticLayoutGeneration() const { return elfLayoutGeneration_; }
        /**
         * @brief Snapshot all LOCAL values as (chName, value) pairs — "<var>" for a
         * scalar and "<var>.<field>" for each manual-struct field. The GUI feeds
         * these into matching scope chs each frame so LOCAL data plots live
         */
        std::vector<std::pair<std::string, float>> collectLocalChannelValues() const;
        struct LocalSampleBatch {
                std::vector<float>  values;
                std::vector<double> times;
                RawSample           raw;
        };
        void markLocalSampleSource(const std::string &name);
        void publishLocalSample(const std::string &name, const std::vector<uint8_t> &bytes, double time);
        std::unordered_map<std::string, LocalSampleBatch> drainLocalSamples();
        /**
         * @brief Select the fastest monitor rate that references a bound LOCAL scalar
         * Zero means no monitor references one, so the manager interval is used
         */
        void setLocalMonitorSampleRates(const std::unordered_map<std::string, u32> &rates);

        // Popup support: immediate members of a DWARF struct VarEntry
        // Pass typeOff=0/baseAddr=0 to start from the top-level variable
        struct PopupMember {
                std::string name;
                std::string valStr; // cached display value, "..." if not yet read
                bool        isStruct{false};
                u64         typeOff{0};
                u64         addr{0};
        };
        std::vector<PopupMember>
        getPopupMembers(const std::string &varName, const std::string &pathPrefix, u64 typeOff = 0, u64 baseAddr = 0) const;
        /**
         * @brief Read a DWARF struct member from the display-value cache as float
         */
        bool getDwarfMemberAsFloat(const std::string &memberPath, float &out) const;
        /**
         * @brief Proactively refresh a DWARF member value via JLink (rate-limited ~100ms)
         * Populates memberValueCache_ so getDwarfMemberAsFloat returns up-to-date data
         */
        void refreshDwarfMember(const std::string &memberPath);

        /**
         * @brief After ELF reload: refresh `ch`'s enum entries from this Variable's
         * current DWARF info + user overrides (enumDefs / memberEnumDefs)
         * Returns true if the ch's symbol path belongs to this Variable
         * (i.e. is present in searchPool_), false otherwise — callers should
         * try other Variable instances when false
         */
        bool refreshChannelEnums(MonitorChannel *ch);

        /**
         * @brief Publish header-derived enum definitions for the members of a LOCAL struct
         * variable (an SDK data table). Keys are full member paths, e.g
         * "SDK0.fsav3_rt.control.set_control_mode". Paths absent from `memberEnums`
         * keep whatever the user configured by hand. Returns true if anything
         * actually changed, so the caller can skip refreshing monitor chs
         */
        bool setLocalMemberEnums(const std::string                                                     &varName,
                                 const std::unordered_map<std::string, std::vector<VarEntry::EnumDef>> &memberEnums);

        /**
         * @brief LOCAL counterpart of refreshChannelEnums(): push a LOCAL variable's
         * memberEnumDefs into an already-plotted ch. LOCAL variables have no
         * ELF backing, so neither searchPool_ nor DWARF can answer here
         */
        bool refreshLocalChannelEnums(const std::string &varName, MonitorChannel *ch);

      public:
        explicit Variable(std::string name) : name_(std::move(name)) { LOG_I("Variable Window Created: %s", name_.c_str()); }
        Variable() { LOG_I("Variable Window Created (default)"); };
        ~Variable()
        {
                LOG_I("Variable Window Destroyed: %s", name_.c_str());
                stopAccessorThread();
                stopPollThread();
                if (currentLoadingTask_) {
                        currentLoadingTask_->aborted = true;
                }
                if (elfLoadThread_.joinable())
                        elfLoadThread_.join();
                unregisterFirmwareSource();
        };

        bool loadCfg(const std::string &cfgPath);
        bool loadBin(const std::string &binPath);
        bool loadElf(const std::string &elfPath);
        void completeElfReload();

        void updateDisplay();

        /**
         * @brief Add a dragged symbol to this window's watch list. Shared by the watch-list
         * drop target and the "mirror from a monitor drop" path. Both dedup by name
         */
        void addScalarToWatch(const ChannelDropPayload &p);
        void addStructToWatch(const StructChannelPayload &s);
        /**
         * @brief Dispatch a queued mirror request (symbol-browser drag dropped onto a monitor)
         */
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
        std::string        getLocalElfPath() const
        {
                std::lock_guard lk(mtxElf_);
                return remote::isUrl(elfPath_) ? elfLocalPath_ : elfPath_;
        }
        bool consumeElfReloaded()
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
