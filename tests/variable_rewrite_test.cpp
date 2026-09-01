#include "core/parser.hpp"
#include "core/jlink_port.hpp"
#include "core/type_codec.hpp"
#include "core/variable_rewrite.hpp"
#include "core/safety.hpp"
#include "gui/monitor_channel.hpp"
#include "gui/variable.hpp"
#include "gui/parameter_errors.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <fstream>
#include <cstdlib>
#include <cmath>

log_t             g_log_ops{};
log_t             g_log_perf{};
std::atomic<bool> g_appRunning{false};
int
get_log_idx()
{
        return 0;
}

struct RttReconnectTestAccess {
        static void state(bool connected, u64 epoch)
        {
                auto &port = JLinkPort::instance();
                port.isConnected_ = connected;
                port.targetEpoch_ = epoch;
        }
};

struct VariableRewriteRegression {

        static void diagnosticProfile(const char *axf, const char *json)
        {
                ElfParser parser; assert(parser.parse(axf));
                Variable v("diagnostic-profile");v.dwarfInfo_=parser.getDwarfInfo();
                const auto profile=safety::load(json);
                unsigned roots=0,fields=0,typed=0,errors=0;
                for (const auto &group:profile.groups) {
                        const auto leaves=v.diagnosticFields(group.root);
                        if(leaves.empty()){std::printf("MISSING ROOT: %s\n",group.root.c_str());++errors;continue;}
                        ++roots;
                        for(const auto &field:group.fields){
                                const auto path=group.root+(field.path.empty()||field.path.starts_with('[')?"":".")+field.path;
                                const auto found=v.diagnosticFields(path);
                                if(found.size()!=1||!found[0].supported){std::printf("MISSING/UNSUPPORTED FIELD: %s\n",path.c_str());++errors;continue;}
                                ++fields;
                                const auto *decoder=safety::findDecoder(profile,field.decoder);
                                if(!decoder||decoder->kind!="number"||decoder->type=="auto")continue;
                                ++typed;bool match=false;
                                for(const auto &[id,type]:v.dwarfInfo_.types){
                                        if(type.kind!=dwarf::TypeKind::BASE||type.name!=found[0].type)continue;
                                        const bool kind=decoder->type=="bool"?type.encoding==2:
                                                decoder->type.starts_with('f')?type.encoding==4:
                                                decoder->type.starts_with('i')?(type.encoding==5||type.encoding==6):
                                                (type.encoding==7||type.encoding==8);
                                        if(kind&&(decoder->type=="bool"||type.size*8==decoder->width))match=true;
                                }
                                if(!match){std::printf("TYPE MISMATCH: %s (%s) decoder=%s (%s/%u)\n",path.c_str(),found[0].type.c_str(),decoder->id.c_str(),decoder->type.c_str(),decoder->width);++errors;}
                        }
                }
                std::printf("Profile check: %u roots, %u explicit fields, %u explicit number types, %u errors\n",roots,fields,typed,errors);
                assert(errors==0);
        }

        static void diagnosticValues()
        {
                Variable v("diagnostic-values");
                auto scalar = [&](u64 id, u64 size, u32 encoding) {
                        dwarf::Type t; t.kind=dwarf::TypeKind::BASE; t.size=size; t.encoding=encoding;
                        v.dwarfInfo_.types[id]=t;
                };
                scalar(1,8,7); scalar(2,8,5); scalar(3,4,4); scalar(4,8,4);
                scalar(5,1,2); scalar(6,2,5); scalar(7,4,7);
                dwarf::Type enumeration; enumeration.kind=dwarf::TypeKind::ENUM;
                enumeration.inner=6; enumeration.size=2; enumeration.name="Stage";
                enumeration.enums={{"FAILED",-2},{"READY",7}}; v.dwarfInfo_.types[8]=enumeration;
                dwarf::Type array; array.kind=dwarf::TypeKind::ARRAY; array.inner=7; array.dims={3,11};
                v.dwarfInfo_.types[9]=array;
                dwarf::Type config; config.kind=dwarf::TypeKind::STRUCT; config.size=256;
                config.members={{"big",0,1},{"negative",8,2},{"speed",16,3},{"energy",24,4},
                                {"ready",32,5},{"stage",34,8},{"bits",36,7,6,31},{"samples",48,9}};
                v.dwarfInfo_.types[10]=config;
                dwarf::Type alias;alias.kind=dwarf::TypeKind::TYPEDEF;alias.inner=10;v.dwarfInfo_.types[11]=alias;
                v.dwarfInfo_.variables.push_back({"controller",0x20000000,11,true});
                VarEntry entry{}; entry.name="controller";entry.port=PortType::LOCAL;entry.typeOff=11;
                entry.isStruct=true;entry.type=DataType::ARRAY;entry.addr=0x20000000;v.vars_.push_back(entry);
                auto &bytes=v.localBufs_["controller"];bytes.resize(256);
                auto put=[&](size_t offset,const auto &val){std::memcpy(bytes.data()+offset,&val,sizeof(val));};
                put(0,UINT64_MAX);put(8,INT64_MIN);put(16,-0.125f);put(24,1.0e-120);put(32,u8{1});put(34,i16{-2});
                put(36,u64{0x456789ab}<<6);put(48+(2*11+10)*4,u32{123456789});
                const auto fields=v.diagnosticFields("controller");assert(fields.size()==40);
                assert(v.diagnosticFields("controller.samples[1]").size()==11);
                assert(v.diagnosticFields("controller.samples[2][10]").size()==1);
                assert(v.diagnosticFields("missing").empty());
                assert(v.diagnosticFields("controller.samples[3]").empty());
                diagnostics::Value val;std::string status;
                auto read=[&](const char *name){assert(v.readDiagnosticValue(name,val,status));};
                read("controller.big");assert(val.bits==UINT64_MAX&&val.width==64&&diagnostics::formatValue(val)=="18446744073709551615");
                read("controller.negative");assert(diagnostics::signValue(val)==INT64_MIN);
                read("controller.speed");assert(val.kind==diagnostics::Kind::Floating&&val.real==-0.125);
                read("controller.energy");assert(val.real==1.0e-120);
                read("controller.ready");assert(val.kind==diagnostics::Kind::Boolean&&val.bits==1);
                read("controller.stage");assert(diagnostics::signValue(val)==-2&&val.enums.size()==2);
                read("controller.bits");assert(val.bits==0x456789ab&&val.width==31);
                read("controller.samples[2][10]");assert(val.bits==123456789);
                u32 word=0;assert(v.readDiagnosticWord("controller.bits",word,status)&&word==0x456789ab);
                assert(!v.readDiagnosticWord("controller.big",word,status)&&word==0);
                assert(!v.readDiagnosticValue("controller.samples[3][0]",val,status)&&val.width==0&&val.bits==0);
                const auto snapshot=bytes;
                v.vars_[0].port=PortType::JLINK;
                assert(!v.readDiagnosticValue("controller.big",val,status)&&val.width==0);
                assert(bytes==snapshot);   // Diagnostic access never writes to the source
                v.poll_=std::make_shared<Variable::PollState>();
                Variable::PollVal sample{};sample.sz=8;sample.ok=true;sample.epoch=42;sample.timestamp=get_mono_ts_ms();
                const u64 expected=UINT64_MAX;std::memcpy(sample.buf,&expected,8);
                v.poll_->vals[0x20000000]=sample;
                const auto oldEpoch=JLinkPort::instance().targetEpoch();
                RttReconnectTestAccess::state(true,42);
                assert(v.readDiagnosticValue("controller.big",val,status)&&val.bits==UINT64_MAX);
                RttReconnectTestAccess::state(false,42);
                assert(!v.readDiagnosticValue("controller.big",val,status)&&val.width==0);
                RttReconnectTestAccess::state(true,43);
                assert(!v.readDiagnosticValue("controller.big",val,status)&&val.width==0);
                v.poll_->vals[0x20000000].epoch=43;v.poll_->vals[0x20000000].timestamp=0;
                assert(!v.readDiagnosticValue("controller.big",val,status)&&val.width==0);
                v.poll_->vals[0x20000000].timestamp=get_mono_ts_ms();v.poll_->vals[0x20000000].ok=false;
                assert(!v.readDiagnosticValue("controller.big",val,status)&&val.width==0);
                assert(status == "读取失败");
                auto &cached = v.poll_->vals[0x20000000];
                cached.ok = true; cached.timestamp = get_mono_ts_ms() + 1000;
                assert(!v.readDiagnosticMemory(0x20000000,4,word,status));
                assert(status == "等待刷新, 旧值已过期");
                cached.timestamp = 0;
                assert(!v.readDiagnosticMemory(0x20000000,4,word,status));
                cached.timestamp = get_mono_ts_ms();
                assert(v.readDiagnosticMemory(0x20000000,4,word,status) && word == UINT32_MAX && status.empty());
                RttReconnectTestAccess::state(false,oldEpoch);
                assert(!v.readDiagnosticMemory(0x20000000,4,word,status));
                assert(status.find("连接已断开") != std::string::npos);
                v.poll_.reset();
                std::puts("PASS: generic read-only diagnostics, full multidimensional arrays, signed/unsigned 64-bit, floating, enum, packed bitfield, disconnect/epoch/expiry");
        }

        static void manualTarget(const char *axf, const char *profiles)
        {
                assert(!JLinkPort::instance().isConnected());
                ElfParser parser;
                assert(parser.parse(axf));
                Variable v("manual-target");
                v.elfPath_ = axf;
                v.elfInfo_ = parser.getElfInfo();
                v.dwarfInfo_ = parser.getDwarfInfo();
                v.rebuildSearchPool();
                v.parameterDirectory_ = profiles;
                const auto schemaPath = std::filesystem::path(profiles) / "param_list.json";
                std::ifstream schemaInput(schemaPath, std::ios::binary);
                const std::string schemaText((std::istreambuf_iterator<char>(schemaInput)), {});
                const auto schema = parameter_json::parse(schemaText);
                const auto symbol = std::find_if(v.searchPool_.begin(), v.searchPool_.end(), [&](const auto &entry) {
                        return entry.path == schema.target;
                });
                assert(symbol != v.searchPool_.end());
                const auto type = v.dwarfInfo_.types.at(symbol->typeOff);
                for (const std::string alias : {"drive_a.settings", "custom_controller.parameters"}) {
                        VarEntry watch{};
                        watch.name = alias;
                        watch.addr = symbol->addr;
                        watch.typeOff = symbol->typeOff;
                        watch.type = DataType::ARRAY;
                        watch.isStruct = true;
                        watch.port = PortType::LOCAL;
                        watch.writable = true;
                        v.vars_.push_back(watch);
                        v.localBufs_[alias].resize(type.size);
                        assert(v.prepareParameterDevice(alias));
                        assert(v.rewrite_.isFocCfg && v.rewrite_.guardedFoc);
                        assert(v.rewrite_.stateAddress == v.parameterFirmwareIdentity().field(64));
                        size_t count = 0;
                        for (const auto &file : std::filesystem::directory_iterator(profiles)) {
                                if (file.path().extension() != ".json" || file.path().filename() == "param_list_manifest.json")
                                        continue;
                                std::ifstream input(file.path(), std::ios::binary);
                                const std::string text((std::istreambuf_iterator<char>(input)), {});
                                auto doc = parameter_json::parse(text);
                                doc.target = alias;
                                if (!v.importRewriteJson(parameter_json::format(doc))) {
                                        std::fprintf(stderr,"%s: %s\n",file.path().string().c_str(),v.rewrite_.status.c_str());
                                        assert(false);
                                }
                                assert(v.rewrite_.jsonFields.size() == doc.fields.size());
                                const auto imported = v.rewrite_.bytes;
                                const auto exported = v.exportRewriteJson();
                                const auto result = parameter_json::parse(exported);
                                assert(result.target == alias && result.fields.size() == doc.fields.size());
                                assert(v.importRewriteJson(exported));
                                assert(v.rewrite_.bytes == imported);
                                ++count;
                        }
                        assert(count >= 20);
                        std::printf("PASS: %s, %zu real firmware profiles, all fields imported/exported byte-for-byte\n",alias.c_str(),count);
                }
                assert(!v.prepareParameterDevice("not_an_existing_variable"));
                assert(!v.prepareParameterDevice(schema.target)); // Disconnected probe: bind, but do not read
                assert(v.rewrite_.variableName == schema.target && v.rewrite_.typeOff == symbol->typeOff);
                assert(v.rewrite_.guardedFoc && v.rewrite_.source.port == PortType::JLINK);
                std::puts("PASS: unknown target rejected; no J-Link connection or device writes");
        }

        static void jsonUnionPaths()
        {
                Variable v("json-paths");
                dwarf::Type scalar;
                scalar.kind=dwarf::TypeKind::BASE; scalar.size=4; scalar.encoding=7;
                v.dwarfInfo_.types[1]=scalar;
                dwarf::Type phase; phase.kind=dwarf::TypeKind::STRUCT; phase.size=4;
                phase.members.push_back({"ready",0,1,0,1});
                v.dwarfInfo_.types[2]=phase;
                dwarf::Type fault; fault.kind=dwarf::TypeKind::STRUCT; fault.size=4;
                fault.members.push_back({"tripped",0,1,7,1});
                v.dwarfInfo_.types[3]=fault;
                dwarf::Type views; views.kind=dwarf::TypeKind::UNION; views.size=4;
                views.members={{"storage",0,1},{"phase",0,2},{"fault",0,3}};
                v.dwarfInfo_.types[4]=views;
                dwarf::Type root; root.kind=dwarf::TypeKind::STRUCT; root.size=4;
                root.members={{"status",0,4}};
                v.dwarfInfo_.types[5]=root;
                VarEntry watch{};watch.name="selected_device.options";watch.typeOff=5;watch.type=DataType::ARRAY;
                watch.isStruct=true;watch.port=PortType::LOCAL;watch.writable=true;
                v.vars_.push_back(watch);v.localBufs_[watch.name]={0x40,0,0,0x80};
                assert(v.prepareParameterDevice(watch.name));
                auto doc=parameter_json::parse(R"({"format":"foc-parameters","version":1,"target":"selected_device.options","parameters":[{"path":"status.phase.ready","type":"u32","value":"1"},{"path":"status.fault.tripped","type":"u32","value":"1"}]})");
                const bool imported = v.importRewriteJson(parameter_json::format(doc));
                if (!imported) std::fprintf(stderr, "%s\n", v.rewrite_.status.c_str());
                assert(imported);
                assert((v.rewrite_.bytes==std::vector<u8>{0xc1,0,0,0x80}));
                assert((v.localBufs_[watch.name]==std::vector<u8>{0x40,0,0,0x80}));
                auto exported=parameter_json::parse(v.exportRewriteJson());
                assert(exported.fields.size()==2);
                assert(std::any_of(exported.fields.begin(),exported.fields.end(),[](const auto &f){return f.row.path=="status.fault.tripped";}));
                const auto before=v.rewrite_.bytes;
                doc.fields.back().row.path="status.nonexistent.tripped";
                assert(!v.importRewriteJson(parameter_json::format(doc)) && v.rewrite_.bytes==before);
                doc.fields.back().row.path="status.fault.tripped";doc.fields.back().row.val="2";
                assert(!v.importRewriteJson(parameter_json::format(doc)) && v.rewrite_.bytes==before);
                v.parameterFile_.load(parameter_json::format(exported));
                parameter_json::Json original(cJSON_Parse(v.parameterFile_.source.c_str()), cJSON_Delete);
                cJSON_AddStringToObject(original.get(), "source_marker", "keep");
                char *raw = cJSON_Print(original.get());
                v.parameterFile_.load(raw);
                cJSON_free(raw);
                v.setParameterTarget("another_device.parameters");
                assert(v.parameterFile_.document.target=="another_device.parameters" && v.parameterFile_.dirty);
                assert(v.parameterTargetOverride_=="another_device.parameters" && v.parameterDeviceValues_.empty());
                const auto saved=v.parameterFile_.save();
                parameter_json::Json savedRoot(cJSON_Parse(saved.c_str()), cJSON_Delete);
                assert(parameter_json::string(savedRoot.get(), "target")=="another_device.parameters");
                assert(parameter_json::string(savedRoot.get(), "source_marker")=="keep");
                const auto switched=std::filesystem::current_path()/"manual-target-switch.json";
                {std::ofstream output(switched,std::ios::binary);output<<v.parameterFile_.source;}
                assert(v.loadParameterFile(switched.string()));
                assert(v.parameterFile_.document.target=="another_device.parameters");
                assert(parameter_json::parse(v.parameterFile_.save()).target=="another_device.parameters");
                std::filesystem::remove(switched);
                v.parameterFileOpen_=true;
                draw(v);
                std::puts("PASS: JSON selects arbitrary union views; unrelated bits preserved; invalid path/overflow rollback; manual target UI");
        }

        static void elfReload(const char *path)
        {
                Variable v("reloadRegression");
                assert(v.loadElf(path));
                v.elfLoadThread_.join();
                assert(!v.dwarfInfo_.present && !v.consumeElfReloaded());
                v.applyPendingElf();
                assert(v.elfLoadError_.empty() && v.dwarfInfo_.present && !v.searchPool_.empty());
                assert(v.consumeElfReloaded() && !v.consumeElfReloaded());
                // File-local symbols can have duplicate names and unstable ordering
                const auto selected = std::find_if(v.searchPool_.begin(), v.searchPool_.end(),
                    [](const SearchEntry &entry) { return entry.path == "g_cia402.in.mode_of_op"; });
                assert(selected != v.searchPool_.end());
                const auto symbol = *selected;
                VarEntry watch;
                watch.name = symbol.path;
                watch.port = PortType::JLINK;
                watch.type = symbol.type;
                watch.typeOff = symbol.typeOff;
                v.vars_.push_back(watch);
                v.updateIntervalMs_ = 0;
                size_t reads = 0;
                for (int pass = 0; pass < 30; ++pass) {
                        const auto generation = v.elfLayoutGeneration_;
                        const auto *types = &*v.getDwarfInfo().types.begin();
                        const auto *pool = v.searchPool_.data();
                        v.vars_[0].addr = 0;
                        assert(v.loadElf(path));
                        do {
                                // Same read path as the crash, with a disconnected
                                // probe: no hardware connection or target writes
                                v.updateVariables();
                                assert(v.elfLayoutGeneration_ == generation);
                                assert(&*v.getDwarfInfo().types.begin() == types);
                                assert(v.searchPool_.data() == pool && v.vars_[0].addr == 0);
                                assert(!v.consumeElfReloaded());
                                ++reads;
                                std::this_thread::yield();
                        } while (v.isElfLoading_);
                        v.elfLoadThread_.join();
                        assert(v.elfLayoutGeneration_ == generation && v.vars_[0].addr == 0);
                        v.applyPendingElf();
                        assert(v.elfLayoutGeneration_ == generation + 1);
                        assert(v.vars_[0].addr == symbol.addr && v.vars_[0].typeOff == symbol.typeOff);
                        assert(v.consumeElfReloaded() && !v.consumeElfReloaded());
                }
                const auto generation = v.elfLayoutGeneration_;
                const auto *types = &*v.getDwarfInfo().types.begin();
                assert(v.loadElf(std::string(path) + ".missing-reload-test"));
                v.elfLoadThread_.join();
                v.applyPendingElf();
                assert(!v.elfLoadError_.empty() && v.elfLayoutGeneration_ == generation);
                assert(&*v.getDwarfInfo().types.begin() == types && !v.consumeElfReloaded());
                assert(v.loadElf(path));
                v.currentLoadingTask_->aborted = true;
                v.elfLoadThread_.join();
                v.applyPendingElf();
                assert(!v.isElfLoading_ && v.elfLayoutGeneration_ == generation && !v.consumeElfReloaded());
                // Destruction while a reload is in flight also joins the worker
                assert(v.loadElf(path));
                std::printf("PASS: 30 asynchronous AXF reloads with %zu watch updates; GUI-only publication, failure and cancellation\n", reads);
        }
        static void perVariableRadix()
        {
                Variable v("radixRegression");
                v.updateIntervalMs_ = 0;
                for (int r : {0,10,16,8,2}) {
                        VarEntry row{};
                        row.name = "integer" + std::to_string(r);
                        row.port = PortType::LOCAL; row.type = DataType::I8; row.displayRadix = r;
                        v.vars_.push_back(row);
                        v.localBufs_[row.name] = {0xfe};
                }
                v.displayRadix_ = 16;
                v.updateVariables();
                const char *expected[] = {"-2", "-2", "0xFE", "0o376", "0b11111110"};
                for(size_t i=0;i<v.vars_.size();++i) assert(v.vars_[i].valueStr==expected[i]);
                v.displayRadix_ = 10;
                v.prevUpdateTs_ = 0; v.updateVariables();
                assert(v.vars_[0].valueStr=="-2" && v.vars_[2].valueStr=="0xFE");
                v.vars_[2].enumDefs.push_back({"ENUM_VALUE", -2});
                v.prevUpdateTs_ = 0; v.updateVariables();
                assert(v.vars_[2].valueStr=="0xFE");
                parameter_json::Json saved(cJSON_CreateObject(),cJSON_Delete); v.save(saved.get());
                Variable restored("radixRestored"); restored.load(saved.get());
                assert(restored.vars_.size()==v.vars_.size());
                for(size_t i=0;i<v.vars_.size();++i) assert(restored.vars_[i].displayRadix==v.variableRadix(v.vars_[i]));
                cJSON_ReplaceItemInObject(cJSON_GetArrayItem(cJSON_GetObjectItem(saved.get(),"vars"),0),"displayRadix",cJSON_CreateNumber(7));
                restored.load(saved.get()); assert(restored.vars_[0].displayRadix==10);
                std::puts("PASS: independent value formats, no manager fallback, signed bit patterns, enum numeric display and persistence");
        }
        static void parameterErrorLanguage()
        {
                const auto previous = g_lang;
                const std::string readError = "Cannot read device parameter descriptor; check J-Link connection and retry";
                const std::string layoutError = "Device parameter layout or addresses do not match AXF; load a compatible AXF";
                const std::string nested = "Parameter #17 [motor.poles]: value outside min/max";
                g_lang = Lang::EN;
                assert(parameterError(readError) == readError && parameterError(layoutError) == layoutError);
                assert(parameterError(nested) == nested);
                g_lang = Lang::ZH;
                assert(parameterError(readError) == "无法读取设备参数描述信息，请检查 J-Link 连接后重试");
                assert(parameterError(layoutError) == "设备参数布局或地址与 AXF 不匹配，请加载兼容的 AXF");
                assert(parameterError(nested) == "参数序号 17 [motor.poles]: 数值超出最小值或最大值范围");
                assert(parameterError("g_foc.cfg.mode: Preset value is not a declared enum option") ==
                       "g_foc.cfg.mode: 预设数值不是已声明的枚举选项");
                assert(parameterError("Parameter ID 123: path/type/revision differs from the current firmware schema") ==
                       "参数 ID 123：路径、类型或修订版本与当前固件参数模式不一致");
                assert(parameterError("AXF is missing the FOC parameter descriptor; rebuild firmware") ==
                       "AXF 缺少 FOC 参数描述信息，请重新构建固件");
                Variable variable("error-language-regression");
                assert(!variable.importRewriteJson("{"));
                assert(variable.rewrite_.status == "参数 JSON 无效");
                assert(!variable.importRewriteCsv("wrong header"));
                assert(variable.rewrite_.status == "CSV 表头必须为：path,type,default,unit,min,max,description");
                g_lang = Lang::EN;
                assert(parameterError(readError) == readError);
                assert(!variable.importRewriteJson("{"));
                assert(variable.rewrite_.status == "Invalid parameter JSON");
                assert(!variable.importRewriteCsv("wrong header"));
                assert(variable.rewrite_.status == "Expected CSV header: path,type,default,unit,min,max,description");
                g_lang = previous;
                std::puts("PASS: parameter errors follow UI language; nested diagnostics retain field paths and IDs");
        }
        static void parameterCompatibility()
        {
                firmware_identity::Image image;
                image.descriptorAddress = 0x08001000;
                for (size_t i = 0; i < image.descriptor.size(); ++i)
                        image.descriptor[i] = static_cast<uint8_t>(i);
                auto device = image.descriptor;
                auto read = [&](uint32_t address, uint32_t size, uint8_t *bytes) {
                        assert(address == image.descriptorAddress && size == device.size());
                        std::copy(device.begin(), device.end(), bytes);
                        return true;
                };
                image.verify(read);
                // A rebuild can change defaults, firmware version, timestamp and executable bytes
                for (size_t i = 24; i < 60; ++i) device[i] ^= 0xff;
                for (size_t i = 68; i < 92; ++i) device[i] ^= 0xff;
                image.segments.push_back({0x08002000, 0, 16});
                image.bytes.assign(16, 0xa5);
                image.verify(read); // Reader only accepts the descriptor, never executable reads
                // Every ABI byte and both RAM pointers must still match
                for (size_t i = 0; i < device.size(); ++i) {
                        if (i >= 24 && (i < 60 || i >= 68)) continue;
                        device[i] ^= 1;
                        bool rejected = false;
                        try { image.verify(read); }
                        catch (const std::runtime_error &) { rejected = true; }
                        assert(rejected);
                        device[i] ^= 1;
                }
                bool failedRead = false;
                try { image.verify([](uint32_t, uint32_t, uint8_t *) { return false; }); }
                catch (const std::runtime_error &e) {
                        failedRead = std::string(e.what()).find("Cannot read device parameter descriptor") != std::string::npos;
                }
                assert(failedRead);
                std::puts("PASS: compatible rebuilds accepted; ABI/address mismatches and descriptor read failures rejected");
        }
        static void sameSessionVariables(const Variable &actual, const Variable &expected)
        {
                assert(actual.vars_.size() == expected.vars_.size());
                for (size_t i = 0; i < expected.vars_.size(); ++i) {
                        const auto &a = actual.vars_[i];
                        const auto &e = expected.vars_[i];
                        assert(a.name == e.name && a.addr == e.addr && a.type == e.type);
                        assert(a.port == e.port && a.writable == e.writable && a.typeOff == e.typeOff);
                }
        }
        static void sessionRoundtrip(const Variable &source)
        {
                parameter_json::Json saved(cJSON_CreateObject(), cJSON_Delete);
                source.save(saved.get());
                const auto *records = cJSON_GetObjectItemCaseSensitive(saved.get(), "vars");
                assert(cJSON_IsArray(records));
                for (const auto *record = records->child; record; record = record->next) {
                        assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(record, "addr")));
                        assert(!cJSON_GetObjectItemCaseSensitive(record, "address"));
                }
                char *text = cJSON_PrintUnformatted(saved.get());
                assert(text);
                parameter_json::Json parsed(cJSON_Parse(text), cJSON_Delete);
                cJSON_free(text);
                assert(parsed);
                Variable restored("session-roundtrip");
                restored.load(parsed.get());
                sameSessionVariables(restored, source);
        }
        static void sessionLoad()
        {
                // Keep a missing addr near the start: the pre-fix loader dereferences
                // a null cJSON pointer here, independently of ELF or device access
                parameter_json::Json document(cJSON_Parse(R"({"vars":[
                    {"name":"valid_before","type":1,"addr":536875008,"writable":true},
                    {"name":"missing_addr","type":5},
                    {"name":"renamed_address_key","type":5,"address":536875012},
                    {"type":5,"addr":536875012},
                    {"name":null,"type":5,"addr":536875012},
                    {"name":42,"type":5,"addr":536875012},
                    {"name":{},"type":5,"addr":536875012},
                    {"name":"missing_type","addr":536875012},
                    {"name":"string_type","type":"u32","addr":536875012},
                    {"name":"null_type","type":null,"addr":536875012},
                    {"name":"boolean_type","type":true,"addr":536875012},
                    {"name":"negative_type","type":-1,"addr":536875012},
                    {"name":"unknown_enum_value","type":13,"addr":536875012},
                    {"name":"fractional_type","type":5.5,"addr":536875012},
                    {"name":"overflow_type","type":1e999,"addr":536875012},
                    {"name":"negative_addr","type":5,"addr":-1},
                    {"name":"u64_limit_addr","type":5,"addr":18446744073709551616},
                    {"name":"overflow_addr","type":5,"addr":1e999},
                    {"name":"fractional_addr","type":5,"addr":536875012.5},
                    {"name":"string_addr","type":5,"addr":"536875012"},
                    {"name":"null_addr","type":5,"addr":null},
                    {"name":"boolean_addr","type":5,"addr":false},
                    null,42,[],
                    {"name":"valid_after","type":8,"addr":536875016}
                ]})"), cJSON_Delete);
                assert(document);
                auto *nanRecord = cJSON_CreateObject();
                cJSON_AddStringToObject(nanRecord, "name", "nan_addr");
                cJSON_AddNumberToObject(nanRecord, "type", static_cast<int>(DataType::U32));
                cJSON_AddNumberToObject(nanRecord, "addr", 0)->valuedouble = std::numeric_limits<double>::quiet_NaN();
                cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(document.get(), "vars"), nanRecord);
                Variable loaded("session-load-regression");
                loaded.load(document.get());
                assert(loaded.vars_.size() == 2);
                assert(loaded.vars_[0].name == "valid_before" && loaded.vars_[0].addr == 536875008);
                assert(loaded.vars_[0].type == DataType::F32 && loaded.vars_[0].writable);
                assert(loaded.vars_[1].name == "valid_after" && loaded.vars_[1].addr == 536875016);
                assert(loaded.vars_[1].type == DataType::I16 && !loaded.vars_[1].writable);
                sessionRoundtrip(loaded);

                // A second load replaces prev rows. Cover every persisted enum
                // value, genuine zero and the largest double below the u64 limit
                parameter_json::Json valid(cJSON_CreateObject(), cJSON_Delete);
                auto *records = cJSON_AddArrayToObject(valid.get(), "vars");
                for (int type = static_cast<int>(DataType::UNKNOWN); type <= static_cast<int>(DataType::F16); ++type) {
                        auto *record = cJSON_CreateObject();
                        cJSON_AddStringToObject(record, "name", ("valid_type_" + std::to_string(type)).c_str());
                        cJSON_AddNumberToObject(record, "type", type);
                        cJSON_AddNumberToObject(record, "addr", type == 0 ? 0 : 536875008 + type * 8);
                        cJSON_AddItemToArray(records, record);
                }
                const double prevAddress = std::nextafter(std::ldexp(1.0, 64), 0.0);
                auto *prev = cJSON_GetArrayItem(records, cJSON_GetArraySize(records) - 1);
                cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(prev, "addr"), prevAddress);
                loaded.load(valid.get());
                assert(loaded.vars_.size() == static_cast<size_t>(DataType::F16) + 1);
                for (size_t i = 0; i < loaded.vars_.size(); ++i) {
                        assert(loaded.vars_[i].name == "valid_type_" + std::to_string(i));
                        assert(loaded.vars_[i].type == static_cast<DataType>(i));
                }
                assert(loaded.vars_.front().addr == 0);
                assert(loaded.vars_.back().addr == static_cast<u64>(prevAddress));
                sessionRoundtrip(loaded);

                parameter_json::Json optional(cJSON_Parse(R"({"vars":[{
                    "name":"optional_strings","type":5,"addr":536875008,"port":2,"shmName":42,
                    "memberEnumDefs":{"optional_strings.field":[
                        {"name":42,"value":1},{"value":2},{"name":"VALID","value":3}
                    ]}
                }]})"), cJSON_Delete);
                assert(optional);
                loaded.load(optional.get());
                assert(loaded.vars_.size() == 1 && loaded.vars_[0].addr == 536875008);
                assert(loaded.vars_[0].shm.name[0] == '\0');
                const auto &definitions = loaded.vars_[0].memberEnumDefs.at("optional_strings.field");
                assert(definitions.size() == 1 && definitions[0].name == "VALID" && definitions[0].val == 3);
                sessionRoundtrip(loaded);
                std::puts("PASS: session load rejects malformed names/types/addresses, preserves valid rows and u64 bounds, and roundtrips canonical addr keys");
        }
        static void sessionFile(const char *path, const char *elfPath)
        {
                std::ifstream input(path, std::ios::binary);
                assert(input);
                const std::string text((std::istreambuf_iterator<char>(input)), {});
                parameter_json::Json document(cJSON_Parse(text.c_str()), cJSON_Delete);
                assert(document);
                const auto *windows = cJSON_GetObjectItemCaseSensitive(document.get(), "Variables");
                assert(cJSON_IsArray(windows) && cJSON_GetArraySize(windows) > 0);
                size_t variables = 0, resolved = 0, chs = 0;
                for (const auto *window = windows->child; window; window = window->next) {
                        Variable loaded("real-session");
                        const auto *records = cJSON_GetObjectItemCaseSensitive(window, "vars");
                        assert(cJSON_IsArray(records));
                        for (const auto *record = records->child; record; record = record->next) {
                                assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(record, "name")));
                                assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(record, "type")));
                                assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(record, "addr")));
                                assert(!cJSON_GetObjectItemCaseSensitive(record, "address"));
                        }
                        loaded.load(window);
                        assert(loaded.vars_.size() == static_cast<size_t>(cJSON_GetArraySize(records)));
                        for (size_t i = 0; i < loaded.vars_.size(); ++i) {
                                const auto *record = cJSON_GetArrayItem(records, static_cast<int>(i));
                                const auto &entry = loaded.vars_[i];
                                assert(entry.name == cJSON_GetObjectItemCaseSensitive(record, "name")->valuestring);
                                assert(entry.type == static_cast<DataType>(cJSON_GetObjectItemCaseSensitive(record, "type")->valueint));
                                assert(entry.addr == static_cast<u64>(cJSON_GetObjectItemCaseSensitive(record, "addr")->valuedouble));
                        }
                        variables += loaded.vars_.size();
                        sessionRoundtrip(loaded);
                        if (elfPath) {
                                // Exercise the real asynchronous ELF loader on restored rows,
                                // including address resynchronization, without starting polling
                                const auto restoredRows = loaded.vars_;
                                assert(loaded.loadElf(elfPath));
                                loaded.elfLoadThread_.join();
                                loaded.applyPendingElf();
                                assert(!loaded.isElfLoading_ && loaded.elfLoadError_.empty());
                                assert(loaded.dwarfInfo_.present && !loaded.searchPool_.empty());
                                assert(loaded.vars_.size() == static_cast<size_t>(cJSON_GetArraySize(records)));
                                for (size_t i = 0; i < loaded.vars_.size(); ++i) {
                                        const auto &entry = loaded.vars_[i];
                                        assert(entry.name == restoredRows[i].name && entry.type == restoredRows[i].type);
                                        if (entry.port != PortType::JLINK) continue;
                                        const auto found = std::find_if(loaded.searchPool_.begin(), loaded.searchPool_.end(),
                                            [&](const SearchEntry &symbol) { return symbol.path == entry.name; });
                                        if (found != loaded.searchPool_.end()) {
                                                assert(entry.addr == found->addr && entry.typeOff == found->typeOff);
                                                ++resolved;
                                        } else {
                                                assert(entry.addr == restoredRows[i].addr);
                                        }
                                }
                                sessionRoundtrip(loaded);
                        }
                }
                const auto *monitors = cJSON_GetObjectItemCaseSensitive(document.get(), "monitors");
                assert(cJSON_IsArray(monitors));
                for (const auto *monitor = monitors->child; monitor; monitor = monitor->next) {
                        const auto *scopes = cJSON_GetObjectItemCaseSensitive(monitor, "scopes");
                        assert(cJSON_IsArray(scopes));
                        for (const auto *scope = scopes->child; scope; scope = scope->next) {
                                const auto *entries = cJSON_GetObjectItemCaseSensitive(scope, "channels");
                                if (!entries)
                                        entries = cJSON_GetObjectItemCaseSensitive(scope, "chs");
                                assert(cJSON_IsArray(entries));
                                for (const auto *entry = entries->child; entry; entry = entry->next) {
                                        const auto *address = cJSON_GetObjectItemCaseSensitive(entry, "addr");
                                        // Monitor chs persist hexadecimal strings; watch
                                        // variables use numeric addr values in the same session
                                        assert(cJSON_IsString(address) || cJSON_IsNumber(address));
                                        assert(!cJSON_GetObjectItemCaseSensitive(entry, "address"));
                                        ++chs;
                                }
                        }
                }
                assert(variables > 0 && chs > 0 && (!elfPath || resolved > 0));
                std::printf("PASS: real session loads and roundtrips %zu watch variables, validates %zu ch addr keys, resolves %zu ELF-backed rows\n",
                            variables, chs, resolved);
        }
        static void draw(Variable &v, std::string *parameterLog = nullptr)
        {
                ImGui::CreateContext();
                auto &io       = ImGui::GetIO();
                io.IniFilename = nullptr;
                io.DisplaySize = ImVec2(1280, 960);
                io.DeltaTime   = 1.0f / 60;
                unsigned char *pixels;
                int            width, height;
                io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
                for (int frame = 0; frame < 3; ++frame) {
                        ImGui::NewFrame();
                        ImGui::Begin("Regression");
                        v.drawRewriteDialog();
                        const bool capture = parameterLog && frame == 2;
                        if (capture)
                                ImGui::LogToBuffer(0);
                        v.drawParameterFileEditor();
                        if (capture) {
                                *parameterLog = ImGui::GetCurrentContext()->LogBuffer.c_str();
                                ImGui::LogFinish();
                        }
                        ImGui::End();
                        assert(ImGui::GetCurrentContext()->OpenPopupStack.empty());
                        if (v.rewrite_.open) {
                                const auto *window = ImGui::FindWindowByName(("###RewriteVariable" + v.name_).c_str());
                                assert(window && !(window->Flags & (ImGuiWindowFlags_Modal | ImGuiWindowFlags_NoDocking)));
                        }
                        if (v.parameterFileOpen_) {
                                const auto *window = ImGui::FindWindowByName(("###ParameterFileEditor" + v.name_).c_str());
                                assert(window && !(window->Flags & (ImGuiWindowFlags_Modal | ImGuiWindowFlags_NoDocking)));
                        }
                        ImGui::Render();
                }
                ImGui::DestroyContext();
        }
        static void parameterPageActivation()
        {
                assert(!JLinkPort::instance().isConnected());
                Variable v("parameter-activation");
                v.parameterFile_.load(R"({"format":"foc-parameters","version":1,"target":"g_foc.cfg","parameters":[{"path":"mode","type":"u32","value":"0"}]})");
                v.openParameterEditor();
                ImGui::CreateContext();
                auto &io = ImGui::GetIO();
                io.IniFilename = nullptr;
                io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
                io.DisplaySize = ImVec2(1280, 960);
                io.DeltaTime = 1.0f / 60;
                unsigned char *pixels;
                int width, height;
                io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
                constexpr ImGuiID node = 0x50415241;
                const auto title = std::string(tr("Parameter Configuration###ParameterFileEditor", "参数配置###ParameterFileEditor")) + v.name_;
                const char *siblingTitle = "Parameter activation sibling";
                const auto frame = [&](bool build = false) {
                        ImGui::NewFrame();
                        if (build) {
                                ImGui::DockBuilderAddNode(node, ImGuiDockNodeFlags_DockSpace);
                                ImGui::DockBuilderSetNodeSize(node, io.DisplaySize);
                                ImGui::DockBuilderDockWindow(title.c_str(), node);
                                ImGui::DockBuilderDockWindow(siblingTitle, node);
                                ImGui::DockBuilderFinish(node);
                        }
                        ImGui::DockSpaceOverViewport(node, ImGui::GetMainViewport());
                        v.drawParameterFileEditor();
                        ImGui::Begin(siblingTitle); ImGui::TextUnformatted("Sibling"); ImGui::End();
                        ImGui::Render();
                };
                const auto select = [&](const char *name) {
                        auto *window = ImGui::FindWindowByName(name);
                        assert(window && window->DockNode && window->DockNode->TabBar);
                        window->DockNode->TabBar->NextSelectedTabId = window->TabId;
                        ImGui::FocusWindow(window);
                        frame(); frame();
                };
                frame(true); frame();
                select(title.c_str());
                assert(v.parameterFileVisible_ && v.parameterReadPending_);
                v.parameterReadPending_ = false; // Stand in for a completed device read
                frame(); frame();
                assert(!v.parameterReadPending_);
                select(siblingTitle);
                assert(!v.parameterFileVisible_);
                select(title.c_str());
                assert(v.parameterFileVisible_ && v.parameterReadPending_);
                v.parameterReadPending_ = false;
                frame(); frame();
                assert(!v.parameterReadPending_);
                v.openParameterEditor();
                assert(v.parameterReadPending_); // Explicit entry also refreshes an already open page
                v.parameterFileOpen_ = false;
                frame();
                assert(!v.parameterFileVisible_);
                v.openParameterEditor(); frame(); frame();
                assert(v.parameterFileVisible_ && v.parameterReadPending_);
                ImGui::DestroyContext();
                std::puts("PASS: parameter page refreshes once per tab entry or reopen, without requeuing on continuous frames");
        }
        static void offlineParameters()
        {
                Variable v("offline-parameters");
                const std::string input = R"({"format":"foc-parameters","version":1,"target":"g_foc.cfg","source":{"commit":"keep-me"},"parameters":[{"path":"mode","type":"uint32_t","value":"STOP","enum":{"STOP":0,"RUN":1},"unit":"custom","future_metadata":42},{"path":"limit","type":"uint64_t","value":"18446744073709551615"}]})";
                v.parameterFile_.load(input);
                assert(v.dwarfInfo_.variables.empty() && !v.rewrite_.targetLoaded && !v.rewrite_.pending);
                v.parameterFile_.document.fields[0].row.val = "1";
                const auto saved = v.parameterFile_.save();
                parameter_json::Json raw(cJSON_Parse(saved.c_str()), cJSON_Delete);
                assert(parameter_json::string(parameter_json::get(raw.get(), "source"), "commit") == "keep-me");
                const auto *fields = parameter_json::get(raw.get(), "parameters");
                assert(parameter_json::get(cJSON_GetArrayItem(fields,0),"future_metadata")->valueint == 42);
                parameter_json::FileEditor roundtrip;
                roundtrip.load(saved);
                assert(roundtrip.document.fields[0].row.val == "1");
                assert(roundtrip.document.fields[1].row.val == "18446744073709551615");
                for (const auto &bad : {"-1", "18446744073709551616", "1.5"}) {
                        roundtrip.document.fields[1].row.val = bad;
                        bool failed = false;
                        try { roundtrip.save(); } catch (const std::exception &) { failed = true; }
                        assert(failed);
                }
                roundtrip.load(R"({"format":"foc-parameters","version":1,"target":"g_foc.cfg","status":"needs_mapping","parameters":[],"notes":["keep"]})");
                const auto archive = roundtrip.save();
                bool rejected = false;
                try { parameter_json::parse(archive); } catch (const std::exception &) { rejected = true; }
                assert(rejected);
                roundtrip.load(R"({"format":"foc-parameters","version":1,"target":"g_foc.cfg","parameters":[{"path":"control.pid.reference_change_max","type":"float","role":"derived","expression":"reference_rate_max/fs","runtime_path":"g_foc.lo.pid.cfg.ref_change_max"},{"path":"control.array[10].gain","type":"float","value":"2"},{"path":"control.array[2].gain","type":"float","value":"3"}]})");
                const auto hierarchy = parameter_json::tree(roundtrip.document);
                assert(hierarchy.children.size()==1 && hierarchy.children[0].name=="control");
                const auto &array = hierarchy.children[0].children[1];
                assert(array.name=="array" && array.children[0].name=="[2]" && array.children[1].name=="[10]");
                const auto derivedSave = roundtrip.save();
                auto derivedDoc = parameter_json::parse(derivedSave);
                assert(derivedDoc.fields[0].role=="derived" && derivedDoc.fields[0].row.val.empty());
                assert(derivedDoc.fields[0].expression == "reference_rate_max/fs");
                assert(derivedDoc.fields[0].runtimePath == "g_foc.lo.pid.cfg.ref_change_max");
                v.parameterFile_.load(derivedSave);
                v.parameterTreeExpand_ = 1;
                v.parameterFileOpen_ = v.parameterFileRequestOpen_ = true;
                std::string parameterLog;
                draw(v, &parameterLog);
                assert(parameterLog.find("gain") != std::string::npos);
                assert(parameterLog.find("reference_change_max") == std::string::npos);
                assert(parameterLog.find("Show derived fields") == std::string::npos);
                assert(parameterLog.find("显示派生字段") == std::string::npos);
                std::strcpy(v.parameterFileSearch_, "reference_");
                draw(v, &parameterLog);
                assert(parameterLog.find("reference_change_max") == std::string::npos);
                assert(parameterLog.find("gain") == std::string::npos);
                v.parameterFileSearch_[0] = '\0';
                assert(v.parameterFile_.save() == derivedSave);
                assert(!v.rewrite_.targetLoaded && !v.rewrite_.pending);
                std::puts("PASS: legacy derived metadata roundtrips but remains hidden in the configuration page, including search");
                if (const char *directory = std::getenv("JSON_PROFILE_DIRECTORY")) {
                        for (const auto &entry : std::filesystem::directory_iterator(directory)) {
                                if (entry.path().extension() != ".json") continue;
                                std::ifstream stream(entry.path(), std::ios::binary);
                                const std::string text((std::istreambuf_iterator<char>(stream)), {});
                                parameter_json::Json rawDocument(cJSON_Parse(text.c_str()), cJSON_Delete);
                                if (parameter_json::string(rawDocument.get(), "format") != "foc-parameters") continue;
                                roundtrip.load(text);
                                roundtrip.load(roundtrip.save());
                        }
                }
                std::puts("PASS: offline JSON without AXF/device, headless UI, editing, uint64 limits, metadata preservation, all historical documents and device gating");
        }
        static void componentParameters()
        {
                const std::string base = R"({"format":"foc-parameters","version":1,"target":"g_foc.cfg","name":"NEO_45H","parameters":[{"path":"mech.ratio","type":"uint32_t","value":"1"},{"path":"hw.board","type":"uint32_t","value":"2"},{"path":"sw.gain","type":"uint32_t","value":"3"}]})";
                const std::string preset = R"({"format":"foc-parameters","version":1,"target":"g_foc.cfg","name":"NEO_65H","parameters":[{"path":"mech.ratio","type":"uint32_t","value":"101"},{"path":"hw.board","type":"uint32_t","value":"102"},{"path":"sw.gain","type":"uint32_t","value":"103"}]})";
                const auto path = std::filesystem::temp_directory_path() /
                        ("ava-component-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
                { std::ofstream output(path, std::ios::binary); output << preset; }
                Variable v("component-parameters");
                v.parameterFile_.load(base);
                v.parameterFilePath_ = "original-preset.json";
                v.parameterFile_.document.fields[2].row.val = "999";
                v.parameterFile_.dirty = true;
                v.openParameterEditor();
                assert(v.parameterReadPending_);
                v.parameterDevice_.variableName = "g_foc.cfg";
                v.parameterDeviceValues_["mech.ratio"] = "0";
                assert(v.selectParameterComponent("hw", path.string()));
                assert(!v.parameterReadPending_);
                assert(v.parameterDeviceValues_.at("mech.ratio") == "0");
                assert(v.parameterFile_.document.name == "NEO_45H/NEO_65H/NEO_45H*");
                assert(v.parameterFile_.document.fields[0].row.val == "1");
                assert(v.parameterFile_.document.fields[1].row.val == "102");
                assert(v.parameterFile_.document.fields[2].row.val == "999");
                assert(v.parameterFilePath_.empty() && v.parameterFileCombination_ && v.parameterFile_.dirty);
                const auto composed = v.parameterFile_.save();
                v.openParameterComposition();
                assert(v.parameterFile_.save() == composed);
                assert(v.parameterFileOpen_ && v.parameterFileRequestOpen_ && !v.rewrite_.pending);
                draw(v);
                parameter_json::FileEditor roundtrip;
                roundtrip.load(composed);
                assert(roundtrip.document.components.at("hw") == "NEO_65H");
                assert(roundtrip.document.name == "NEO_45H/NEO_65H/NEO_45H*");
                roundtrip.document.fields[2].row.val = "3";
                roundtrip.refreshNames();
                assert(roundtrip.document.name == "NEO_45H/NEO_65H/NEO_45H");
                { std::ofstream output(path, std::ios::binary); output << R"({"format":"foc-parameters","version":1,"target":"g_foc.cfg","name":"broken","parameters":[{"path":"hw.other","type":"uint32_t","value":"4"}]})"; }
                v.parameterFilePath_ = "saved-combination.json";
                assert(!v.selectParameterComponent("hw", path.string()));
                assert(v.parameterFile_.save() == composed && v.parameterFilePath_ == "saved-combination.json");
                // Selecting a complete preset replaces all component selections and pending edits
                { std::ofstream output(path, std::ios::binary); output << preset; }
                assert(v.selectParameterComponent("mech", path.string()));
                assert(v.parameterFile_.document.name == "NEO_65H/NEO_65H/NEO_45H*");
                { std::ofstream output(path, std::ios::binary); output << base; }
                v.parameterReadPending_ = true; // A delayed initial read must not overwrite a new preset
                assert(v.loadParameterFile(path.string()));
                assert(!v.parameterReadPending_);
                assert(v.parameterDeviceValues_.at("mech.ratio") == "0");
                assert(v.parameterFile_.document.name == "NEO_45H");
                assert(v.parameterFile_.document.fields[0].row.val == "1");
                assert(v.parameterFile_.document.fields[1].row.val == "2");
                assert(v.parameterFile_.document.fields[2].row.val == "3");
                assert(!v.parameterFile_.dirty && !v.parameterFileCombination_);
                assert(v.parameterFilePath_ == path.string());
                assert(!v.rewrite_.targetLoaded && !v.rewrite_.pending);
                auto unnamed = parameter_json::parse(base);
                unnamed.name.clear();
                unnamed.components.clear();
                { std::ofstream output(path, std::ios::binary); output << parameter_json::format(unnamed); }
                assert(v.loadParameterFile(path.string()));
                assert(v.parameterFile_.document.name == path.stem().string());
                v.parameterFile_.document.fields[0].row.val = "42";
                v.parameterFile_.reset();
                assert(v.parameterFile_.document.name == path.stem().string());
                std::filesystem::remove(path);
                std::puts("PASS: component UI preserves other edits, displays mixed names, detaches source path and rejects invalid selections atomically");
        }
        static void parameterBundle()
        {
                namespace fs = std::filesystem;
                const auto folder = fs::current_path() / ("parameter-bundle-test-" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
                assert(fs::create_directory(folder));
                const auto bundled = folder / "firmware/parameters";
                const auto stale = folder / "old-presets";
                fs::create_directories(bundled);
                fs::create_directory(stale);
                const std::string oldPath = "hw.cur.adc.uvw[0].adc";
                const std::string newPath = "hw.cur.adc.uvw.u.adc";
                auto document = [&](const std::string &name, const std::string &path, const std::string &val) {
                        return "{\"format\":\"foc-parameters\",\"version\":1,\"target\":\"g_foc.cfg\",\"name\":\"" + name +
                            "\",\"parameters\":[{\"path\":\"" + path + "\",\"type\":\"uint32_t\",\"value\":\"" + val +
                            "\",\"id\":1,\"revision\":1}]}";
                };
                auto write = [](const fs::path &path, const std::string &text) {
                        std::ofstream stream(path, std::ios::binary); stream << text; stream.close(); assert(stream);
                };
                const auto schema = bundled / "param_list.json";
                const auto manifest = bundled / "param_list_manifest.json";
                write(schema, document("CURRENT", newPath, "1"));
                write(manifest, "{\"format\":\"foc-parameter-registry\",\"version\":1,\"parameters\":[{\"id\":1,\"revision\":1,\"type\":\"uint32_t\",\"path\":\"" + newPath + "\"}]}");
                write(stale / "param_list.json", document("STALE", oldPath, "99"));
                write(stale / "MODEL.json", document("MODEL", oldPath, "7"));
                Variable offline("offline-preset");
                offline.parameterDirectory_ = stale;
                assert(offline.parameterPreset("MODEL")->fields[0].row.path == oldPath);
                Variable v("parameter-bundle");
                v.elfPath_ = (folder / "firmware/FocDriver.axf").string();
                v.parameterDirectory_ = stale;
                assert(v.parameterSchemaDirectory() == bundled);
                assert(v.parameterPreset("param_list")->name == "CURRENT");
                assert(v.parameterPreset("param_list")->fields[0].row.path == newPath);
                bool staleRejected = false;
                try { (void)v.parameterPreset("MODEL"); } catch (const std::exception &) { staleRejected = true; }
                assert(staleRejected); // Reassigned IDs cannot migrate values to unrelated fields
                write(stale / "MODEL.json", document("MODEL", newPath, "7"));
                assert(v.parameterPreset("MODEL")->fields[0].row.path == newPath);
                assert(v.parameterPreset("MODEL")->fields[0].row.val == "7");
                const auto oldTime = fs::last_write_time(schema);
                write(schema, document("UPDATED", newPath, "2"));
                fs::last_write_time(schema, oldTime + std::chrono::seconds(2));
                assert(v.parameterPreset("param_list")->name == "UPDATED");
                assert(v.parameterPreset("param_list")->fields[0].row.val == "2");
                // A new primary preset must supersede an already cached schema fallback
                write(bundled / "FALLBACK.json", document("FALLBACK", newPath, "11"));
                assert(v.parameterPreset("FALLBACK")->fields[0].row.val == "11");
                const auto directoryTime = fs::last_write_time(stale);
                write(stale / "FALLBACK.json", document("FALLBACK", newPath, "22"));
                fs::last_write_time(stale, directoryTime + std::chrono::seconds(2));
                assert(v.parameterPreset("FALLBACK")->fields[0].row.val == "22");
                const auto presetTime = fs::last_write_time(stale / "FALLBACK.json");
                write(stale / "FALLBACK.json", document("FALLBACK", newPath, "33"));
                fs::last_write_time(stale / "FALLBACK.json", presetTime + std::chrono::seconds(2));
                assert(v.parameterPreset("FALLBACK")->fields[0].row.val == "33");
                fs::remove(manifest);
                bool rejected = false;
                try { (void)v.parameterPreset("param_list"); } catch (const std::exception &) { rejected = true; }
                assert(rejected); // Never fall back to an unrelated application's schema
                assert(fs::weakly_canonical(folder).parent_path() == fs::weakly_canonical(fs::current_path()));
                fs::remove_all(folder);
                std::puts("PASS: AXF-adjacent schema, stale preset rejection, same-directory cache refresh, missing-manifest rejection");
        }
        /**
         * @brief Every in-program type token is lowercase. Tokens read from older
         *        sessions/presets can still be uppercase, so normalizeTypeName() is the
         *        gate that keeps them decodable; nothing downstream is case-tolerant
         */
        static void typeVocabulary()
        {
                const f32   one = 1.0f;
                const auto *raw = reinterpret_cast<const u8 *>(&one);
                assert(decodeAs(raw, normalizeTypeName("F32")) == 1.0f);
                // Pins the hazard: decodeAs is strict, so an unnormalized token
                // silently falls through to the 4-byte u32 reinterpretation
                assert(decodeAs(raw, "F32") == 1065353216.0f);
                assert(typeBytes(normalizeTypeName("I8")) == 1);
                assert(typeBytes(normalizeTypeName("F64")) == 8);
                assert(typeBytes(normalizeTypeName("U16")) == 2);
                MonitorChannel ch("type-vocabulary");
                ch.setType("I16");
                assert(ch.getType() == "i16" && ch.getNumBytes() == 2);
                ch.setType("U64");
                assert(ch.getType() == "u64" && ch.getNumBytes() == 8);
                assert(std::strcmp(Parser::dataTypeToStr(DataType::U8), "u8") == 0);
                assert(std::strcmp(Parser::dataTypeToStr(DataType::ARRAY), "array") == 0);
                // The reader side stays tolerant of both spellings
                assert(Parser::strToDataType("array") == DataType::ARRAY);
                assert(Parser::strToDataType("U32") == DataType::U32);
                assert(Parser::strToDataType("u32") == DataType::U32);
                std::puts("PASS: type tokens are lowercase end to end, legacy spellings normalize at the sampler funnel");
        }

        static void runAxf(const char *path, bool compatibilityOnly = false)
        {
                ElfParser parser;
                assert(parser.parse(path));
                Variable v("axf-regression");
                v.elfPath_ = path;
                assert(dwarf::parse(parser.getElfInfo(), v.dwarfInfo_));
                // Resolve the actual symbols used by BKP SRAM capture, including
                // renamed roots and array members; a layout fallback cannot hide a typo
                v.elfInfo_ = parser.getElfInfo();
                v.rebuildSearchPool();
                const auto &firmware = v.parameterFirmwareIdentity();
                auto readImage = [&](uint32_t address, uint32_t size, uint8_t *bytes) {
                        for (const auto &segment : firmware.segments) {
                                if (address >= segment.address && uint64_t(address-segment.address)+size <= segment.size) {
                                        std::copy_n(firmware.bytes.data()+segment.offset+address-segment.address,size,bytes);
                                        return true;
                                }
                        }
                        return false;
                };
                firmware.verify(readImage);
                bool rejectedIdentity = false;
                try {
                        firmware.verify([&](uint32_t address, uint32_t size, uint8_t *bytes) {
                                const bool ok=readImage(address,size,bytes);
                                if (ok) bytes[0]^=1;
                                return ok;
                        });
                } catch (const std::exception &) { rejectedIdentity=true; }
                assert(rejectedIdentity);
                firmware.verify([&](uint32_t address, uint32_t size, uint8_t *bytes) {
                        assert(address == firmware.descriptorAddress && size == firmware.descriptor.size());
                        const bool ok = readImage(address, size, bytes);
                        if (ok) {
                                bytes[24] ^= 1; // Default configuration digest
                                bytes[56] ^= 1; // Firmware version
                                bytes[68] ^= 1; // Build timestamp
                        }
                        return ok;
                });
                rejectedIdentity=false;
                try { firmware.validateManifest(1,firmware.field(12),firmware.field(16)^1,firmware.field(20)); }
                catch (const std::exception &) { rejectedIdentity=true; }
                assert(rejectedIdentity);
                std::puts("PASS: actual AXF parameter compatibility accepts rebuild metadata and rejects layout/manifest mismatch");
                if (compatibilityOnly)
                        return;
                for (const auto *symbolPath : {
                            "g_debug_trace_ctrl",
                            "g_debug_trace_data",
                            "g_debug_trace_ctrl.seq",
                            "g_debug_trace_ctrl.ack",
                            "g_debug_trace_ctrl.ch_en.value|g_debug_trace_ctrl.ch_enable.val",
                            "g_debug_trace_ctrl.sample_divider",
                            "g_debug_trace_ctrl.status",
                            "g_debug_trace_ctrl.err_ch",
                            "g_debug_trace_ctrl.data_size",
                            "g_debug_trace_ctrl.ch[0].address|g_debug_trace_ctrl.ch[0].addr",
                            "g_debug_trace_ctrl.ch[0].type",
                            "g_debug_trace_ctrl.ch[1].address|g_debug_trace_ctrl.ch[1].addr",
                            "g_debug_trace_ctrl.state[0].address|g_debug_trace_ctrl.state[0].addr",
                            "g_debug_trace_ctrl.state[1].address|g_debug_trace_ctrl.state[1].addr",
                            "g_debug_trace_ctrl.state[0].active",
                            "g_debug_trace_ctrl.state[0].type",
                            "g_debug_trace_ctrl.state[0].buf_address|g_debug_trace_ctrl.state[0].buf_addr",
                            "g_debug_trace_ctrl.state[0].capacity|g_debug_trace_ctrl.state[0].cap",
                            "g_debug_trace_ctrl.state[0].generation",
                            "g_debug_trace_ctrl.state[0].write_seq",
                            "g_debug_trace_ctrl.state[0].overwrites",
                     }) {
                        u32 address = 0;
                        bool found = false;
                        std::string names(symbolPath);
                        for (;;) {
                                const auto separator = names.find('|');
                                if (v.findElfSymbolAddress(names.substr(0, separator), address)) { found = true; break; }
                                if (separator == std::string::npos) break;
                                names.erase(0, separator + 1);
                        }
                        if (!found) {
                                std::fprintf(stderr, "Unresolved BKP trace symbol: %s\n", symbolPath);
                                assert(false);
                        }
                }
                std::puts("PASS: actual ELF resolves all BKP trace control/data symbols and members");
                auto resolve = [&](u64 off) -> const dwarf::Type * {
                        for (int i = 0; i < 32; ++i) {
                                auto it = v.dwarfInfo_.types.find(off);
                                if (it == v.dwarfInfo_.types.end())
                                        return nullptr;
                                if (it->second.kind != dwarf::TypeKind::TYPEDEF && it->second.kind != dwarf::TypeKind::MODIFIER)
                                        return &it->second;
                                off = it->second.inner;
                        }
                        return nullptr;
                };
                for (const auto *enumName : {"foc_encoder", "foc_encoder_theta", "foc_tor_sensor", "foc_predrv",
                                             "foc_dir", "foc_periph_handle_id", "foc_hw_shared_memory_protocol"}) {
                        const auto found = std::find_if(v.dwarfInfo_.types.begin(), v.dwarfInfo_.types.end(),
                                                       [&](const auto &entry) {
                                                               const auto *type = resolve(entry.first);
                                                               return type && type->kind == dwarf::TypeKind::ENUM &&
                                                                      type->name == enumName && !type->enums.empty();
                                                       });
                        if (found == v.dwarfInfo_.types.end() && std::strcmp(enumName, "foc_hw_shared_memory_protocol") == 0)
                                continue;
                        if (found == v.dwarfInfo_.types.end()) {
                                std::fprintf(stderr, "Unresolved hardware enum type: %s\n", enumName);
                                assert(false);
                        }
                        const auto *type = resolve(found->first);
                        std::vector<std::pair<const char *, int>> expected;
                        if (std::strcmp(enumName, "foc_predrv") == 0)
                                expected = {{"FOC_PREDRV_NONE", 0}, {"FOC_PREDRV_8353", 1}, {"FOC_PREDRV_INDEPENDENT", 2}};
                        else if (std::strcmp(enumName, "foc_encoder_theta") == 0)
                                expected = {{"FOC_ENCODER_ANGLE_NONE", 0}, {"FOC_ENCODER_ANGLE_ELEC", 1}, {"FOC_ENCODER_ANGLE_MECH", 2}};
                        else if (std::strcmp(enumName, "foc_encoder") == 0)
                                expected = {{"FOC_ENCODER_NONE", 0}, {"FOC_ENCODER_GW_INNER", 1}, {"FOC_ENCODER_GW_OUTER", 2}};
                        for (const auto &[name, val] : expected)
                                assert(std::any_of(type->enums.begin(), type->enums.end(), [&](const auto &option) {
                                        return option.name == name && option.val == val;
                                }));
                }
                std::puts("PASS: actual ELF resolves hardware enum types and distinguishes encoder models from theta categories");
                const auto root = std::find_if(v.dwarfInfo_.variables.begin(), v.dwarfInfo_.variables.end(), [](const auto &x) {
                        return x.name == "g_foc";
                });
                assert(root != v.dwarfInfo_.variables.end());
                const auto *type = resolve(root->type);
                assert(type);
                const auto cfg =
                    std::find_if(type->members.begin(), type->members.end(), [](const auto &m) { return m.name == "cfg"; });
                assert(cfg != type->members.end());
                auto resolvePath = [&](const std::string &path) -> const dwarf::Type * {
                        size_t end = path.find('.');
                        const auto symbol = std::find_if(v.dwarfInfo_.variables.begin(),v.dwarfInfo_.variables.end(),[&](const auto &item){return item.name==path.substr(0,end);});
                        if (symbol==v.dwarfInfo_.variables.end()) return nullptr;
                        const auto *current=resolve(symbol->type);
                        while (end!=std::string::npos && current) {
                                const size_t start=end+1;end=path.find('.',start);
                                const auto name=path.substr(start,end==std::string::npos?end:end-start);
                                const auto member=std::find_if(current->members.begin(),current->members.end(),[&](const auto &m){return m.name==name;});
                                if (member==current->members.end()) return nullptr;
                                current=resolve(member->type);
                        }
                        return current;
                };
                assert(!resolvePath("g_foc.cfg.sw.ctrl.vel_pi.ref_change_max"));
                assert(resolvePath("g_foc.lo.vel_pi.lo.ref_change_max") || resolvePath("g_foc.lo.ctrl.vel_pi.lo.ref_change_max"));
                assert(resolvePath("g_foc.cfg.sw.ctrl.tor_pi.ref_rate_max"));
                assert(resolvePath("g_foc.cfg.hw.predrv.predrv"));
                assert(resolvePath("g_foc.cfg.hw.predrv.predrv_en.pin") || resolvePath("g_foc.cfg.hw.predrv.predrv_enable.pin"));
                const auto *cfgType = resolve(cfg->type);
                assert(cfgType);
                assert(std::none_of(cfgType->members.begin(), cfgType->members.end(), [](const auto &m) { return m.name == "type_cfg"; }));
                const auto software = std::find_if(cfgType->members.begin(), cfgType->members.end(), [](const auto &m) { return m.name == "sw"; });
                assert(software != cfgType->members.end());
                const auto *softwareType = resolve(software->type); assert(softwareType);
                const auto check = std::find_if(softwareType->members.begin(), softwareType->members.end(), [](const auto &m) { return m.name == "check" || m.name == "safety"; });
                assert(check != softwareType->members.end());
                VarEntry entry{};
                entry.name     = "g_foc";
                entry.addr     = root->addr;
                entry.typeOff  = root->type;
                entry.type     = DataType::ARRAY;
                entry.port     = PortType::LOCAL;
                entry.isStruct = true;
                entry.writable = true;
                v.vars_.push_back(entry);
                v.localBufs_[entry.name].resize(type->size);
                const auto nameMember = std::find_if(cfgType->members.begin(), cfgType->members.end(), [](const auto &m) { return m.name == "name"; });
                assert(nameMember != cfgType->members.end());
                std::memcpy(v.localBufs_[entry.name].data() + cfg->offset + nameMember->offset, "NEO_75H", 8);
                ImGui::CreateContext();
                auto &io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = ImVec2(1280, 960); io.DeltaTime = 1.0f/60;
                unsigned char *pixels; int width, height; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
                for (int frame = 0; frame < 2; ++frame) {
                        ImGui::NewFrame(); ImGui::Begin("Name display regression");
                        if (ImGui::BeginTable("Variables", 6)) {
                                v.drawVarVarTreeRow("g_foc.cfg", root->addr + cfg->offset, cfg->type, 1, PortType::LOCAL, "", 0);
                                ImGui::EndTable();
                        }
                        ImGui::End(); ImGui::Render();
                }
                ImGui::DestroyContext();
                assert(v.memberValueCache_.at("g_foc.cfg.name") == "NEO_75H");
                std::puts("PASS: actual AXF char array displays NEO_75H in variable tree");

                {
                        std::lock_guard lk(v.mtxElf_);
                        v.openRewriteMember(0, root->addr + cfg->offset, cfg->type, "g_foc.cfg");
                }
                v.prepareRewrite();
                assert(v.rewrite_.targetLoaded && v.rewrite_.isFocCfg);
                if (const char *binary = std::getenv("PARAMETER_DEFAULT_BINARY")) {
                        std::ifstream input(binary, std::ios::binary);
                        input.read(reinterpret_cast<char *>(v.rewrite_.bytes.data()), v.rewrite_.bytes.size());
                        assert(input.gcount() == v.rewrite_.bytes.size());
                        cJSON *schema = cJSON_CreateObject();
                        std::function<void(u64, std::string, u32)> walk;
                        walk = [&](u64 off, std::string name, u32 bitSize) {
                                const auto *t = resolve(off); assert(t);
                                if (t->kind == dwarf::TypeKind::STRUCT || t->kind == dwarf::TypeKind::UNION) {
                                        const dwarf::Type::Member *selected = nullptr;
                                        if (t->kind == dwarf::TypeKind::UNION && !t->members.empty()) {
                                                selected = &t->members.front();
                                                for (const auto &m : t->members) if (m.name == "bit" || m.name == "page") selected = &m;
                                        }
                                        for (const auto &m : t->members) if (!selected || selected == &m)
                                                walk(m.type, name.empty() ? m.name : name + "." + m.name, m.bitSize);
                                } else if (t->kind == dwarf::TypeKind::ARRAY) {
                                        u64 count = 1;
                                        for (auto dim : t->dims) count *= dim;
                                        for (u64 i = 0; i < count; ++i) walk(t->inner, name + "[" + std::to_string(i) + "]", 0);
                                } else {
                                        auto *field = cJSON_CreateObject();
                                        cJSON_AddItemToObject(schema, name.c_str(), field);
                                        if (bitSize) cJSON_AddNumberToObject(field, "bit_size", bitSize);
                                        if (t->kind == dwarf::TypeKind::ENUM) {
                                                auto *enums = cJSON_CreateObject(); cJSON_AddItemToObject(field, "enums", enums);
                                                for (const auto &e : t->enums) cJSON_AddNumberToObject(enums, e.name.c_str(), static_cast<double>(e.val));
                                        }
                                }
                        };
                        walk(cfg->type, "", 0);
                        const auto csvRows = parameter_csv::parse(v.exportRewriteCsv());
                        for (const auto &r : csvRows) {
                                auto *field = cJSON_GetObjectItemCaseSensitive(schema, r.path.c_str()); assert(field);
                                cJSON_AddStringToObject(field, "type", r.type.c_str());
                        }
                        char *schemaText = cJSON_Print(schema);
                        std::ofstream schemaOut(std::string(binary) + ".schema.json"); schemaOut << schemaText; schemaOut.close();
                        cJSON_free(schemaText); cJSON_Delete(schema);
                        const auto original = v.rewrite_.bytes;
                        const auto csv = v.exportRewriteCsv();
                        assert(csv.find("hw.cur.adc.uvw[2].ch") != std::string::npos);
                        std::ofstream out(std::string(binary) + ".csv", std::ios::binary); out << csv; out.close();
                        std::fill(v.rewrite_.bytes.begin(), v.rewrite_.bytes.end(), 0);
                        assert(v.importRewriteCsv(csv));
                        assert(v.rewrite_.bytes == original);
                        if (const char *csvPath = std::getenv("PARAMETER_CSV")) {
                                std::ifstream in(csvPath, std::ios::binary);
                                std::string supplied((std::istreambuf_iterator<char>(in)), {});
                                assert(v.importRewriteCsv(supplied));
                                assert(v.rewrite_.bytes == original);
                        }
                        std::puts("PASS: complete firmware default CSV byte-for-byte roundtrip");
                }
                if (const char *folder = std::getenv("JSON_PROFILE_DIRECTORY")) {
                        v.parameterDirectory_ = folder;
                        size_t count=0;
                        for (const auto &file : std::filesystem::directory_iterator(folder)) {
                                if (file.path().extension() != ".json" || file.path().filename() == "param_list.json") continue;
                                std::ifstream input(file.path(),std::ios::binary);
                                std::string text((std::istreambuf_iterator<char>(input)),{});
                                parameter_json::Json doc(cJSON_Parse(text.c_str()),cJSON_Delete);
                                if (parameter_json::string(doc.get(),"status") != "ready") continue;
                                for (const auto &field : parameter_json::parse(text).fields) {
                                        if (field.role != "derived") continue;
                                        if (!resolvePath(field.runtimePath)) {
                                                std::fprintf(stderr,"Unresolved runtime path: %s\n",field.runtimePath.c_str());
                                                assert(false);
                                        }
                                }
                                if (count == 0) {
                                        auto staleProfile = parameter_json::parse(text);
                                        auto first = std::find_if(staleProfile.fields.begin(), staleProfile.fields.end(),
                                                                 [](const auto &field) { return field.id != 0; });
                                        assert(first != staleProfile.fields.end());
                                        auto other = std::find_if(std::next(first), staleProfile.fields.end(),
                                                                 [&](const auto &field) { return field.id != 0 && field.row.type == first->row.type; });
                                        assert(other != staleProfile.fields.end());
                                        std::swap(first->id, other->id);
                                        const auto before = v.rewrite_.bytes;
                                        assert(!v.importRewriteJson(parameter_json::format(staleProfile)));
                                        assert(v.rewrite_.bytes == before);
                                }
                                if (!v.importRewriteJson(text)) { std::fprintf(stderr,"%s: %s\n",file.path().string().c_str(),v.rewrite_.status.c_str()); assert(false); }
                                const auto bytes=v.rewrite_.bytes;
                                const auto exported=v.exportRewriteJson();
                                const auto exportedDocument = parameter_json::parse(exported);
                                const auto originalDocument = parameter_json::parse(text);
                                assert(exportedDocument.name == originalDocument.name);
                                assert(exportedDocument.fields.size() == originalDocument.fields.size());
                                const auto derivedCount = [](const auto &document) {
                                        return std::count_if(document.fields.begin(), document.fields.end(), [](const auto &field) { return field.role == "derived"; });
                                };
                                assert(derivedCount(exportedDocument) == derivedCount(originalDocument));
                                auto badName = parameter_json::parse(text);
                                badName.name = std::string(64, 'x');
                                bool nameRejected = false;
                                try { nameRejected = !v.importRewriteJson(parameter_json::format(badName)); }
                                catch (const std::exception &) { nameRejected = true; }
                                assert(nameRejected);
                                assert(v.rewrite_.bytes == bytes);
                                std::fill(v.rewrite_.bytes.begin(),v.rewrite_.bytes.end(),0);
                                assert(v.importRewriteJson(exported));
                                assert(v.rewrite_.bytes==bytes);
                                ++count;
                        }
                        assert(count>=18);
                        std::printf("PASS: %zu actuator JSON profiles against actual AXF, full byte roundtrip\n",count);

                        const auto source45 = std::filesystem::path(folder) / "NEO_45H.json";
                        auto source65 = std::filesystem::path(folder) / "NEO_65H.json";
                        if (!std::filesystem::exists(source65)) source65 = std::filesystem::path(folder) / "NEO_65H_SMH.json";
                        auto readProfile = [](const std::filesystem::path &profile) {
                                std::ifstream stream(profile, std::ios::binary);
                                return std::string((std::istreambuf_iterator<char>(stream)), {});
                        };
                        const auto model65 = parameter_json::parse(readProfile(source65)).name;
                        const auto mixedName = std::string("NEO_45H/") + model65 + "/NEO_45H";
                        const auto modifiedMixedName = std::string("NEO_45H*/") + model65 + "/NEO_45H";
                        v.parameterDirectory_ = folder;
                        assert(v.importRewriteJson(readProfile(source45)));
                        const auto bytes45 = v.rewrite_.bytes;
                        assert(v.importRewriteJson(readProfile(source65)));
                        const auto bytes65 = v.rewrite_.bytes;
                        assert(v.importRewriteJson(readProfile(source45)));
                        // Simulate the ordinary first device read: no earlier JSON
                        // import supplied metadata for editable/derived fields
                        v.rewrite_.jsonFields.clear();
                        v.rewrite_.csvRows.clear();
                        v.openParameterComposition();
                        if (v.parameterFile_.source.empty()) {
                                std::fprintf(stderr, "Fresh AXF metadata snapshot: %s\n", v.rewrite_.status.c_str());
                                assert(false);
                        }
                        assert(v.parameterFilePath_.empty() && v.parameterFileCombination_);
                        const auto sourceDocument = parameter_json::parse(readProfile(source45));
                        const auto expectedDerived = std::count_if(sourceDocument.fields.begin(), sourceDocument.fields.end(),
                                                                  [](const auto &field) { return field.role == "derived"; });
                        const auto pristineSnapshot = v.parameterFile_.save();
                        assert(std::count_if(v.parameterFile_.document.fields.begin(), v.parameterFile_.document.fields.end(),
                                             [](const auto &field) { return field.role == "derived"; }) == expectedDerived);
                        assert(v.parameterFile_.document.fields.size() == sourceDocument.fields.size());
                        for (const auto *section : {"mech", "hw", "sw"}) {
                                v.parameterFile_.load(pristineSnapshot);
                                if (!v.selectParameterComponent(section, source65.string())) {
                                        std::fprintf(stderr, "Fresh AXF snapshot %s: %s\n", section, v.parameterFileStatus_.c_str());
                                        assert(false);
                                }
                                v.rewrite_.bytes = bytes45;
                                assert(v.importRewriteJson(v.parameterFile_.save()));
                                for (const auto *part : {"mech", "hw", "sw"}) {
                                        const auto member = std::find_if(cfgType->members.begin(), cfgType->members.end(),
                                                                       [&](const auto &m) { return m.name == part; });
                                        assert(member != cfgType->members.end());
                                        const auto *memberType = resolve(member->type); assert(memberType);
                                        const auto &expected = std::strcmp(section, part) == 0 ? bytes65 : bytes45;
                                        assert(std::equal(v.rewrite_.bytes.begin() + member->offset,
                                                          v.rewrite_.bytes.begin() + member->offset + memberType->size,
                                                          expected.begin() + member->offset));
                                }
                        }
                        v.parameterFile_.load(pristineSnapshot);
                        v.rewrite_.bytes = bytes45;
                        if (!v.selectParameterComponent("hw", source65.string())) {
                                std::fprintf(stderr, "Composition from AXF snapshot: %s\n", v.parameterFileStatus_.c_str());
                                assert(false);
                        }
                        assert(v.parameterFile_.document.name == mixedName);
                        assert(v.importRewriteJson(v.parameterFile_.save()));
                        assert(std::strcmp(reinterpret_cast<const char *>(v.rewrite_.bytes.data() + nameMember->offset),
                                           mixedName.c_str()) == 0);
                        for (const auto *section : {"mech", "hw", "sw"}) {
                                const auto member = std::find_if(cfgType->members.begin(), cfgType->members.end(),
                                                               [&](const auto &m) { return m.name == section; });
                                assert(member != cfgType->members.end());
                                const auto *memberType = resolve(member->type); assert(memberType);
                                const auto &expected = std::strcmp(section, "hw") == 0 ? bytes65 : bytes45;
                                assert(std::equal(v.rewrite_.bytes.begin() + member->offset,
                                                  v.rewrite_.bytes.begin() + member->offset + memberType->size,
                                                  expected.begin() + member->offset));
                        }
                        const auto mixedExport = parameter_json::parse(v.exportRewriteJson());
                        assert(mixedExport.components.at("hw") == model65);
                        assert(mixedExport.name == mixedName);
                        assert(mixedExport.fields.size() == sourceDocument.fields.size());
                        assert(std::count_if(mixedExport.fields.begin(), mixedExport.fields.end(),
                                             [](const auto &field) { return field.role == "derived"; }) == expectedDerived);
                        if (const char *outputPath = std::getenv("PARAMETER_COMPOSITION_JSON")) {
                                std::ofstream output(outputPath, std::ios::binary);
                                output << v.parameterFile_.save();
                                assert(output.good());
                        }
                        std::puts("PASS: fresh/live AXF snapshots retain full derived metadata and independently compose all three groups without changing other bytes");

                        // A fresh read of a mixed model must compare each group
                        // against its own preset, not param_list's default model
                        v.rewrite_.jsonFields.clear();
                        v.rewrite_.nameComparedBytes.clear();
                        assert(v.syncRewriteName());
                        assert(v.rewrite_.profileName == mixedName);
                        const auto defaultPreview = v.rewrite_.bytes;
                        const auto originalSnapshot = v.rewrite_.original;
                        const auto rows = parameter_csv::parse(v.exportRewriteCsv());
                        const auto originalNpp = *std::find_if(rows.begin(), rows.end(), [](const auto &row) { return row.path == "mech.motor.npp"; });
                        auto modifiedNpp = originalNpp;
                        modifiedNpp.val = std::to_string(std::stoul(originalNpp.val) + 1);
                        assert(v.importRewriteCsv(parameter_csv::format({modifiedNpp})));
                        assert(v.syncRewriteName());
                        assert(v.rewrite_.profileName == modifiedMixedName);
                        assert(v.rewrite_.original == originalSnapshot);
                        const auto changedJson = v.exportRewriteJson();
                        parameter_json::FileEditor savedModified;
                        savedModified.load(changedJson);
                        savedModified.load(savedModified.save());
                        assert(savedModified.document.name == modifiedMixedName);
                        auto &savedNpp = *std::find_if(savedModified.document.fields.begin(), savedModified.document.fields.end(),
                                                     [](const auto &field) { return field.row.path == "mech.motor.npp"; });
                        assert(savedNpp.presetValue && *savedNpp.presetValue == originalNpp.val);
                        if (const char *outputPath = std::getenv("PARAMETER_MODIFIED_JSON")) {
                                std::ofstream output(outputPath, std::ios::binary);
                                output << savedModified.save();
                                assert(output.good());
                        }
                        savedNpp.row.val = *savedNpp.presetValue;
                        savedModified.refreshNames();
                        assert(savedModified.document.name == mixedName);

                        // Name bytes participate in the ordinary masked write
                        std::memcpy(v.localBufs_[entry.name].data() + cfg->offset, defaultPreview.data(), defaultPreview.size());
                        v.rewrite_.original = defaultPreview;
                        std::vector<u8> masks(defaultPreview.size(), 0);
                        for (size_t i = 0; i < masks.size(); ++i) if (v.rewrite_.bytes[i] != defaultPreview[i]) masks[i] = 0xff;
                        assert(v.writeRewrite(v.rewrite_.bytes, masks));
                        std::vector<u8> written(defaultPreview.size());
                        assert(v.readRewrite(written) && written == v.rewrite_.bytes);
                        assert(std::strcmp(reinterpret_cast<const char *>(written.data() + nameMember->offset),
                                           modifiedMixedName.c_str()) == 0);
                        v.rewrite_.original = written;
                        assert(v.importRewriteCsv(parameter_csv::format({originalNpp})));
                        assert(v.syncRewriteName());
                        assert(v.rewrite_.profileName == mixedName);

                        // Restore-to-default also removes the marker in export,
                        // and a saved edited profile restores its true baseline
                        assert(parameter_json::parse(v.exportRewriteJson()).name == mixedName);
                        assert(v.importRewriteJson(changedJson));
                        assert(v.rewrite_.profileName == modifiedMixedName);
                        assert(v.importRewriteCsv(parameter_csv::format({originalNpp})));
                        assert(v.syncRewriteName());
                        assert(v.rewrite_.profileName == mixedName);
                        std::puts("PASS: per-component default markers survive save/reload, update preview/name writes, and clear on exact restoration");

                        std::memset(v.rewrite_.bytes.data() + nameMember->offset, 0, 64);
                        std::memcpy(v.rewrite_.bytes.data() + nameMember->offset, "CUSTOM_LOCAL", 12);
                        v.rewrite_.jsonFields.clear();
                        v.rewrite_.nameComparedBytes.clear();
                        assert(v.syncRewriteName());
                        assert(v.rewrite_.profileName == "CUSTOM_LOCAL");
                        const auto customDefault = v.rewrite_.bytes;
                        std::memcpy(v.localBufs_[entry.name].data() + cfg->offset, customDefault.data(), customDefault.size());
                        v.rewrite_.original = customDefault;
                        assert(v.importRewriteCsv(parameter_csv::format({modifiedNpp})));
                        assert(v.syncRewriteName());
                        assert(v.rewrite_.profileName == "CUSTOM_LOCAL*/CUSTOM_LOCAL/CUSTOM_LOCAL");
                        std::fill(masks.begin(), masks.end(), 0);
                        for (size_t i = 0; i < masks.size(); ++i) if (v.rewrite_.bytes[i] != customDefault[i]) masks[i] = 0xff;
                        assert(v.writeRewrite(v.rewrite_.bytes, masks));
                        v.reloadRewrite();
                        assert(v.rewrite_.targetLoaded && v.rewrite_.profileName == "CUSTOM_LOCAL*/CUSTOM_LOCAL/CUSTOM_LOCAL");
                        assert(v.importRewriteCsv(parameter_csv::format({originalNpp})));
                        assert(v.syncRewriteName());
                        assert(v.rewrite_.profileName == "CUSTOM_LOCAL/CUSTOM_LOCAL/CUSTOM_LOCAL");
                        std::puts("PASS: custom preset references survive masked write and same-session target reload");
                }
                v.rewrite_.expandMode = 1;
                draw(v);
                v.rewrite_.requestOpen = true;
                const auto protectionPath = std::string("g_foc.cfg.sw.") + check->name;
                std::strcpy(v.rewrite_.search, check->name.c_str());
                draw(v);
                {
                        std::lock_guard lk(v.mtxElf_);
                        v.openRewriteMember(0, root->addr + cfg->offset + software->offset + check->offset, check->type, protectionPath);
                }
                v.prepareRewrite();
                assert(v.rewrite_.targetLoaded && v.rewrite_.guardedFoc);
                assert(v.rewrite_.isFocCfg && v.rewrite_.variableName == "g_foc.cfg");
                assert(std::string(v.rewrite_.search) == protectionPath);
                v.rewrite_.expandMode = 1;
                draw(v);
                std::puts("PASS: actual AXF configuration and protection layouts, expanded tree and nested search");
        }
        static void focRefresh()
        {
                namespace fs = std::filesystem;
                const auto folder = fs::current_path() / ("refresh-foc-" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
                fs::create_directory(folder);
                auto writePreset = [&](const char *file, const char *name, int scale) {
                        parameter_json::Document doc;
                        doc.target = "g_foc.cfg"; doc.name = name;
                        for (const auto &[path, val] : std::vector<std::pair<std::string, int>>{
                                 {"backup", 0}, {"mech.ratio", scale}, {"hw.board", 2 * scale}, {"sw.gain", 3 * scale}}) {
                                parameter_json::Field field;
                                field.row.path = path; field.row.type = "u32"; field.row.val = std::to_string(val);
                                doc.fields.push_back(std::move(field));
                        }
                        std::ofstream output(folder / file, std::ios::binary);
                        output << parameter_json::format(doc);
                        assert(output);
                };
                writePreset("param_list.json", "A", 1); writePreset("A.json", "A", 1); writePreset("B.json", "B", 10);
                Variable v("foc-refresh");
                v.parameterDirectory_ = folder;
                dwarf::Type byte; byte.kind = dwarf::TypeKind::BASE; byte.size = 1; byte.encoding = 7;
                dwarf::Type scalar = byte; scalar.size = 4;
                dwarf::Type name; name.kind = dwarf::TypeKind::ARRAY; name.inner = 1; name.size = 64;
                v.dwarfInfo_.types[1] = byte; v.dwarfInfo_.types[2] = scalar; v.dwarfInfo_.types[3] = name;
                for (const auto &[id, member] : std::vector<std::pair<u64, std::string>>{{4, "ratio"}, {5, "board"}, {6, "gain"}}) {
                        dwarf::Type group; group.kind = dwarf::TypeKind::STRUCT; group.size = 4;
                        group.members.push_back({member, 0, 2, 0, 0}); v.dwarfInfo_.types[id] = group;
                }
                dwarf::Type root; root.kind = dwarf::TypeKind::STRUCT; root.name = "foc_cfg"; root.size = 84;
                root.members = {{"name", 0, 3, 0, 0}, {"backup", 64, 2, 0, 0}, {"mech", 68, 4, 0, 0},
                                {"hw", 72, 5, 0, 0}, {"sw", 76, 6, 0, 0}};
                v.dwarfInfo_.types[7] = root;
                VarEntry entry{}; entry.name = "g_foc.cfg"; entry.port = PortType::LOCAL;
                entry.type = DataType::ARRAY; entry.typeOff = 7; entry.isStruct = entry.writable = true;
                v.vars_.push_back(entry);
                auto &target = v.localBufs_[entry.name]; target.resize(84);
                std::memcpy(target.data(), "A/A/A", 6); target[68] = 1; target[72] = 2; target[76] = 3;
                v.openRewrite(entry); v.prepareRewrite();
                auto &d = v.rewrite_;
                assert(d.targetLoaded && d.statusOk && d.profileName == "A/A/A");
                d.bytes[64] = 1;
                std::memcpy(target.data(), "B/B/B", 6); target[68] = 10; target[72] = 20; target[76] = 30;
                v.reloadRewrite(true);
                assert(d.statusOk && d.profileName == "B/B/B" && d.bytes[64] == 1 && target[64] == 0);
                assert(d.jsonFields.at("mech.ratio").presetValue == std::optional<std::string>{"10"});
                // Reading a different device model switches the component selectors
                // and their defaults, rather than marking the opened JSON's model
                assert(v.loadParameterFile((folder / "A.json").string()));
                assert(v.updateParameterDevice(false));
                assert(v.parameterFile_.document.name == "B/B/B");
                assert(v.parameterFile_.document.components.at("mech") == "B");
                assert(v.parameterFilePath_.empty() && v.parameterFileCombination_);
                target[68] = 11;
                assert(v.updateParameterDevice(false));
                assert(v.parameterFile_.document.name == "B*/B/B");
                v.parameterFile_.restoreDefaults();
                assert(v.parameterFile_.document.name == "B/B/B");
                assert(v.parameterFile_.document.fields[1].row.val == "10");
                std::memset(target.data(), 0, 64);
                std::memcpy(target.data(), "A/B/A", 5); target[68] = 1; target[72] = 20; target[76] = 3;
                assert(v.updateParameterDevice(false));
                assert(v.parameterFile_.document.name == "A/B/A");
                assert(v.parameterFile_.document.components.at("hw") == "B");
                assert(v.parameterFile_.document.fields[1].presetValue == std::optional<std::string>{"1"});
                std::memset(target.data(), 0, 64);
                std::memcpy(target.data(), "B", 1); target[68] = 10; target[72] = 20; target[76] = 30;
                assert(v.updateParameterDevice(false));
                assert(v.parameterFile_.document.name == "B");
                assert(parameter_json::componentNames(v.parameterFile_.document).at("sw") == "B");
                fs::remove(folder / "param_list.json");
                v.reloadRewrite(true);
                assert(!d.statusOk && d.status.find("matching parameter preset directory") != std::string::npos);
                d.bytes[80] = 7;        // unsupported padding must never silently disappear
                const auto preview = d.bytes;
                v.reloadRewrite(true);
                assert(!d.statusOk && d.bytes == preview);
                assert(!v.exportRewriteCsv().empty()); // recovery backup uses captured layout
                assert(fs::weakly_canonical(folder).parent_path() == fs::weakly_canonical(fs::current_path()));
                fs::remove_all(folder);
                std::puts("PASS: FOC refresh adopts new component defaults, preserves root edits and validation errors, rejects uncovered edits");
        }
        static void jsonImportPerformance()
        {
                Variable v("json-switch-perf");
                constexpr size_t fields = 512;
                dwarf::Type scalar;
                scalar.kind = dwarf::TypeKind::BASE;
                scalar.size = 4;
                scalar.encoding = 7;
                scalar.name = "uint32_t";
                v.dwarfInfo_.types[1] = scalar;
                dwarf::Type root;
                root.kind = dwarf::TypeKind::STRUCT;
                root.size = fields * 4;
                root.name = "bench_cfg";
                parameter_json::Document document;
                document.target = "bench";
                for (size_t i = 0; i < fields; ++i) {
                        const auto path = "field" + std::to_string(i);
                        root.members.push_back({path, i * 4, 1, 0, 0});
                        parameter_json::Field field;
                        field.row.path = path;
                        field.row.type = "u32";
                        field.row.val = "2";
                        field.enums = {{"zero", "0"}, {"one", "1"}, {"two", "2"}, {"three", "3"}};
                        document.fields.push_back(std::move(field));
                }
                v.dwarfInfo_.types[2] = root;
                VarEntry entry{};
                entry.name = "bench";
                entry.port = PortType::LOCAL;
                entry.type = DataType::ARRAY;
                entry.typeOff = 2;
                entry.isStruct = entry.writable = true;
                v.vars_.push_back(entry);
                v.localBufs_[entry.name].resize(root.size);
                v.openRewrite(entry);
                v.prepareRewrite();
                const auto text = parameter_json::format(document);
                const auto start = std::chrono::steady_clock::now();
                for (int pass = 0; pass < 10; ++pass)
                        assert(v.importRewriteJson(text));
                const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 10;
                for (size_t i = 0; i < fields; ++i)
                        assert(v.rewrite_.bytes[i * 4] == 2 && v.localBufs_[entry.name][i * 4] == 0);
                const auto preview = v.rewrite_.bytes;
                document.fields.back().enums["overflow"] = "4294967296";
                assert(!v.importRewriteJson(parameter_json::format(document)) && v.rewrite_.bytes == preview);
                std::printf("PASS: JSON switching 512 fields / 2048 enum choices: %.3f ms/import; atomic late-option failure; no device writes\n", elapsed);
        }
        static void parameterComponentTabs()
        {
                Variable v("parameter-tabs");
                v.parameterFile_.load(R"({"format":"foc-parameters","version":1,"target":"test","parameters":[{"path":"bkp","type":"u32","value":"0"},{"path":"mech.mechanical_flag","type":"bool","value":"1"},{"path":"hw.hardware_flag","type":"u32","display":"checkbox","value":"0"},{"path":"sw.software_gain","type":"u32","value":"3"}]})");
                v.openParameterEditor();
                v.parameterDevice_.targetLoaded = true;
                v.parameterDevice_.variableName = "test";
                v.parameterDevice_.source.port = PortType::LOCAL;
                v.parameterDevice_.layoutGeneration = v.elfLayoutGeneration_;
                v.parameterDeviceValues_ = {{"bkp", "0"}, {"mech.mechanical_flag", "1"},
                                             {"hw.hardware_flag", "0"}, {"sw.software_gain", "3"}};
                ImGui::CreateContext();
                auto &io = ImGui::GetIO();
                io.IniFilename = nullptr;
                io.DisplaySize = ImVec2(1280, 960);
                io.DeltaTime = 1.0f / 60;
                unsigned char *pixels;
                int width, height;
                io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
                const auto logPath = std::filesystem::current_path() / "parameter-tabs-ui.log";
                const auto frame = [&](bool capture = false) {
                        ImGui::NewFrame();
                        if (capture) {
                                std::ofstream(logPath, std::ios::trunc).close();
                                ImGui::LogToFile(0, logPath.string().c_str());
                        }
                        v.drawParameterFileEditor();
                        ImGui::LogFinish();
                        ImGui::Render();
                        std::ifstream log(logPath);
                        return std::string(std::istreambuf_iterator<char>(log), {});
                };
                frame(); frame();
                const auto title = std::string(tr("Parameter Configuration###ParameterFileEditor", "参数配置###ParameterFileEditor")) + v.name_;
                auto *window = ImGui::FindWindowByName(title.c_str());
                assert(window);
                auto *tabs = ImGui::GetCurrentContext()->TabBars.GetByKey(window->GetID("##parameter_components"));
                assert(tabs && tabs->Tabs.Size == 3);
                const char *fields[] = {"mechanical_flag", "hardware_flag", "software_gain"};
                for (int i = 0; i < 3; ++i) {
                        tabs->NextSelectedTabId = tabs->Tabs[i].ID;
                        frame(); frame();
                        const auto log = frame(true);
                        assert(v.parameterComponentTab_ == i);
                        assert(log.find("bkp") != std::string::npos);
                        for (int j = 0; j < 3; ++j)
                                assert((log.find(fields[j]) != std::string::npos) == (i == j));
                        if (i < 2) {
                                const std::string marker = i == 0 ? "[x]" : "[ ]";
                                const auto first = log.find(marker, log.find(fields[i]));
                                assert(first != std::string::npos);
                                assert(log.find(marker, first + marker.size()) != std::string::npos);
                        }
                }
                assert(v.parameterFile_.document.fields[1].row.val == "1");
                assert(v.parameterFile_.document.fields[2].row.val == "0");
                ImGui::DestroyContext();
                std::filesystem::remove(logPath);
                std::puts("PASS: three component tabs isolate fields, retain shared fields and show boolean readback checkboxes");
        }
        static void directParameterApply()
        {
                Variable v("direct-parameter-apply");
                VarEntry entry{};
                entry.name = "test_value";
                entry.port = PortType::LOCAL;
                entry.type = DataType::U32;
                entry.writable = true;
                v.vars_.push_back(entry);
                v.localBufs_[entry.name] = {42, 0, 0, 0};
                v.openRewrite(entry);
                v.prepareRewrite();
                v.rewrite_.bytes[0] = 99;
                v.parameterFile_.load(R"({"format":"foc-parameters","version":1,"name":"preset","target":"test_value","parameters":[{"path":"","type":"u32","value":"80","min":"0","max":"100"}]})");
                assert(v.updateParameterDevice(false));
                assert(v.parameterDeviceValues_.at("") == "42");
                assert(v.parameterFile_.document.fields[0].row.val == "42");
                assert(v.localBufs_[entry.name][0] == 42 && v.rewrite_.bytes[0] == 99);
                // Readback fills the write control; writing it unchanged is a no-op
                assert(v.updateParameterDevice(true));
                assert(v.localBufs_[entry.name][0] == 42);
                v.parameterFile_.document.fields[0].row.val = "80";
                assert(v.updateParameterDevice(true));
                assert(v.localBufs_[entry.name][0] == 80);
                assert(v.parameterDeviceValues_.at("") == "80");
                assert(v.parameterDevice_.bytes == v.parameterDevice_.original);
                assert(v.rewrite_.bytes[0] == 99 && v.rewrite_.original[0] == 42);
                v.parameterFile_.document.fields[0].row.val = "101";
                assert(!v.updateParameterDevice(true));
                assert(v.localBufs_[entry.name][0] == 80 && v.rewrite_.bytes[0] == 99);
                // Reading device values does not depend on valid in-progress JSON edits
                assert(v.updateParameterDevice(false));
                assert(v.parameterDeviceValues_.at("") == "80");
                assert(v.parameterFile_.document.fields[0].row.val == "80");
                v.rewrite_.bytes[0] = 81;
                assert(!v.commitRewrite()); // device changed from the captured value 42 to 80
                assert(v.localBufs_[entry.name][0] == 80);
                std::puts("PASS: direct parameter apply/readback, actual values, draft isolation, invalid edit rejection and write conflicts");
        }
        static void run()
        {
                Variable    v("rewrite-regression");
                dwarf::Type scalar;
                scalar.kind           = dwarf::TypeKind::BASE;
                scalar.size           = 4;
                scalar.encoding       = 7;
                scalar.name           = "uint32_t";
                v.dwarfInfo_.types[1] = scalar;
                dwarf::Type root;
                root.kind = dwarf::TypeKind::STRUCT;
                root.size = 8;
                root.name = "test_cfg";
                root.members.push_back({"flag", 0, 1, 3, 1});
                root.members.push_back({"value", 4, 1, 0, 0});
                v.dwarfInfo_.types[2] = root;
                VarEntry entry{};
                entry.name     = "cfg";
                entry.port     = PortType::LOCAL;
                entry.type     = DataType::ARRAY;
                entry.typeOff  = 2;
                entry.isStruct = true;
                entry.writable = true;
                v.vars_.push_back(entry);
                v.localBufs_["cfg"] = {0x12, 0, 0, 0, 42, 0, 0, 0};
                // This is the actual former crash path: request an editor under the ELF mutex
                {
                        std::lock_guard lk(v.mtxElf_);
                        v.openRewriteMember(0, 0, 1, "cfg.flag", DataType::UNKNOWN, 3, 1);
                        assert(v.rewrite_.pending);
                }
                v.prepareRewrite();
                assert(v.rewrite_.targetLoaded && v.rewrite_.source.bitSize == 1);
                auto &d = v.rewrite_;
                assert(variable_rewrite::storeBits(d.bytes.data(), 4, 3, 1, 1));
                assert(v.writeRewrite(d.bytes, {8, 0, 0, 0}));
                assert(v.localBufs_["cfg"][0] == 0x1a && v.localBufs_["cfg"][4] == 42);
                draw(v);
                // Snapshot survives type map replacement while the editor is still open
                v.dwarfInfo_.types.clear();
                assert(d.info.types.at(1).size == 4);
                d.requestOpen = true;
                draw(v);
                // A manual member queues safely while the local buffer mutex is held too
                {
                        std::lock_guard lk(v.mtxLocal_);
                        v.openRewriteMember(0, 4, 0, "cfg.value", DataType::U32);
                }
                v.prepareRewrite();
                assert(d.targetLoaded && d.bytes[0] == 42);
                d.bytes[0] = 77;
                assert(v.writeRewrite(d.bytes, {255, 255, 255, 255}));
                assert(v.localBufs_["cfg"][0] == 0x1a && v.localBufs_["cfg"][4] == 77);
                draw(v);
                // Search visits nested leaves even when their parent has not been expanded
                v.dwarfInfo_.types[1] = scalar;
                v.dwarfInfo_.types[2] = root;
                v.openRewrite(v.vars_[0]);
                v.prepareRewrite();
                std::strcpy(d.search, "value");
                d.changedOnly = true;
                d.bytes[4]    = 78;
                draw(v);
                // Refresh rebases only edited fields and never writes the target
                v.localBufs_["cfg"][0] |= 1; // unrelated bit changes while editing value
                v.reloadRewrite(true);
                assert(d.statusOk && d.targetLoaded && d.bytes[4] == 78 && d.original[4] == 77);
                assert(d.bytes[0] == v.localBufs_["cfg"][0] && v.localBufs_["cfg"][4] == 77);
                const auto keptPreview = d.bytes, keptOriginal = d.original;
                v.localBufs_["cfg"][4] = 79; // same field changed concurrently
                v.reloadRewrite(true);
                assert(!d.statusOk && d.bytes == keptPreview && d.original == keptOriginal);
                v.localBufs_["cfg"][4] = 77;
                v.reloadRewrite();
                assert(d.statusOk && d.bytes == d.original && d.bytes[4] == 77);
                d.bytes[4] = 78;
                std::puts("PASS: refresh preserves edits/unrelated bits, rejects conflicting target changes, and explicitly discards edits");
                const std::string header = "path,type,default,unit,min,max,description\n";
                assert(v.importRewriteCsv(header + "value,u32,80,A,0,100,电流\nflag,u32,1,,,,\n"));
                assert(d.bytes[4] == 80 && (d.bytes[0] & 8));
                assert(v.localBufs_["cfg"][4] == 77); // import is preview only
                const auto beforeCsv = d.bytes;
                assert(!v.importRewriteCsv(header + "value,u32,81,,,,\nmissing,u32,1,,,,\n"));
                assert(d.bytes == beforeCsv);
                assert(!v.importRewriteCsv(header + "value,f32,1,,,,\n"));
                assert(!v.importRewriteCsv(header + "value,u32,101,,0,100,\n"));
                assert(!v.importRewriteCsv(header + "value,u32,1,,,,\nvalue,u32,2,,,,\n"));
                assert(!v.importRewriteCsv(header + "flag,u32,2,,,,\n"));
                const auto exported = v.exportRewriteCsv();
                assert(v.importRewriteCsv(exported));
                d.bytes[4] = 101;
                assert(!v.validateRewriteCsv());
                d.bytes = beforeCsv;
                assert(v.validateRewriteCsv());
                draw(v);
                const auto quoted = parameter_csv::format({{"value", "uint32_t", "80", "A", "0", "100", "comma, quote\" newline\n中文"}});
                assert(parameter_csv::parse(quoted)[0].description == "comma, quote\" newline\n中文");
                std::puts("PASS: CSV atomic import, preview, types, limits, duplicate/unknown paths, bitfields, UTF-8 and quoting");
                parameter_json::Document jsonDoc;
                jsonDoc.name="test"; jsonDoc.target="cfg";
                jsonDoc.fields={{{"value","u32","FAST","A","0","100","custom enum"},{{"STOP","0"},{"FAST","80"}}}};
                assert(v.importRewriteJson(parameter_json::format(jsonDoc)));
                assert(d.bytes[4]==80 && d.jsonFields.at("value").enums.size()==2);
                draw(v);
                const auto jsonBefore=d.bytes;
                const auto profileBefore=d.profileName;
                const auto enumCountBefore=d.jsonFields.at("value").enums.size();
                jsonDoc.fields[0].enums["INVALID"]="4294967296";
                assert(!v.importRewriteJson(parameter_json::format(jsonDoc)));
                assert(d.bytes==jsonBefore && d.profileName==profileBefore &&
                       d.jsonFields.at("value").enums.size()==enumCountBefore);
                assert(v.importRewriteJson(v.exportRewriteJson()));
                assert(!v.importRewriteJson("{\"format\":\"foc-parameters\",\"format\":\"foc-parameters\",\"version\":1}"));
                const auto preserved = v.rewrite_.bytes;
                parameter_json::Document derived;
                derived.target = v.rewrite_.variableName;
                parameter_json::Field computed;
                computed.row.path = "a";
                computed.row.type = "uint32_t";
                computed.role = "derived";
                derived.fields.push_back(computed);
                assert(v.importRewriteJson(parameter_json::format(derived)));
                assert(v.rewrite_.bytes == preserved);
                std::puts("PASS: derived-only import leaves target bytes untouched; tree groups arrays and runtime metadata roundtrips");
                std::puts("PASS: JSON explicit integer enums, option overflow, atomic rollback, duplicate keys and roundtrip");
                // Array element offsets and full-array rendering
                dwarf::Type array;
                array.kind            = dwarf::TypeKind::ARRAY;
                array.inner           = 1;
                array.size            = 8;
                v.dwarfInfo_.types[3] = array;
                v.vars_[0].typeOff    = 3;
                v.vars_[0].isArray    = true;
                {
                        std::lock_guard lk(v.mtxElf_);
                        v.openRewriteMember(0, 4, 1, "cfg[1]");
                }
                v.prepareRewrite();
                assert(d.targetLoaded && d.bytes[0] == 77);
                draw(v);
                v.openRewrite(v.vars_[0]);
                v.prepareRewrite();
                draw(v);
                // Scalar with no ELF, manually configured enum, F16, and invalid/unsupported sources
                for (auto dt : {DataType::F16, DataType::F32, DataType::F64, DataType::U8, DataType::I64}) {
                        v.vars_[0].typeOff = 0;
                        v.vars_[0].type    = dt;
                        v.vars_[0].isArray = false;
                        v.openRewrite(v.vars_[0]);
                        v.prepareRewrite();
                        assert(d.targetLoaded);
                        draw(v);
                }
                v.vars_[0].type        = DataType::I8;
                v.vars_[0].enumDefs    = {{"negative", -1}, {"zero", 0}};
                v.localBufs_["cfg"][0] = 255;
                v.openRewrite(v.vars_[0]);
                v.prepareRewrite();
                draw(v);
                for (auto port : {PortType::UDP, PortType::AUDIO, PortType::SHM}) {
                        v.vars_[0].port = port;
                        v.openRewrite(v.vars_[0]);
                        v.prepareRewrite();
                        assert(!d.targetLoaded && !d.status.empty());
                        draw(v);
                }
                v.vars_[0].port = PortType::JLINK;
                v.vars_[0].addr = UINT32_MAX;
                v.vars_[0].type = DataType::U32;
                v.openRewrite(v.vars_[0]);
                v.prepareRewrite();
                assert(!d.targetLoaded && d.bytes.empty());
                draw(v);
                // Offline imported array edits update the local snapshot only, including big endian data
                v.vars_[0].port = PortType::LOCAL;
                v.vars_[0].addr = 0x1000;
                v.vars_[0].enumDefs.clear();
                v.vars_[0].flashArray = flasharray::decode({0, 1, 0, 2}, 0x1000, flasharray::Type::U16, true, 1, "array");
                v.openRewriteMember(0, 0x1002, 0, "cfg[1]");
                v.prepareRewrite();
                assert(d.targetLoaded && d.bytes[0] == 2);
                d.bytes[0] = 3;
                assert(v.writeRewrite(d.bytes, {255, 255}));
                assert((v.vars_[0].flashArray->bytes == std::vector<u8>{0, 1, 0, 3}));
                draw(v);
                v.openRewrite(v.vars_[0]);
                v.prepareRewrite();
                assert(d.targetLoaded);
                draw(v);
        }
};
int
main(int argc, char **argv)
{
        _set_error_mode(_OUT_TO_STDERR);
        setvbuf(stdout, nullptr, _IONBF, 0);
        g_log_ops.cfg.level = g_log_perf.cfg.level = static_cast<log_level>(99);
        VariableRewriteRegression::diagnosticValues();
        if (argc > 1 && std::strcmp(argv[1], "--diagnostics") == 0) return 0;
        if (argc > 3 && std::strcmp(argv[1], "--diagnostic-profile") == 0) {
                VariableRewriteRegression::diagnosticProfile(argv[2],argv[3]);return 0;
        }
        if (argc > 3 && std::strcmp(argv[1], "--manual-target") == 0) {
                VariableRewriteRegression::jsonUnionPaths();
                VariableRewriteRegression::manualTarget(argv[2], argv[3]);
                return 0;
        }
        if (argc > 2 && std::strcmp(argv[1], "--elf-reload") == 0) {
                VariableRewriteRegression::elfReload(argv[2]);
                return 0;
        }
        if (argc > 2 && std::strcmp(argv[1], "--firmware-compatibility") == 0) {
                VariableRewriteRegression::parameterCompatibility();
                VariableRewriteRegression::runAxf(argv[2], true);
                return 0;
        }
        if (argc > 1 && std::strcmp(argv[1], "--session-load") == 0) {
                VariableRewriteRegression::sessionLoad();
                if (argc > 2)
                        VariableRewriteRegression::sessionFile(argv[2], argc > 3 ? argv[3] : nullptr);
                return 0;
        }
        VariableRewriteRegression::sessionLoad();
        using namespace variable_rewrite;
        assert(range(4, 4, 8) && !range(SIZE_MAX, 1, 8) && !range(7, SIZE_MAX, 8));
        assert(mask(8, 0, 64) == UINT64_MAX && mask(8, 64, 1) == 0 && mask(1, 7, 2) == 0);
        uint64_t bits = 0xa5;
        assert(storeBits(reinterpret_cast<uint8_t *>(&bits), 8, 8, 3, 6) && bits == 0x6a5);
        assert(!storeBits(reinterpret_cast<uint8_t *>(&bits), 8, 8, 3, 8));
        assert(storeSignedBits(reinterpret_cast<uint8_t *>(&bits), 8, 8, 3, -4) && bits == 0x4a5);
        assert(!storeSignedBits(reinterpret_cast<uint8_t *>(&bits), 8, 8, 3, 4));
        assert(!storeSignedBits(reinterpret_cast<uint8_t *>(&bits), 8, 8, 0, -1));
        std::vector<uint8_t> result;
        assert(merge({0, 1}, {8, 1}, {16, 2}, {8, 0}, result) && result == std::vector<uint8_t>({24, 2}));
        assert(!merge({0}, {3}, {2}, {255}, result));
        assert(merge({0}, {3}, {3}, {255}, result));
        uint64_t val = 0;
        assert(integer<uint64_t>("18446744073709551615", reinterpret_cast<uint8_t *>(&val)) && val == UINT64_MAX);
        assert(!integer<uint8_t>("256", reinterpret_cast<uint8_t *>(&val)));
        assert(!integer<uint32_t>("-1", reinterpret_cast<uint8_t *>(&val)));
        assert(!integer<int32_t>("12oops", reinterpret_cast<uint8_t *>(&val)));
        assert(integer<int64_t>("-9223372036854775808", reinterpret_cast<uint8_t *>(&val)) && val == (uint64_t{1} << 63));
        assert(integer<uint32_t>(" 0xff ", reinterpret_cast<uint8_t *>(&val)));
        {
                auto doc = parameter_json::parse(R"({"format":"foc-parameters","version":1,"target":"g_foc.cfg","parameters":[{"path":"flag","type":"uint32_t","value":"1","display":"checkbox"},{"path":"mode","type":"uint32_t","value":"RUN","enum":{"STOP":0,"RUN":2}}]})");
                auto copied = parameter_json::parse(parameter_json::format(doc));
                assert(copied.fields[0].display == "checkbox" && copied.fields[0].row.val == "1");
                assert(copied.fields[1].enums.size() == 2 && copied.fields[1].row.val == "2");
                doc.fields[0].row.val = "2";
                bool rejected = false;
                try { parameter_json::parse(parameter_json::format(doc)); } catch (...) { rejected = true; }
                assert(rejected);
        }
        VariableRewriteRegression::parameterBundle();
        VariableRewriteRegression::parameterCompatibility();
        VariableRewriteRegression::parameterErrorLanguage();
        VariableRewriteRegression::typeVocabulary();
        VariableRewriteRegression::perVariableRadix();
        try { VariableRewriteRegression::parameterPageActivation(); }
        catch (const std::exception &e) { std::fprintf(stderr, "Activation fixture: %s\n", e.what()); return 1; }
        VariableRewriteRegression::offlineParameters();
        VariableRewriteRegression::componentParameters();
        VariableRewriteRegression::directParameterApply();
        VariableRewriteRegression::parameterComponentTabs();
        VariableRewriteRegression::run();
        VariableRewriteRegression::jsonImportPerformance();
        VariableRewriteRegression::focRefresh();
        if (argc > 1)
                VariableRewriteRegression::runAxf(argv[1]);
        if (argc > 2)
                VariableRewriteRegression::sessionFile(argv[2], argv[1]);
        std::puts("PASS: rewrite lock regression, headless editor, members/arrays/bitfields/manual variables, snapshot "
                  "isolation, offline arrays, merge and numeric bounds");
}
