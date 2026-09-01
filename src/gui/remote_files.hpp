#pragma once
#include "core/local_file_server.hpp"
#include "core/remote_files.hpp"
#include <future>

/**
 * @brief UI for the shared software/session settings and an asynchronous directory menu
 */
bool drawServerSettings(LocalFileServer &server, LocalFileServer &website, const std::string &settingsDirectory);
class RemoteFileMenu
{
        remote::Settings                       config_;
        std::string                            url_;
        std::vector<remote::File>              files_;
        std::future<std::vector<remote::File>> pending_;
        std::string                            error_;
        bool                                   initialized_{false};

      public:
        std::string draw(const std::string &url, const std::vector<std::string> &extensions);
};
