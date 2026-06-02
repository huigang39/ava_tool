/**
 * @file  updater.hpp
 * @brief In-app update checker. Queries the latest GitHub release, compares it
 *        against AVA_VERSION, and (on the user's request) downloads the installer
 *        in the background and prompts the user to restart to install.
 */
#ifndef UPDATER_HPP
#define UPDATER_HPP

#include <atomic>
#include <mutex>
#include <string>

class Updater
{
      public:
        struct Info {
                bool        checked   = false; // a check has completed (success or failure)
                bool        available = false; // a newer version is available
                std::string currentVersion;
                std::string latestVersion;
                std::string assetUrl;   // installer (.exe) download URL, if any
                std::string releaseUrl; // release HTML page (fallback)
                std::string notes;      // release body (truncated for display)
                std::string error;      // non-empty on failure
        };

        enum class DownloadState { Idle, Downloading, Done, Failed };

        // Start a background check. No-op if one is already running. Safe to call
        // once at startup and again from a "Check for Updates" menu item.
        void checkAsync();

        // Thread-safe snapshot of the latest result.
        Info get() const;

        bool isChecking() const { return running_.load(std::memory_order_acquire); }

        // --------------- Background download ---------------

        // Start downloading `assetUrl` to a temp file in the background.
        // No-op if a download is already in progress.
        void downloadAsync(const std::string &assetUrl);

        DownloadState getDownloadState() const { return dlState_.load(std::memory_order_acquire); }

        // Returns 0..100. Only meaningful when state == Downloading.
        int getDownloadProgress() const { return dlProgress_.load(std::memory_order_acquire); }

        // Path to the downloaded installer (valid when state == Done).
        std::string getDownloadedPath() const;

        // Human-readable error (valid when state == Failed).
        std::string getDownloadError() const;

        // Reset download state back to Idle (e.g. after user dismisses error).
        void resetDownload();

        // --------------- Install ---------------

        // Launch the already-downloaded installer at `setupPath` with silent flags.
        // Returns true if successfully started — caller should then close the app.
        bool launchInstaller(const std::string &setupPath);

        // (Legacy) Launch updater.exe to download + install. Kept for fallback.
        bool launchUpdater(const std::string &assetUrl);

        // Compare dotted version strings ("1.2.0"). >0 if a>b, 0 if equal, <0 if a<b.
        // Leading 'v'/'V' and trailing pre-release suffixes are ignored.
        static int compareVersions(const std::string &a, const std::string &b);

      private:
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
