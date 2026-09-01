/**
 * @file  updater.hpp
 * @brief In-app update checker. Lists the installer directory on the internal
 *        file server (remote::settings().toolUrl), compares the newest
 *        ava_tool_setup_*.exe version against AVA_VERSION, and (on the user's
 *        request) downloads it in the background and prompts the user to
 *        restart to install.
 */
#ifndef UPDATER_HPP
#define UPDATER_HPP
#include <thread>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class Updater
{
      public:
        ~Updater();
        struct Installer {
                std::string version;
                std::string url;
        };
        struct Info {
                bool                   checked   = false; // a check has completed (success or failure)
                bool                   available = false; // a newer version is available
                std::string            currentVersion;
                std::string            sourceUrl; // configured update address used for this check
                std::string            latestVersion;
                std::string            assetUrl;   // installer (.exe) download URL, if any
                std::string            releaseUrl; // release HTML page (fallback)
                std::string            notes;      // release body (truncated for display)
                std::string            error;      // non-empty on failure
                std::vector<Installer> installers; // newest first, one installer per version
        };

        enum class DownloadState { Idle, Downloading, Done, Failed };

        /**
         * @brief Start a background check. No-op if one is already running. Safe to call
         * once at startup and again from a "Check for Updates" menu item
         */
        void checkAsync();

        /**
         * @brief Thread-safe snapshot of the latest result
         */
        Info get() const;

        bool isChecking() const { return running_.load(std::memory_order_acquire); }

        // --------------- Background download ---------------

        /**
         * @brief Start downloading `assetUrl` to a temp file in the background
         * No-op if a download is already in progress
         */
        void downloadAsync(const std::string &assetUrl);

        DownloadState getDownloadState() const { return dlState_.load(std::memory_order_acquire); }

        /**
         * @brief Returns 0..100. Only meaningful when state == Downloading
         */
        int getDownloadProgress() const { return dlProgress_.load(std::memory_order_acquire); }

        /**
         * @brief Path to the downloaded installer (valid when state == Done)
         */
        std::string getDownloadedPath() const;

        /**
         * @brief Human-readable error (valid when state == Failed)
         */
        std::string getDownloadError() const;

        /**
         * @brief Reset download state back to Idle (e.g. after user dismisses error)
         */
        void resetDownload();

        // --------------- Install ---------------

        /**
         * @brief Launch the already-downloaded installer at `setupPath` with silent flags
         * Returns true if successfully started — caller should then close the app
         */
        bool launchInstaller(const std::string &setupPath);

        /**
         * @brief (Legacy) Launch updater.exe to download + install. Kept for fallback
         */
        bool launchUpdater(const std::string &assetUrl);

        /**
         * @brief Compare dotted version strings ("1.2.0"). >0 if a>b, 0 if equal, <0 if a<b
         * Leading 'v'/'V' and trailing pre-release suffixes are ignored
         */
        static int compareVersions(const std::string &a, const std::string &b);

      private:
        std::thread        checkThread_;
        std::thread        downloadThread_;
        mutable std::mutex mtx_;
        Info               info_;
        std::atomic<bool>  running_{false};

        // Download state
        std::atomic<DownloadState> dlState_{DownloadState::Idle};
        std::atomic<int>           dlProgress_{0};
        mutable std::mutex         dlMtx_;
        std::string                dlPath_;  // path to downloaded file
        std::string                dlError_; // error message
};

#endif // !UPDATER_HPP
