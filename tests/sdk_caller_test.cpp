#include "core/sdk_loader.hpp"
#include "gui/sdk_panel.hpp"
#include "app_log.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <filesystem>
#define KPS6050D_STATIC
#include "../sdk/kps6050d/inc/kps6050d.h"

log_t g_log_ops{};
log_t g_log_perf{};
int get_log_idx() { return 0; }

struct SdkPanelRegression {
        static void sampleFreshness(const char *library)
        {
                SdkPanel panel;
                assert(panel.loader_.load(library));
                // The fixture writes the first 32 bytes; appended metadata is
                // controlled here to model no-data and cached-frame SDK reads.
                panel.parseResult_ = parseHeaderFull(R"(
                    typedef struct fixture_rt {
                        uint32_t write_command;
                        uint32_t command_value;
                        uint64_t feedback;
                        double torque;
                        uint32_t values[2];
                        uint32_t fresh;
                        uint32_t sample_count;
                    } fixture_rt;
                    int32_t sdk_sync_read_rt(fixture_rt *p);
                    int32_t sdk_sync_write_rt(fixture_rt *p);
                )");
                const auto *schema = findStruct(panel.parseResult_, "fixture_rt");
                assert(schema && schema->totalSize == 40 && schema->fields.size() == 7);
                assert(schema->fields[5].name == "fresh" && schema->fields[5].offset == 32);
                assert(schema->fields[6].name == "sample_count" && schema->fields[6].offset == 36);
                SdkPanel::DataTableBinding binding;
                binding.name = binding.structName = "fixture_rt";
                binding.localName = "SDK0.fixture_rt";
                binding.kind = SdkPanel::DataTableKind::Realtime;
                binding.data.resize(40);
                binding.readFnIdx = 0; binding.writeFnIdx = 1;
                binding.structParamRead = binding.structParamWrite = 0;
                panel.dataTables_.push_back(binding);
                SdkPanel::LocalBuffers locals{{binding.localName + ".torque", std::vector<uint8_t>(8)}};
                panel.onSnapshotLocalBuffers_ = [&] { return locals; };
                panel.onCompareExchangeLocalBuffer_ = [&](const auto &name, const auto &expected, const auto &desired) {
                        if (locals.at(name) != expected) return false;
                        locals[name] = desired; return true;
                };
                int samples = 0;
                panel.onLocalSample_ = [&](const auto &, const auto &, double) { ++samples; };
                panel.refreshDataTableBindings();
                const auto read = [&](uint32_t fresh, uint32_t count) {
                        memcpy(panel.dataTables_[0].data.data() + 32, &fresh, 4);
                        memcpy(panel.dataTables_[0].data.data() + 36, &count, 4);
                        panel.runDataTableCall(0, false, false);
                };
                read(0, 0); assert(samples == 0);
                read(1, 1); assert(samples == 1);
                read(1, 1); assert(samples == 1);
                read(0, 2); assert(samples == 1);
                read(1, 3); assert(samples == 2);
                panel.runDataTableCall(0, true, true); assert(samples == 2);
                puts("SDK samples: empty reads, cached frames and write acknowledgements do not advance waveforms");
        }
        static void presets()
        {
                SdkPanel panel;
                panel.onDrawDsoGui_ = [] {};
                const char *headers[] = {"dyn200.h", "kps6050d.h", "dsox2024a.h", "fsav3_c.h"};
                const char *functions[] = {"dyn200_read_port_param", "kps6050d_open", "dsox2024a_open", "fsav3_read_rt"};
                for (size_t i=0; i<4; ++i) {
                        assert(panel.loadSdkPreset(i));
                        assert(panel.isDsoGuiActive() == (i == 2));
                        assert(panel.loader_.isLoaded() && !panel.statusIsErr_);
                        assert(std::filesystem::u8path(panel.headerPath_).filename()==headers[i]);
                        assert(panel.headerFileMtime()!=0);
                        bool found=false;
                        for (const auto &fn : panel.parseResult_.functions) if(fn.name==functions[i]) found=true;
                        assert(found);
                        assert(!panel.loader_.exports().empty());
                        if (i == 1) {
                                assert(panel.dataTables_.size() == 3);
                                bool port = false, param = false, rt = false;
                                for (const auto &table : panel.dataTables_) {
                                        if (table.name == "kps6050d_port_param") {
                                                port = true;
                                                assert(table.data.size() == sizeof(kps6050d_port_param));
                                                assert(table.portCountFnIdx >= 0 && table.portNameFnIdx >= 0);
                                                assert(table.actionFieldIdx >= 0);
                                        }
                                        if (table.name == "kps6050d_param") {
                                                param = true;
                                                assert(table.data.size() == sizeof(kps6050d_param));
                                                assert(table.readFnIdx >= 0 && table.writeFnIdx >= 0);
                                                bool nested = false, gate = false;
                                                for (const auto &f : panel.dataTableStruct(table)->fields) {
                                                        if (f.name == "voltage.val") {
                                                                nested = true;
                                                                assert(f.offset == offsetof(kps6050d_param, voltage));
                                                        }
                                                        if (f.name == "write_voltage") {
                                                                gate = true;
                                                                assert(f.offset == offsetof(kps6050d_param, write_voltage));
                                                        }
                                                }
                                                assert(nested && gate);
                                        }
                                        if (table.name == "kps6050d_rt") {
                                                rt = true;
                                                assert(table.data.size() == sizeof(kps6050d_rt));
                                                assert(table.readFnIdx >= 0 && table.writeFnIdx < 0);
                                        }
                                }
                                assert(port && param && rt);
                        }
                        for (auto &table : panel.dataTables_) {
                                assert(!table.autoRead && !table.autoWrite && table.callCount==0);
                                table.autoWrite=true;   // switching must discard automatic writes
                        }
                }
                assert(panel.loadSdkPreset(0));
                assert(panel.dataTables_.size()==3);
                for (const auto &table : panel.dataTables_) assert(!table.autoRead && !table.autoWrite);
                const auto previous=panel.loader_.loadedPath();
                const auto root=std::filesystem::u8path(previous).parent_path().parent_path();
                const auto header=root/"dyn200/dyn200.h", backup=root/"dyn200/held.h";
                std::filesystem::rename(header,backup);
                assert(!panel.loadSdkPreset(0) && panel.loader_.loadedPath()==previous);
                std::filesystem::rename(backup,header);
                assert(panel.loadSdkPreset(0));
                // Deliberately change CWD: shortcuts resolve relative to the executable
                std::filesystem::create_directory(root/"unrelated");
                std::filesystem::current_path(root/"unrelated");
                assert(panel.loadSdkPreset(2));
                puts("SDK presets: all four bundled pairs load, Unicode relocation, CWD independence, missing-file preservation and no automatic device calls passed");
        }
        static void serialPorts(const char *dll)
        {
                SdkPanel panel;
                assert(panel.loader_.load(dll));
                std::ifstream header("sdk/dyn200/inc/dyn200.h");
                assert(header);
                std::stringstream text; text << header.rdbuf();
                panel.parseResult_ = parseHeaderFull(text.str());
                panel.rebuildDataTables();
                size_t index = 0;
                while (index < panel.dataTables_.size() && panel.dataTables_[index].name != "dyn200_port_param") ++index;
                assert(index < panel.dataTables_.size());
                auto &binding = panel.dataTables_[index];
                const auto *table = panel.dataTableStruct(binding);
                assert(table && binding.portCountFnIdx >= 0 && binding.portNameFnIdx >= 0);
                auto field = [&](const char *name) -> const CFieldDecl & {
                        for (const auto &f : table->fields) if (f.name == name) return f;
                        std::abort();
                };
                auto getText = [&](const char *name) { return std::string(reinterpret_cast<char *>(binding.data.data()+field(name).offset)); };
                auto getInt = [&](const char *name) { uint32_t val; memcpy(&val,binding.data.data()+field(name).offset,4); return val; };
                auto action = [&](uint32_t val) {
                        memcpy(binding.data.data()+field("action").offset,&val,4);
                        panel.runDataTableCall(index,false,true);
                };
                CFuncDecl scenario; scenario.name="sdk_port_scenario"; scenario.retType=CType::Void;
                scenario.params={{"scenario",CType::U32,"uint32_t"}};
                CFuncDecl connections; connections.name="sdk_port_connections"; connections.retType=CType::U32;
                auto connectionCount = [&] { return panel.loader_.call(connections,{}).rawU64; };
                action(3);
                assert(binding.portsScanned && binding.portScanError.empty() && binding.serialPorts.size()==3);
                assert(binding.serialPorts[1].name=="COM11" && binding.serialPorts[2].description=="扭矩传感器 USB");
                assert(getText("port")=="COM3" && connectionCount()==0);
                const auto choice = binding.serialPorts[2];
                const auto committed = SdkPanel::selectSerialPort(binding,*table,choice);
                assert(committed.size()==4 && getText("port")=="COM42" && getText("name")=="COM42" && getInt("index")==2);
                assert(connectionCount()==0);   // selection is only an edit, never an implicit connection
                action(1);
                assert(connectionCount()==1 && getText("active_port")=="COM42");
                assert(panel.loader_.call(scenario,{"1"}).ok);
                action(3);
                assert(binding.serialPorts.size()==2 && getText("port")=="COM42" && getInt("index")==0);
                assert(connectionCount()==1);   // reordered discovery preserves the selected port
                assert(panel.loader_.call(scenario,{"2"}).ok);
                action(3);
                assert(binding.serialPorts.empty() && getText("name").empty() && getText("desc").empty());
                assert(getText("port")=="COM42" && connectionCount()==1);     // unplug never silently selects another adapter
                assert(panel.loader_.call(scenario,{"3"}).ok);
                action(3);
                assert(binding.serialPorts.empty() && !binding.portScanError.empty());
                assert(panel.loader_.call(scenario,{"0"}).ok);
                action(3);
                assert(binding.serialPorts.size()==3 && binding.portScanError.empty() && getInt("index")==2);
                puts("SDK serial dropdown: multi-port discovery, selection, connection, reorder, unplug and recovery passed");
        }
        static void run()
        {
                CStructDecl table;
                table.totalSize = 12;
                for (int i = 0; i < 3; ++i) {
                        CFieldDecl field;
                        field.name = "field" + std::to_string(i);
                        field.type = CType::U32;
                        field.size = 4;
                        field.offset = static_cast<size_t>(i * 4);
                        table.fields.push_back(field);
                }
                SdkPanel::DataTableBinding before;
                before.data.resize(12);
                const uint32_t initial[] = {1, 0xFF00, 1};
                memcpy(before.data.data(), initial, sizeof(initial));
                before.dataRevision = 4;
                auto edited = before;
                auto latest = before;
                const uint32_t incoming[] = {99, 0xAB00, 0};
                memcpy(latest.data.data(), incoming, sizeof(incoming));
                latest.callCount = 21;
                latest.status = "new result";
                latest.measuredHz = 500;
                latest.fieldFlashUntil = {9, 8, 7};

                // Merely drawing a stale snapshot must never restore old output
                SdkPanel::mergeDataTableEdits(latest, edited, before.data, before.dataRevision, table, {});
                assert(memcmp(latest.data.data(), incoming, sizeof(incoming)) == 0);

                // Edit an entire integer: merging only changed bytes would leave
                // upper bytes from the concurrent read and produce 0xAB01
                const uint32_t editedValue = 0xFF01;
                memcpy(edited.data.data() + 4, &editedValue, 4);
                ++edited.dataRevision;
                // A one-shot write gate can equal the snapshot but still needs
                // re-arming after the SDK cleared it during the UI frame
                SdkPanel::mergeDataTableEdits(latest, edited, before.data, before.dataRevision, table, {1, 2});
                uint32_t actual[3];
                memcpy(actual, latest.data.data(), sizeof(actual));
                assert(actual[0] == 99 && actual[1] == 0xFF01 && actual[2] == 1);
                assert(latest.dataRevision == 5);
                assert(latest.callCount == 21 && latest.status == "new result" && latest.measuredHz == 500);
                assert(latest.fieldFlashUntil[0] == 9);
        }

        static void linkedTables(const char *dll)
        {
                SdkPanel panel;
                assert(panel.loader_.load(dll));
                panel.parseResult_ = parseHeaderFull(R"(
                    typedef struct fixture_command { uint32_t val; } fixture_command;
                    typedef struct fixture_rt {
                        uint32_t write_command;
                        fixture_command command;
                        uint64_t feedback;
                        double torque;
                        uint32_t values[2];
                    } fixture_rt;
                    int32_t sdk_sync_read_rt(fixture_rt *p);
                    int32_t sdk_sync_write_rt(fixture_rt *p);
                    uint32_t sdk_sync_writes(void);
                    int32_t sdk_fail(fixture_rt *p);
                )");
                const auto *table=findStruct(panel.parseResult_,"fixture_rt");
                assert(table&&table->totalSize==32);
                SdkPanel::DataTableBinding binding;
                binding.name=binding.structName="fixture_rt";binding.localName="SDK0.fixture_rt";
                binding.kind=SdkPanel::DataTableKind::Realtime;binding.data.resize(32);
                binding.readFnIdx=0;binding.writeFnIdx=1;binding.structParamRead=binding.structParamWrite=0;
                panel.dataTables_.push_back(binding);
                SdkPanel::LocalBuffers locals;
                bool concurrentEdit=false;
                const std::string root=binding.localName, command=root+".command.val", torque=root+".torque", array=root+".values[1]";
                const auto put=[&](const std::string &name, const auto &val) {
                        auto &bytes=locals[name];bytes.resize(sizeof(val));memcpy(bytes.data(),&val,sizeof(val));
                };
                const auto scalar=[&](const std::string &name) {uint32_t v{};memcpy(&v,locals.at(name).data(),4);return v;};
                panel.onSnapshotLocalBuffers_=[&] {return locals;};
                std::vector<std::string> sampleSources;
                std::vector<double> sampleTimes;
                panel.onLocalSampleSource_=[&](const auto &name) { sampleSources.push_back(name); };
                panel.onLocalSample_=[&](const auto &name, const auto &bytes, double time) {
                        assert(locals.count(name) && !bytes.empty()); sampleTimes.push_back(time);
                };
                panel.onCompareExchangeLocalBuffer_=[&](const std::string &name,const auto &expected,const auto &desired) {
                        if(concurrentEdit&&name==command) {put(command,uint32_t(43));concurrentEdit=false;}
                        const auto it=locals.find(name);
                        if(it==locals.end()||it->second!=expected)return false;
                        it->second=desired;return true;
                };
                const auto writes=[&] {return panel.loader_.call(panel.parseResult_.functions[2],{}).rawU64;};
                panel.refreshDataTableBindings();
                assert(!panel.dataTables_[0].autoRead&&!panel.dataTables_[0].autoWrite);
                put(torque,0.0);panel.refreshDataTableBindings();
                assert(panel.dataTables_[0].autoRead&&panel.dataTables_[0].autoWrite);
                assert(!panel.dataTables_[0].rootPublished&&locals.count(root)==0);
                assert(!sampleSources.empty() && sampleTimes.empty());
                panel.dataTables_[0].readFnIdx=3;
                panel.runDataTableCall(0,false,false);
                assert(sampleTimes.empty()); // a failed/offline read never advances a waveform
                panel.dataTables_[0].readFnIdx=0;
                panel.runDataTableCall(0,true,false);assert(writes()==0);
                assert(sampleTimes.empty());
                panel.runDataTableCall(0,false,false);
                assert(sampleTimes.size()==1);
                double actualTorque{};memcpy(&actualTorque,locals.at(torque).data(),8);assert(actualTorque==8.3);

                put(command,uint32_t(0));panel.refreshDataTableBindings();panel.runDataTableCall(0,false,false);
                assert(scalar(command)==7);
                put(command,uint32_t(42));panel.runDataTableCall(0,false,false);
                assert(scalar(command)==42);   // pending edit survives a read before the write tick
                panel.runDataTableCall(0,true,false);assert(writes()==1);
                assert(panel.dataTables_[0].data[0]==0);   // callback acknowledged the inferred write_command gate
                panel.runDataTableCall(0,false,false);assert(scalar(command)==42);
                panel.runDataTableCall(0,true,false);panel.runDataTableCall(0,true,false);assert(writes()==1);

                locals[root]=panel.dataTables_[0].data;put(array,uint32_t(0));panel.refreshDataTableBindings();
                panel.runDataTableCall(0,false,false);
                uint64_t wide{};memcpy(&wide,locals.at(root).data()+8,8);assert(wide>0xfedcba9876543200ULL);
                assert(scalar(array)==22);
                put(array,uint32_t(99));panel.runDataTableCall(0,true,false);panel.runDataTableCall(0,false,false);
                assert(scalar(array)==99&&writes()==2);
                concurrentEdit=true;panel.runDataTableCall(0,false,false);assert(scalar(command)==43);
                panel.runDataTableCall(0,true,false);panel.runDataTableCall(0,false,false);
                assert(scalar(command)==43&&writes()==3);

                panel.dataTables_[0].kind=SdkPanel::DataTableKind::Param;panel.refreshDataTableBindings();
                assert(panel.dataTables_[0].autoRead&&panel.dataTables_[0].autoWrite);
                panel.dataTables_[0].actionFieldIdx=0;panel.refreshDataTableBindings();
                assert(!panel.dataTables_[0].autoRead&&!panel.dataTables_[0].autoWrite);
                panel.dataTables_[0].actionFieldIdx=-1;locals.clear();panel.refreshDataTableBindings();
                assert(!panel.dataTables_[0].autoRead&&!panel.dataTables_[0].autoWrite&&panel.dataTables_[0].localBindings.empty());
                puts("SDK linked tables: scalar/root/array reads, automatic changed-value writes, write gates, exact u64, concurrent edits and unlink passed");
        }

        static void worker(const char *dll)
        {
                using Clock = std::chrono::steady_clock;
                SdkPanel panel;
                assert(panel.loader_.load(dll));
                CStructDecl table;
                table.name = table.fullName = "fixture_rt";
                table.totalSize = 4;
                table.fields.push_back({"sample_count", CType::U32, "uint32_t", 0, 4});
                panel.parseResult_.structs.push_back(table);
                CFuncDecl read;
                read.name = "sdk_tick";
                read.retType = CType::I32;
                read.params.push_back({"value", CType::Ptr, "uint32_t *"});
                panel.parseResult_.functions.push_back(read);
                SdkPanel::DataTableBinding binding;
                binding.name = binding.structName = "fixture_rt";
                binding.kind = SdkPanel::DataTableKind::Realtime;
                binding.readFnIdx = 0;
                binding.structParamRead = 0;
                binding.data.resize(4);
                panel.dataTables_.push_back(binding);

                // Repeated immediate stop exercises the stop-before-wait race
                for (int i = 0; i < 200; ++i) {
                        panel.scheduleDataUpdate();
                        if (i % 2)
                                std::this_thread::yield();
                        panel.stopDataUpdate();
                }
                panel.dataTables_[0].autoRead = true;
                panel.rtUpdateHz_ = 1;
                auto calls = [&] {
                        std::lock_guard lock(panel.dataTableMtx_);
                        return panel.dataTables_[0].callCount;
                };
                auto waitCalls = [&](uint64_t count) {
                        const auto deadline = Clock::now() + std::chrono::milliseconds(400);
                        while (calls() < count && Clock::now() < deadline)
                                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        assert(calls() >= count);
                };
                panel.scheduleDataUpdate();
                waitCalls(1);
                const auto faster = Clock::now();
                {
                        std::lock_guard lock(panel.dataTableMtx_);
                        panel.rtUpdateHz_ = 1000;
                }
                panel.dataUpdateCv_.notify_one();
                waitCalls(5); // must not wait for the previous 1-second deadline
                assert(Clock::now() - faster < std::chrono::milliseconds(400));
                {
                        std::lock_guard lock(panel.dataTableMtx_);
                        panel.rtUpdateHz_ = 1;
                }
                panel.dataUpdateCv_.notify_one();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                const auto stop = Clock::now();
                panel.stopDataUpdate();
                assert(Clock::now() - stop < std::chrono::milliseconds(250));

                // Repeated automatic failures are coalesced; manual attempts
                // remain individually visible, even with auto-read enabled
                panel.parseResult_.functions[0].name = "sdk_fail";
                panel.dataLog_.clear();
                panel.dataTables_[0].rateWindowStart = 0;
                panel.dataTables_[0].rateWindowCalls = 0;
                for (int i = 0; i < 50; ++i)
                        panel.runDataTableCall(0, false, false);
                assert(panel.dataLog_.size() == 1);
                panel.runDataTableCall(0, false, true);
                assert(panel.dataLog_.size() == 2);
                puts("SDK worker stop/wakeup/rate-change/log-coalescing regressions passed");
        }
};

static CFuncDecl fn(const char *name, CType ret, std::initializer_list<CType> types)
{
        CFuncDecl result;
        result.name = name;
        result.retType = ret;
        for (auto type : types)
                result.params.push_back({"arg", type, ""});
        return result;
}

int main(int argc, char **argv)
{
        if (argc==2 && std::string(argv[1])=="--presets") {
                SdkPanelRegression::presets();
                return 0;
        }
        assert(argc == 3);
        SdkPanelRegression::run();
        SdkPanelRegression::worker(argv[1]);
        SdkPanelRegression::linkedTables(argv[1]);
        SdkPanelRegression::sampleFreshness(argv[1]);
        SdkPanelRegression::serialPorts(argv[1]);
        SdkLoader loader;
        assert(loader.load(argv[1]));
        auto add = fn("sdk_add", CType::I32, {CType::I32, CType::I32});
        for (int i = 0; i < 1000; ++i) {
                const auto result = loader.call(add, {"-4", "9"});
                assert(result.ok && static_cast<int32_t>(result.rawU64) == 5);
        }
        auto floatFn = fn("sdk_float", CType::F32, {CType::F32});
        assert(loader.call(floatFn, {"1.25"}).rawF64 == 2.5);
        assert(loader.call(fn("sdk_double", CType::F64, {CType::F64}), {"1.25"}).rawF64 == 2.5);
        assert(loader.call(fn("sdk_wide", CType::U64, {CType::U64}), {"0xFEDCBA9876543210"}).rawU64 == 0xFEDCBA9876543210ULL);
        auto many = fn("sdk_many", CType::I64, {CType::I64, CType::I64, CType::I64, CType::I64, CType::I64,
                                               CType::I64, CType::I64, CType::I64, CType::I64});
        assert(loader.call(many, {"1", "2", "3", "4", "5", "6", "7", "8", "9"}).rawU64 == 45);
        int32_t output = 3;
        assert(loader.call(fn("sdk_write", CType::I32, {CType::Ptr}), {""}, {&output}).rawU64 == 10);
        assert(output == 10);
        assert(!loader.call(add, {"1"}).ok);
        assert(!loader.call(add, {"1", "2"}, {&output}).ok);
        assert(!loader.call(fn("missing_symbol", CType::Void, {}), {}).ok);

        // Same symbol with a changed parsed return type must get a fresh CIF
        auto unsignedAdd = add;
        unsignedAdd.retType = CType::U32;
        assert(loader.call(unsignedAdd, {"-1", "-1"}).rawU64 == 0xFFFFFFFEULL);

        CMethodDecl method;
        method.className = method.fullClassName = "Fixture";
        method.name = "call";
        method.retType = CType::I32;
        method.params = {{"value", CType::I32, "int"}};
        loader.setExports({{"sdk_member", "int Fixture::call(int)"}});
        int32_t self = 40;
        assert(loader.callMethod(&self, method, {"2"}, {}).rawU64 == 42);
        // Changing exports invalidates both the method lookup and prepared call
        loader.setExports({{"sdk_static", "int Fixture::call(int)"}});
        method.isStatic = true;
        assert(loader.callMethod(nullptr, method, {"2"}, {}).rawU64 == 7);
        assert(loader.callMethod(nullptr, method, {"3"}, {}).rawU64 == 8);

        loader.unload();
        assert(!loader.call(add, {"1", "2"}).ok);
        assert(loader.exports().empty());
        assert(loader.load(argv[2]));
        assert(loader.call(add, {"1", "2"}).rawU64 == 103);
        assert(loader.load(argv[1]));
        assert(loader.call(add, {"1", "2"}).rawU64 == 3);

        const std::vector<std::string> args{"1", "2"};
        constexpr int count = 200000;
        const auto start = std::chrono::steady_clock::now();
        uint64_t sum = 0;
        for (int i = 0; i < count; ++i)
                sum += loader.call(add, args).rawU64;
        assert(sum == static_cast<uint64_t>(count) * 3);
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        printf("SDK caller regression passed; warm synthetic calls: %.0f calls/s (%.3f us/call)\n",
               count / elapsed, elapsed * 1e6 / count);
        return 0;
}
