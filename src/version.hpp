#ifndef AVA_VERSION_HPP
#define AVA_VERSION_HPP

// Single source of truth for the application version
//
// Bump this on every release and keep it in sync with the installer
// (installer/ava_tool.iss -> MyAppVersion) and the uploaded installer name
// (ava_tool_setup_<this>.exe) on the internal file server at
// remote::settings().toolUrl. The in-app updater compares this against the
// newest installer version found there
#define AVA_VERSION "0.2.1"

#endif // !AVA_VERSION_HPP
