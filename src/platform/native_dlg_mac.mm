#include "native_dlg.hpp"
#import <Cocoa/Cocoa.h>

static NSArray<NSString *> *buildTypes(const std::vector<NativeDlgFilter> &filters)
{
    NSMutableArray<NSString *> *arr = [NSMutableArray array];
    for (const auto &f : filters)
        for (const auto &e : f.exts)
            [arr addObject:@(e.c_str())];
    return arr.count > 0 ? arr : nil;
}

std::string
nativeDlgOpen(const std::string &title, const std::vector<NativeDlgFilter> &filters,
              const std::string &defaultDir)
{
    @autoreleasepool {
        NSOpenPanel *panel          = [NSOpenPanel openPanel];
        panel.title                 = @(title.c_str());
        panel.canChooseFiles        = YES;
        panel.canChooseDirectories  = NO;
        panel.allowsMultipleSelection = NO;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        panel.allowedFileTypes = buildTypes(filters); // nil = all files
#pragma clang diagnostic pop

        if (!defaultDir.empty() && defaultDir != ".")
            panel.directoryURL = [NSURL fileURLWithPath:@(defaultDir.c_str())];

        return ([panel runModal] == NSModalResponseOK) ? std::string(panel.URL.path.UTF8String) : "";
    }
}

std::string
nativeDlgSave(const std::string &title, const std::vector<NativeDlgFilter> &filters,
              const std::string &defaultName, const std::string &defaultDir)
{
    @autoreleasepool {
        NSSavePanel *panel = [NSSavePanel savePanel];
        panel.title        = @(title.c_str());
        if (!defaultName.empty())
            panel.nameFieldStringValue = @(defaultName.c_str());

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        panel.allowedFileTypes = buildTypes(filters);
#pragma clang diagnostic pop

        if (!defaultDir.empty() && defaultDir != ".")
            panel.directoryURL = [NSURL fileURLWithPath:@(defaultDir.c_str())];

        return ([panel runModal] == NSModalResponseOK) ? std::string(panel.URL.path.UTF8String) : "";
    }
}
