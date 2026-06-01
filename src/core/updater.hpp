/**
 * @file  updater.hpp
 * @brief In-app update checker. Queries the latest GitHub release, compares it
 *        against AVA_VERSION, and (on the user's request) launches the standalone
 *        updater.exe which downloads + installs the new version and relaunches.
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

        // Start a background check. No-op if one is already running. Safe to call
        // once at startup and again from a "Check for Updates" menu item.
        void checkAsync();

        // Thread-safe snapshot of the latest result.
        Info get() const;

        bool isChecking() const { return running_.load(std::memory_order_acquire); }

        // Launch updater.exe (sitting next to this exe) to download `assetUrl`,
        // wait for this process to exit, install, and relaunch. Returns true if the
        // helper was started — the caller should then close the app.
        bool launchUpdater(const std::string &assetUrl);

        // Compare dotted version strings ("1.2.0"). >0 if a>b, 0 if equal, <0 if a<b.
        // Leading 'v'/'V' and trailing pre-release suffixes are ignored.
        static int compareVersions(const std::string &a, const std::string &b);

      private:
        mutable std::mutex mtx_;
        Info               info_;
        std::atomic<bool>  running_{false};
};

#endif // !UPDATER_HPP
