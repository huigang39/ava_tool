#ifndef AVA_VERSION_HPP
#define AVA_VERSION_HPP

// Single source of truth for the application version.
//
// Bump this on every release and keep it in sync with the installer
// (installer/ava_tool.iss -> MyAppVersion) and the git tag you publish on
// GitHub Releases (tag "v<this>", e.g. v1.0.0). The in-app updater compares
// this against the latest GitHub release tag.
#define AVA_VERSION      "0.1.10"

// GitHub repository that hosts the releases consumed by the update checker.
#define AVA_GITHUB_OWNER "huigang39"
#define AVA_GITHUB_REPO  "ava_tool"

#endif // !AVA_VERSION_HPP
