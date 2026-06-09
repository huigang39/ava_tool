/**
 * @file  monitor_types.hpp
 * @brief Shared data structures for drag-and-drop payloads and view-mode enums.
 */
#ifndef MONITOR_TYPES_HPP
#define MONITOR_TYPES_HPP

#include "module.h"
#include <string>
#include <vector>

// Payload carried when dragging a single scalar symbol from the symbol browser
// onto a monitor scope. Serialised into the ImGui drag-drop system as POD.
struct ChannelDropPayload {
        static constexpr int kMaxEnums = 16;
        struct EnumEntry {
                char name[64];
                i64  value;
        };

        char      name[128];
        u64       addr;
        char      type[8];     // "F32"/"F64"/"I8"/"I16"/"I32"/"I64"/"U8"/"U16"/"U32"/"U64"
        char      device[8];   // "SHM" / "JLINK" / "UDP"
        char      shmName[64]; // SHM region name (when device=="SHM"), separate from variable path
        u8        numBytes;    // 1/2/4/8 - derived from type
        u8        numEnums;    // 0 = not an enum
        u64       typeOff;     // DWARF type offset
        u32       bitOffset;
        u32       bitSize;
        bool      writable;
        EnumEntry enums[kMaxEnums];
        // Source Variable (its watch list) when the drag started in a symbol browser;
        // null otherwise. Lets a monitor drop mirror the symbol back into that watch list.
        const void *srcWatch{nullptr};
};

// Payload for dragging a struct/array from the variable window to a monitor scope.
// Each entry is one scalar leaf member, flattened from the struct/array tree.
struct StructChannelPayload {
        static constexpr int kMaxEntries = 48;
        static constexpr int kMaxEnums   = ChannelDropPayload::kMaxEnums;
        struct Entry {
                char                          name[64]; // full dotted path, e.g. "foc.cfg.base_cfg.dir"
                u64                           addr;
                char                          type[8]; // same codes as ChannelDropPayload::type
                u8                            numEnums;
                u32                           bitOffset;
                u32                           bitSize;
                bool                          writable;
                ChannelDropPayload::EnumEntry enums[kMaxEnums];
        };
        int  count;
        char device[8];
        char shmName[64];
        bool writable;
        // Root struct/array metadata, so a drop target (e.g. the variable watch
        // list) can add the whole struct as a single expandable entry instead of
        // the flattened scalar leaves the monitor consumes.
        char  rootName[128];
        u64   rootAddr;
        u64   rootTypeOff;
        Entry entries[kMaxEntries];
        // See ChannelDropPayload::srcWatch.
        const void *srcWatch{nullptr};
};

// A request, queued when a symbol-browser drag is dropped on a monitor, to mirror
// that symbol into the originating Variable's watch list. Drained once per frame by
// Gui::loop(), which dispatches it to the Variable whose pointer matches `target`.
struct WatchMirrorRequest {
        const void          *target{nullptr}; // the Variable* that should receive it
        bool                 isStruct{false};
        ChannelDropPayload   scalar{}; // valid when !isStruct
        StructChannelPayload group{};  // valid when isStruct
};

inline std::vector<WatchMirrorRequest> &
watchMirrorQueue()
{
        static std::vector<WatchMirrorRequest> q;
        return q;
}

enum class MonitorViewMode { FULL, FOLLOW, MANUAL };

#endif // !MONITOR_TYPES_HPP
