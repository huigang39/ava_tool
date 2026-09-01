#include "cJSON.h"
#include "gui/flash_tool.hpp"
#include <cstdio>

FlashTool::FlashTool() = default;

FlashTool::~FlashTool()
{
        cancelRead_ = true;
        if (readFuture_.valid())
                readFuture_.wait();
        if (flashThread_.joinable())
                flashThread_.join();
        if (listThread_.joinable())
                listThread_.join();
        if (downloadThread_.joinable())
                downloadThread_.join();
}

void
FlashTool::saveSession(cJSON *root) const
{
        cJSON *node = cJSON_CreateObject();
        cJSON_AddBoolToObject(node, "open", open_);
        cJSON_AddStringToObject(node, "firmwarePath", firmwarePath_);
        cJSON_AddStringToObject(node, "binAddress", binAddress_);
        cJSON_AddBoolToObject(node, "eraseChip", eraseChip_);
        cJSON_AddBoolToObject(node, "resetAfter", resetAfter_);
        cJSON_AddStringToObject(node, "readAddress", readAddress_);
        cJSON_AddNumberToObject(node, "readBytes", readBytes_);
        cJSON_AddNumberToObject(node, "readType", readType_);
        cJSON_AddBoolToObject(node, "readBigEndian", readBigEndian_);
        cJSON_AddNumberToObject(node, "readStep", readStep_);
        cJSON_AddStringToObject(node, "arrayName", arrayName_);
        cJSON_AddItemToObject(root, "flashTool", node);
}
void
FlashTool::loadSession(const cJSON *root)
{
        const cJSON *node = cJSON_GetObjectItemCaseSensitive(root, "flashTool");
        open_             = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(node, "open"));
        auto read         = [&](const char *key, char *out, size_t size) {
                const cJSON *v = cJSON_GetObjectItemCaseSensitive(node, key);
                if (cJSON_IsString(v))
                        snprintf(out, size, "%s", v->valuestring);
        };
        resetRemoteState();
        firmwarePath_[0] = '\0';
        snprintf(binAddress_, sizeof(binAddress_), "08000000");
        eraseSectors_ = 0;
        eraseMode_    = 0;
        eraseChip_    = false;
        resetAfter_   = true;
        read("firmwarePath", firmwarePath_, sizeof(firmwarePath_));
        read("binAddress", binAddress_, sizeof(binAddress_));
        if (const auto *v = cJSON_GetObjectItemCaseSensitive(node, "eraseChip"); cJSON_IsBool(v))
                eraseChip_ = cJSON_IsTrue(v);
        if (const auto *v = cJSON_GetObjectItemCaseSensitive(node, "resetAfter"); cJSON_IsBool(v))
                resetAfter_ = cJSON_IsTrue(v);
        cancelRead_ = true;
        readSnapshot_.reset();
        readError_.clear();
        snprintf(readAddress_, sizeof(readAddress_), "08000000");
        snprintf(arrayName_, sizeof(arrayName_), "FlashArray");
        readBytes_     = 4096;
        readType_      = 1;
        readBigEndian_ = false;
        readStep_      = 1;
        read("readAddress", readAddress_, sizeof(readAddress_));
        read("arrayName", arrayName_, sizeof(arrayName_));
        if (const auto *v = cJSON_GetObjectItemCaseSensitive(node, "readBytes");
            cJSON_IsNumber(v) && v->valuedouble >= 1 && v->valuedouble <= 16 * 1024 * 1024)
                readBytes_ = v->valueint;
        if (const auto *v = cJSON_GetObjectItemCaseSensitive(node, "readType");
            cJSON_IsNumber(v) && v->valueint >= 0 && v->valueint < 11)
                readType_ = v->valueint;
        if (const auto *v = cJSON_GetObjectItemCaseSensitive(node, "readBigEndian"); cJSON_IsBool(v))
                readBigEndian_ = cJSON_IsTrue(v);
        if (const auto *v = cJSON_GetObjectItemCaseSensitive(node, "readStep"); cJSON_IsNumber(v) && v->valuedouble > 0)
                readStep_ = v->valuedouble;
        modified_ = false;
}

void
FlashTool::resetRemoteState()
{
        std::lock_guard lock(statusMtx_);
        ++remoteGeneration_;
        remoteConfig_         = remote::settings();
        remoteRefreshPending_ = true;
        pendingLocalPath_.clear();
        remoteFiles_.clear();
        listStatus_.clear();
        downloadStatus_.clear();
        selectedRemotePath_.clear();
}
