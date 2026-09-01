#pragma once

#include "../../ckit/include/jlink.h"

// ava_tool 按需加载的烧录接口, 不依赖导入库是否包含这些导出
extern "C" {
int  JLINK_DownloadFile(const char *fileName, uint32_t address);
void JLINKARM_BeginDownload(uint32_t flags);
int  JLINKARM_EndDownload(void);
int  JLINKARM_WriteVerifyMem(uint32_t address, uint32_t size, const void *data);
int  JLINK_EraseChip(void);
}
