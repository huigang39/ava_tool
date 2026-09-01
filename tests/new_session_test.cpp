#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include <future>
#include <fstream>
#include <stdexcept>

static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
struct SimSamplerTestAccess {
    static void reset(Gui &g) { g.newSession(); }
    static bool clean(const Gui &g) { return g.monitors_.empty() && g.vars_.empty() && g.sdkPanels_.empty() && g.isFirstSave_ && !g.isModified_ && g.currentSessionPath_=="session.ava"; }
    static void addLocal(Gui &g) {
        std::lock_guard lock(g.mtxMonitors_);
        auto v=std::make_shared<Variable>("reset-vars");
        v->addLocalVar("sdk_clock",DataType::U64,8);
        g.vars_["reset-vars"]=v;
    }
    static std::weak_ptr<SdkPanel> panel(Gui &g) { g.newSdkPanel(); return g.sdkPanels_.back(); }
    static void startSequence(Gui &g) {
        SequenceStep s; s.kind=SeqStepKind::Sleep; s.sleepMs=2000;
        g.seqEditor_.setSteps({s});
        g.seqEditor_.runSingleStep(0);
    }
    static bool sequenceStopped(Gui &g) { return !g.seqEditor_.seqThread_.joinable() && !g.seqEditor_.seqRunning_.load() && g.seqEditor_.getSteps().empty(); }
    static void discard(Gui &g) { g.skipAutoSave_=true; }
};
struct SdkPanelRegression {
    static void start(SdkPanel &p) {
        wchar_t system[MAX_PATH]; check(GetSystemDirectoryW(system,MAX_PATH)>0,"system directory");
        auto wide=(std::filesystem::path(system)/"kernel32.dll").u8string();
        check(p.loader_.load(std::string(wide.begin(),wide.end())),"load read-only Windows clock fixture");
        CStructDecl table; table.name=table.fullName="clock_rt"; table.totalSize=8;
        table.fields.push_back({"clock",CType::U64,"uint64_t",0,8});
        p.parseResult_.structs.push_back(table);
        CFuncDecl fn; fn.name="GetSystemTimeAsFileTime"; fn.retType=CType::Void;
        fn.params.push_back({"clock",CType::Ptr,"clock_rt *"});
        p.parseResult_.functions.push_back(fn);
        SdkPanel::DataTableBinding b;
        b.name=b.structName="clock_rt"; b.localName="sdk_clock";
        b.kind=SdkPanel::DataTableKind::Realtime; b.readFnIdx=0; b.structParamRead=0;
        b.data.resize(8); b.autoRead=true; b.rootPublished=true;
        p.dataTables_.push_back(b); p.rtUpdateHz_=1000; p.scheduleDataUpdate();
    }
    static bool published(SdkPanel &p) { std::lock_guard lock(p.dataTableMtx_); return p.dataTables_[0].callCount>=3; }
};
int main(int argc,char **argv) {
    if(argc!=2) return 2;
    module_init();
    std::thread sampler,fft;
    std::unique_ptr<Gui> gui;
    int result=0;
    try {
        std::ofstream(argv[1])<<R"({"monitors":[],"variables":[]})";
        check(glfwInit()==GLFW_TRUE,"GLFW"); glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
        gui=std::make_unique<Gui>(argv[1]);
        SimSamplerTestAccess::discard(*gui);
        // Deterministic ownership check: reset cannot finish while a reader owns
        // the sampler mutex. The reader never touches data while testing this
        std::promise<void> locked;
        std::atomic<bool> started=false,finished=false,unprotected=false;
        std::thread owner([&] {
            std::unique_lock lock(gui->getMonitorMtx()); locked.set_value();
            while(!started.load()) std::this_thread::yield();
            auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);
            while(!finished.load() && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
            unprotected=finished.load();
        });
        locked.get_future().wait(); started=true;
        SimSamplerTestAccess::reset(*gui); finished=true; owner.join();
        check(!unprotected,"new session must acquire the sampler container mutex");
        g_appRunning=true; g_samplerRunMode=0;
        sampler=std::thread(threadFunc,gui.get()); fft=std::thread(fftThreadFunc,gui.get());
        for(int iteration=0;iteration<30;++iteration) {
            std::vector<std::shared_ptr<Monitor>> old;
            {
                std::lock_guard lock(gui->getMonitorMtx());
                for(int i=0;i<8;++i) {
                    auto m=std::make_shared<Monitor>("reset-"+std::to_string(i));
                    m->addScope("scope");
                    auto scope=m->getScopes().at("scope"); scope->addChannel("signal");
                    auto ch=scope->getChannels().at("signal"); ch->setDevice("LOCAL"); ch->setType("f32");
                    gui->getMonitors()[m->getName()]=m; old.push_back(m);
                }
            }
            SimSamplerTestAccess::addLocal(*gui);
            auto weak=SimSamplerTestAccess::panel(*gui);
            {
                auto p=weak.lock(); SdkPanelRegression::start(*p);
                auto until=std::chrono::steady_clock::now()+std::chrono::seconds(1);
                while(!SdkPanelRegression::published(*p) && std::chrono::steady_clock::now()<until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                check(SdkPanelRegression::published(*p),"SDK worker is publishing LOCAL samples before reset");
            }
            SimSamplerTestAccess::startSequence(*gui);
            SimSamplerTestAccess::reset(*gui);
            check(SimSamplerTestAccess::clean(*gui),"new session clears old monitors, variables, SDK pages and save state");
            check(weak.expired(),"new session joins/destroys SDK workers before deleting variable buffers");
            check(SimSamplerTestAccess::sequenceStopped(*gui),"sequence worker joined and steps cleared");
            for(auto &m:old) {
                check(m->isPendingDelete(),"sampler snapshot monitor is retired");
                auto scope=m->getScopes().at("scope");
                check(scope->isPendingDelete() && scope->getChannels().at("signal")->isPendingDelete(),"cached scopes/channels are retired");
            }
        }
        SimSamplerTestAccess::reset(*gui);
        check(SimSamplerTestAccess::clean(*gui),"empty reset is repeatable");
        puts("PASS: new-session mutex ownership, active sampler/FFT/SDK/sequence teardown, cached retirement and 30 repeated resets");
    } catch(const std::exception &e) { fprintf(stderr,"FAIL: %s\n",e.what()); result=1; }
    g_appRunning=false;
    if(sampler.joinable()) sampler.join();
    if(fft.joinable()) fft.join();
    gui.reset();
    return result;
}
