#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include <cmath>
#include <fstream>
#include <stdexcept>

static void check(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

struct FftStartupTestAccess {
    static void run(MonitorScope &scope, std::mutex &mtx) {
        scope.fftWinMin_.store(0);
        scope.fftWinMax_.store(64);
        scope.fftLastRunMs_.store(0);
        scope.fftWorkerStep(mtx);
    }
    static void checkSpectrum(MonitorScope &scope) {
        std::lock_guard lock(scope.fftPubMtx_);
        auto it = scope.fftPublished_.find("signal");
        check(it != scope.fftPublished_.end() && !it->second.freqs.empty(),
              "restored FFT produces a curve without changing point count");
        check(!it->second.peaks.empty(), "FFT publishes peaks");
        check(std::abs(it->second.peaks.front().freq - 128.0) < 0.1,
              "frequency comes from sample timestamps even with zero monitor Hz");
        check(it->second.peaks.front().mag > 0.4, "sine spectrum has nonzero amplitude");
    }
};

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    module_init();
    std::unique_ptr<Gui> gui;
    int result = 0;
    try {
        std::ofstream(argv[1]) << R"({"monitors":[{"name":"fft","scopes":[{"name":"main","showFft":true,"fftPoints":65536,"channels":[{"name":"signal","device":"LOCAL","type":"f32"}]}]}],"variables":[]})";
        check(glfwInit() == GLFW_TRUE, "GLFW initialization");
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        gui = std::make_unique<Gui>(argv[1]);
        auto monitor = gui->getMonitors().at("fft");
        auto scope = monitor->getScopes().at("main");
        check(monitor->getHz() == 0, "startup has no measured sample rate");
        check(scope->getFftPoints() == 65536 && scope->isFftEnabled(), "saved FFT settings restored");
        auto *ch = scope->findChannel("signal");
        check(ch != nullptr, "saved channel restored");
        for (int i = 0; i < 65536; ++i)
            ch->read_.push(static_cast<float>(std::sin(2 * 3.141592653589793 * 128 * i / 1024)),
                           static_cast<double>(i) / 1024);
        FftStartupTestAccess::run(*scope, gui->getMonitorMtx());
        FftStartupTestAccess::checkSpectrum(*scope);
        // Reinitialization must work before sampling, including switching back
        for (int points : {32768, 65536, 65536}) {
            scope->reinitFft(points);
            FftStartupTestAccess::run(*scope, gui->getMonitorMtx());
            FftStartupTestAccess::checkSpectrum(*scope);
        }
        // Malformed saved settings must not destroy the working transform
        for (int points : {0, -1, 65535, 2097152}) {
            scope->reinitFft(points);
            check(scope->getFftPoints() == 65536, "invalid point count leaves FFT intact");
            FftStartupTestAccess::run(*scope, gui->getMonitorMtx());
            FftStartupTestAccess::checkSpectrum(*scope);
        }
        puts("PASS: restored 65536-point FFT, zero startup Hz, point switching and invalid settings");
    } catch (const std::exception &e) {
        fprintf(stderr, "FAIL: %s\n", e.what()); result = 1;
    }
    gui.reset();
    JLinkPort::instance().close();
    log_deinit(&g_log_ops); log_deinit(&g_log_perf);
    return result;
}
