#ifndef FLASH_TOOL_HPP
#define FLASH_TOOL_HPP

#include "core/flash_array.hpp"
#include "core/remote_files.hpp"
#include <atomic>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct cJSON;

class FlashTool
{
        friend struct FlashToolTestAccess;

      public:
        FlashTool();
        ~FlashTool();

        void open() { open_ = true; }
        bool isOpen() const { return open_; }
        void draw();
        void saveSession(cJSON *root) const;
        void loadSession(const cJSON *root);
        bool consumeModified()
        {
                bool changed = modified_;
                modified_    = false;
                return changed;
        }

      private:
        bool                            open_{false};
        bool                            modified_{false};
        remote::Settings                remoteConfig_;
        bool                            remoteRefreshPending_{true};
        std::atomic<unsigned long long> remoteGeneration_{0};
        void                            resetRemoteState();

        char               firmwarePath_[1024]{};
        char               binAddress_[32]{"08000000"};
        bool               eraseChip_{false};
        bool               resetAfter_{true};
        std::atomic<bool>  flashing_{false};
        std::atomic<float> flashProgress_{0.0f};
        std::thread        flashThread_{};
        int                eraseMode_{0}; // selected sectors / whole chip
        uint32_t           eraseSectors_{0};
        int                eraseSpeed_{1000};
        std::string        eraseDevice_;
        uint64_t           eraseEpoch_{0};
        std::string        eraseLog_;
        void               drawEraseFlash();
        void               startEraseFlash();

        // ── Remote firmware (HTTP file server) ────────────────────────────────────
        struct RemoteFirmware {
                std::string name;
                std::string path; // direct HTTP(S) file URL
        };
        std::atomic<bool>           listing_{false};
        std::thread                 listThread_{};
        std::vector<RemoteFirmware> remoteFiles_{};
        std::string                 selectedRemotePath_{};

        std::atomic<bool>  downloading_{false};
        std::atomic<float> downloadProgress_{0.0f};
        std::thread        downloadThread_{};

        std::mutex  statusMtx_{};
        std::string flashStatus_{};
        std::string listStatus_{};
        std::string downloadStatus_{};
        // Path of a just-downloaded image, handed to firmwarePath_ on the UI thread
        // The download runs on its own thread and firmwarePath_ backs an InputText,
        // so it must not be written from there
        std::string pendingLocalPath_{};
        bool        flashOk_{false};
        bool        downloadOk_{false};

        char                                                     readAddress_[32]{"08000000"};
        int                                                      readBytes_{4096};
        int                                                      readType_{1};
        bool                                                     readBigEndian_{false};
        double                                                   readStep_{1.0};
        char                                                     arrayName_[128]{"FlashArray"};
        std::atomic<bool>                                        reading_{false};
        std::atomic<bool>                                        cancelRead_{false};
        std::atomic<float>                                       readProgress_{0.0f};
        std::future<std::shared_ptr<const flasharray::Snapshot>> readFuture_;
        std::shared_ptr<const flasharray::Snapshot>              readSnapshot_;
        std::string                                              readError_;
        size_t                                                   hexSelection_{0};
        char                                                     hexJump_[32]{};
        void                                                     drawReadFlash();
        void startReadFlash(bool reinterpretOnly = false, const std::string &file = "");
        void selectFirmware();
        void startFlash();
        void refreshRemoteFiles();
        void startDownload(const RemoteFirmware &file);
        void setFlashResult(bool ok, const std::string &message);
        void setDownloadResult(bool ok, const std::string &message);
};

#endif // FLASH_TOOL_HPP
