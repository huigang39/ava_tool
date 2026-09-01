#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class LocalFileServer
{
      public:
        enum class Mode { Files, Website };
        explicit LocalFileServer(Mode mode = Mode::Files) : mode_(mode) {}
        struct Settings {
                std::string folder;
                int         port{8090};
                bool        autoStart{false};
                bool        spaFallback{false};
                std::string firmwareFolder, sessionsFolder, updatesFolder;
                std::string name; // Empty uses the computer name
        } config;
        ~LocalFileServer();
        bool                     start();
        void                     stop();
        bool                     running() const { return running_.load(); }
        int                      port() const { return boundPort_; }
        const std::string       &error() const { return error_; }
        uint64_t                 requests() const { return requests_.load(); }
        std::vector<std::string> addresses() const;
        struct DiscoveredServer {
                std::string id, name, version, host;
                int         port{};
                std::string firmwareUrl, sessionsUrl, updatesUrl;
                std::string baseUrl() const { return "http://" + host + ":" + std::to_string(port) + "/"; }
        };
        /**
         * @brief IPv4 LAN discovery; the caller runs this bounded scan on a worker
         */
        static std::vector<DiscoveredServer> discover(int milliseconds = 1800);
        bool                                 refreshDiscovery();
        const std::string                   &discoveryError() const { return discoveryError_; }
        /**
         * @brief Relative directory below the shared root; throws for missing/outside paths
         */
        std::string directoryUrl(const std::string &host, const std::string &relative = "") const;
        void        loadSettings(const std::string &path);
        bool        saveSettings(const std::string &path) const;

      private:
        Mode                     mode_;
        bool                     spaFallback_{};
        std::filesystem::path    root_;
        std::uintptr_t           listener_{~std::uintptr_t{0}};
        std::vector<std::thread> workers_;
        std::atomic<bool>        running_{false};
        std::atomic<uint64_t>    requests_{0};
        int                      boundPort_{};
        bool                     socketsInitialized_{};
        std::string              error_;
        std::uintptr_t           discoverySocket_{~std::uintptr_t{0}};
        std::thread              discoveryThread_;
        std::mutex               discoveryMutex_;
        std::string              discoveryPayload_, discoveryError_, discoverySocketError_, instanceId_;
        void                     startDiscovery();
        void                     answerDiscovery();
        void                     serve();
        void                     handle(std::uintptr_t client);
        bool                     sendAll(std::uintptr_t client, const char *data, size_t size);
};
