#ifndef DEVICE_MANAGER_HPP
#define DEVICE_MANAGER_HPP

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/c_header_parser.hpp"
#include "core/sdk_loader.hpp"

class DeviceManager
{
      public:
        DeviceManager() = default;
        ~DeviceManager();

        DeviceManager(const DeviceManager &)            = delete;
        DeviceManager &operator=(const DeviceManager &) = delete;

        void draw();
        void clear();

        bool isOpen() const { return open_; }
        void setOpen(bool open) { open_ = open; }
        bool isModified() const { return modified_; }

        bool consumeModified()
        {
                const bool wasModified = modified_;
                modified_              = false;
                return wasModified;
        }
        void clearModified() { modified_ = false; }

        // Session persistence. node is cJSON*, while baseDir is the directory
        // containing the .ava file. Runtime library handles are never persisted.
        void save(void *node, const std::string &baseDir) const;
        bool load(const void *node, const std::string &baseDir);

        // Applies dropped DLL/header files to the currently selected device type.
        void pushDroppedFiles(const std::vector<std::string> &paths);
        void setSymbolResolver(std::function<bool(const std::string &, uint32_t &)> resolver)
        {
                symbolResolver_ = std::move(resolver);
        }

      private:
        enum class ArgSource { Literal, PropertyValue, PropertyAddress };
        enum class MethodKind { Instance, Discovery };
        enum class PropertySource { Manual, JLink };

        struct Property {
                uint64_t    id{0};
                std::string name;
                std::string discoveryKey;
                CType       type{CType::F64};
                alignas(16) std::array<uint8_t, 16> data{};
                PropertySource source{PropertySource::Manual};
                std::string    symbol;
                uint32_t       address{0};
                uint32_t       bitOffset{0};
                uint32_t       bitSize{0};
                bool           writable{true};

                // UI-only edit state. Persisted values always come from data.
                std::string editValue;
                bool        editValueInvalid{false};
                bool        editActive{false};
                bool        liveValueKnown{false};
                bool        liveReadOk{false};
                bool        symbolResolved{true};
                bool        writePending{false};
                bool        lastWriteOk{true};
                uint64_t    writeToken{0};
        };

        struct ArgumentBinding {
                ArgSource   source{ArgSource::Literal};
                std::string literal{"0"};
                uint64_t    propertyId{0};
        };

        struct DeviceInstance {
                uint64_t              id{0};
                std::string           name;
                std::string           discoveryKey;
                std::string           discoveredName;
                std::vector<Property> properties;
                bool                  customName{false};
                bool                  online{false}; // runtime discovery state; not persisted
        };

        struct MethodBinding {
                uint64_t                     id{0};
                std::string                  name;
                std::string                  functionName;
                std::string                  exportSymbol;
                std::string                  signature;
                std::vector<ArgumentBinding> arguments;
                uint64_t                     resultPropertyId{0};
                MethodKind                   kind{MethodKind::Instance};
                int                          discoveryBufferArg{-1};
                int                          discoveryCapacityArg{-1};
                uint32_t                     discoveryBufferSize{65536};
                std::string                  lastResult;
                bool                         lastResultOk{false};
        };

        struct Device {
                uint64_t    id{0};
                std::string name;
                std::string libraryPath;
                std::string headerPath;
                std::string headerSource;

                uint64_t                    nextPropertyId{1};
                uint64_t                    nextMethodId{1};
                uint64_t                    nextInstanceId{1};
                std::vector<Property>       properties;
                std::vector<MethodBinding>  methods;
                std::vector<DeviceInstance> instances;

                SdkLoader   loader;
                ParseResult declarations;
                std::string status;
                bool        statusIsError{false};
        };

        bool                                 open_{false};
        bool                                 modified_{false};
        uint64_t                             nextDeviceId_{1};
        std::vector<std::unique_ptr<Device>> devices_;
        int                                  selectedDevice_{-1};
        int                                  selectedInstance_{-1}; // -1 selects the device type itself
        int                                  selectedFunction_{-1};
        float                                sidebarWidth_{260.0f};
        uint64_t                             pendingDeleteTypeId_{0};
        uint64_t                             pendingDeleteInstanceId_{0};
        uint64_t                             pendingDeletePropertyId_{0};
        uint64_t                             pendingDeleteMethodId_{0};
        char                                 newDeviceName_[96]{};
        std::vector<std::string>             pendingDropFiles_;
        std::string                          managerStatus_;
        bool                                 managerStatusIsError_{false};

        struct PollReq {
                uint32_t address{0};
                uint32_t size{0};
        };
        struct PollVal {
                std::array<uint8_t, 8> data{};
                uint32_t               size{0};
                bool                   ok{false};
        };
        struct WriteReq {
                uint64_t               token{0};
                uint32_t               address{0};
                uint32_t               size{0};
                uint32_t               bitOffset{0};
                uint32_t               bitSize{0};
                std::array<uint8_t, 8> data{};
        };
        struct PollState {
                std::mutex                            mutex;
                std::vector<PollReq>                  requests;
                std::unordered_map<uint32_t, PollVal> values;
                std::deque<WriteReq>                  writes;
                std::unordered_map<uint64_t, bool>    writeResults;
                std::atomic<bool>                     running{true};
                std::atomic<uint32_t>                 intervalMs{100};
        };
        std::shared_ptr<PollState>                           poll_{};
        std::thread                                          pollThread_{};
        uint64_t                                             nextWriteToken_{1};
        uint64_t                                             lastSymbolResolveMs_{0};
        std::function<bool(const std::string &, uint32_t &)> symbolResolver_{};

        void createDevice(const std::string &requestedName);
        void deleteDevice(int index);
        void createInstance(Device &device, const std::string &requestedName = "");
        void deleteInstance(Device &device, int index);
        void loadLibrary(Device &device);
        void loadHeader(Device &device);
        void bindFunction(Device &device, const CFuncDecl &function);
        void callMethod(Device &device, DeviceInstance *instance, MethodBinding &method);
        bool applyDiscoveryJson(Device &device, const char *json, uint32_t expectedCount, std::string &error, bool &changed);

        void drawDeviceList();
        void drawDevice(Device &device);
        void drawProperties(Device &device);
        void drawMethods(Device &device, DeviceInstance *instance = nullptr);
        void drawInstance(Device &device, DeviceInstance &instance);
        void drawInstanceProperties(Device &device, DeviceInstance &instance);
        void startPollThread();
        void stopPollThread();
        void refreshJLinkProperties();
        void queuePropertyWrite(Property &property);
        void acceptPropertyDrop(Device &device, DeviceInstance *instance);
        bool addBoundProperty(Device         &device,
                              DeviceInstance *instance,
                              const char     *name,
                              uint64_t        address,
                              const char     *type,
                              const char     *port,
                              bool            writable,
                              uint32_t        bitOffset,
                              uint32_t        bitSize);

        void exportDevice(const Device &device);
        void importDevice();

        void                    saveDevice(void *node, const Device &device, const std::string &baseDir) const;
        std::unique_ptr<Device> loadDevice(const void *node, const std::string &baseDir, std::string &error);

        static Property        *findProperty(Device &device, uint64_t id);
        static const Property  *findProperty(const Device &device, uint64_t id);
        static Property        *findProperty(DeviceInstance &instance, uint64_t id);
        static const Property  *findProperty(const DeviceInstance &instance, uint64_t id);
        static const CFuncDecl *findFunction(const Device &device, const MethodBinding &method);
        static std::string      functionSignature(const CFuncDecl &function);
        static std::string      functionLabel(const CFuncDecl &function);
        static std::string      propertyValueString(const Property &property);
        static bool             setPropertyValue(Property &property, const std::string &value);
        static bool             assignResult(Property &property, const CallResult &result, CType returnType);
};

#endif // DEVICE_MANAGER_HPP
