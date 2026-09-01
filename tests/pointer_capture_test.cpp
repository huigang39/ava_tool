#define main ava_app_main
#define WinMain ava_app_win_main
#include "../src/main.cpp"
#undef WinMain
#undef main
#include "imgui_internal.h"
#include "platform/app_capture.hpp"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <stdexcept>

static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }

struct VariableRewriteRegression {
        static void initialize(Variable &v) {
                auto &info = v.dwarfInfo_; info.present = true;
                auto &number = info.types[1];
                number.kind = dwarf::TypeKind::BASE; number.name = "uint32_t"; number.size = 4; number.encoding = 7;
                auto &pointer = info.types[2];
                pointer.kind = dwarf::TypeKind::POINTER; pointer.inner = 3; pointer.size = 4;
                auto &node = info.types[3];
                node.kind = dwarf::TypeKind::STRUCT; node.name = "Node"; node.size = 16;
                node.members = {{"value", 0, 1}, {"next", 4, 2}, {"items", 8, 4}};
                auto &array = info.types[4];
                array.kind = dwarf::TypeKind::ARRAY; array.inner = 1; array.size = 8; array.dims = {2};
                auto &wide = info.types[5]; wide.kind = dwarf::TypeKind::POINTER; wide.size = 8; wide.inner = 1;
                VarEntry entry{};
                entry.name = "head"; entry.port = PortType::LOCAL; entry.addr = 0x1000; entry.type = DataType::U32; entry.typeOff = 2;
                v.vars_.push_back(entry);
                auto &data = v.localBufs_[entry.name]; data.resize(128);
                const u32 pointerValue = 0x1010, value = 42, second = 73;
                std::memcpy(data.data(), &pointerValue, 4);
                std::memcpy(data.data() + 16, &value, 4);
                std::memcpy(data.data() + 20, &pointerValue, 4); // circular Node::next
                std::memcpy(data.data() + 24, &second, 4);
                v.pointerView_ = {true, entry.name, entry.name, entry.port, entry.addr, entry.addr, 2, v.elfLayoutGeneration_, {}};
        }
        static std::string render(Variable &v) {
                ImGui::NewFrame();
                ImGui::LogToBuffer(12);
                v.drawPointerInspector();
                const std::string output = ImGui::GetCurrentContext()->LogBuffer.c_str();
                ImGui::LogFinish();
                ImGui::Render();
                return output;
        }
        static ImVec2 buttonFrame(Variable &v, ImVec2 mouse, bool down) {
                auto &io = ImGui::GetIO(); io.AddMousePosEvent(mouse.x, mouse.y); io.AddMouseButtonEvent(0, down);
                ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(500, 160));
                ImGui::Begin("pointer-button-host");
                ImGui::BeginTable("pointer-button-table", 1); ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                v.drawPointerButton(0, 0x1000, 2, "head");
                const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                ImGui::EndTable();
                ImGui::End(); ImGui::Render();
                return ImVec2(min.x + 10, (min.y + max.y) * .5f);
        }
        static void run() {
                Variable v("pointer-test"); initialize(v);
                const auto original = v.localBufs_["head"];
                v.pointerView_.open = false;
                buttonFrame(v, ImVec2(-1000, -1000), false);
                auto point = buttonFrame(v, ImVec2(-1000, -1000), false);
                buttonFrame(v, point, true);
                const u32 moved = 0x1040;
                std::memcpy(v.localBufs_["head"].data(), &moved, 4);
                buttonFrame(v, point, false);
                check(v.pointerView_.open && v.pointerView_.address == 0x1000, "pointer button opens even when address changes during click");
                v.localBufs_["head"] = original;
                ImGui::GetIO().AddMousePosEvent(-1000, -1000);
                auto text = render(v); text = render(v);
                check(text.find("42") != std::string::npos, "pointer reads pointee scalar");
                check(text.find("73") != std::string::npos, "pointer reads nested arrays");
                check(text.find("Circular reference") != std::string::npos, "recursive struct pointers stop at cycles");
                check(v.localBufs_["head"] == original, "pointer inspector never writes memory");
                const u32 null = 0;
                std::memcpy(v.localBufs_["head"].data(), &null, 4);
                text = render(v);
                check(text.find("nullptr") != std::string::npos && text.find("42") == std::string::npos, "null pointer hides old target");
                const u32 bad = 0xF0000000;
                std::memcpy(v.localBufs_["head"].data(), &bad, 4);
                text = render(v);
                check(text.find("Read failed") != std::string::npos && text.find("42") == std::string::npos, "unmapped pointer cannot reuse stale contents");
                u8 bytes[8]{};
                check(v.readPointerMemory(v.vars_[0], UINT64_MAX, 8, bytes) == Variable::PointerRead::Error, "address overflow rejected");
                check(v.readPointerMemory(v.vars_[0], 0x1000, 3, bytes) == Variable::PointerRead::Error, "unsupported byte widths rejected");
                v.vars_[0].port = PortType::JLINK;
                check(v.readPointerMemory(v.vars_[0], 0x1000, 4, bytes) == Variable::PointerRead::Disconnected, "offline J-Link never follows pointers");
                check(v.memberPollReqs_.empty(), "offline pointer inspection schedules no memory read");
                v.vars_[0].port = PortType::LOCAL;
                v.vars_[0].typeOff = v.pointerView_.typeOff = 5;
                v.vars_[0].type = DataType::U64;
                const u64 full = 0x100000030ULL;
                std::memcpy(v.localBufs_["head"].data(), &full, 8);
                VarEntry target{}; target.name = "large_address"; target.addr = full; target.port = PortType::LOCAL; target.type = DataType::U32;
                v.vars_.push_back(target); v.localBufs_[target.name] = {123, 0, 0, 0};
                text = render(v);
                check(text.find("0000000100000030") != std::string::npos && text.find("123") != std::string::npos, "64-bit pointer address retained and resolved across registered buffers");
                ++v.elfLayoutGeneration_;
                text = render(v);
                check(text.find("symbols changed") != std::string::npos, "symbol reload invalidates pointer layout");
                puts("PASS: 32/64-bit pointer values, structures, arrays, cycles, nulls, unmapped memory, disconnect, reload and read-only snapshots");
        }
};

static app_capture::Frame pattern(int width, int height, int64_t time) {
        app_capture::Frame frame{width, height, std::vector<uint8_t>(size_t(width) * height * 4), time};
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                auto *p = frame.rgba.data() + (size_t(y) * width + x) * 4;
                p[0] = y >= height / 2 ? 255 : 0; // top red / bottom green in GL order
                p[1] = y < height / 2 ? 255 : 0; p[2] = 0; p[3] = 255;
        }
        return frame;
}
static void captureTest() {
        using namespace app_capture;
        const auto tinyFrame = pattern(4, 4, nowUs());
        const auto dib = clipboardDib(tinyFrame);
        check(dib.size() == 104 && dib[0] == 40 && dib[4] == 4 && dib[8] == 4 && dib[14] == 32, "clipboard DIB header/dimensions");
        check(dib[40] == 0 && dib[41] == 255 && dib[42] == 0 && dib[74] == 255, "clipboard BGRX color order and bottom-up orientation");
        const auto nv12 = toNv12(tinyFrame, 4, 4);
        check(nv12.size() == 24 && nv12[0] >= 61 && nv12[0] <= 64 && nv12[12] > 170, "video RGB to BT709 and top-down orientation");
        check(clipboardDib({}).empty() && toNv12(tinyFrame, 3, 4).empty(), "invalid capture buffers rejected");
        Recorder recorder;
        const std::string path = "pointer-capture-test.mp4";
        check(recorder.start(path, 320, 180), "recording starts");
        check(!recorder.start("second.mp4", 320, 180), "a second start cannot replace active recording");
        const auto start = nowUs();
        for (int i = 0; i < 30; ++i) {
                const auto time = start + int64_t(i) * 40000;
                const auto deadline = nowUs() + 10000000;
                while (!recorder.wantsFrame(time) && recorder.state() != Recorder::State::Idle && nowUs() < deadline)
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                if (recorder.state() == Recorder::State::Idle) {
                        auto error = recorder.takeResult();
                        throw std::runtime_error(error ? error->error : "encoder stopped unexpectedly");
                }
                check(nowUs() < deadline, "encoder queue drains");
                recorder.submit(pattern(i < 15 ? 320 : 200, i < 15 ? 180 : 200, time)); // resize mid-recording
        }
        recorder.stop(); recorder.finish();
        auto result = recorder.takeResult();
        if (!result || !result->ok) throw std::runtime_error(result ? result->error : "no recording completion");
        check(std::filesystem::file_size(path) > 1000, "MP4 has video payload");
        using Microsoft::WRL::ComPtr;
        check(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init for decoder");
        check(SUCCEEDED(MFStartup(MF_VERSION)), "MF init for decoder");
        {
                ComPtr<IMFSourceReader> reader;
                check(SUCCEEDED(MFCreateSourceReaderFromURL(std::filesystem::path(path).c_str(), nullptr, &reader)), "finalized MP4 opens in native decoder");
                ComPtr<IMFMediaType> format;
                check(SUCCEEDED(MFCreateMediaType(&format)), "decoder format");
                format->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                format->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
                check(SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, format.Get())), "native H.264 decoding");
                unsigned count = 0; LONGLONG previous = -1;
                while (true) {
                        DWORD flags{}; LONGLONG time{}; ComPtr<IMFSample> sample;
                        check(SUCCEEDED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &time, &sample)), "MP4 samples decode");
                        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
                        if (!sample) continue;
                        check(time > previous, "recording timestamps strictly increase"); previous = time;
                        if (count == 0) {
                                ComPtr<IMFMediaBuffer> buffer; sample->ConvertToContiguousBuffer(&buffer);
                                BYTE *data{}; DWORD length{}; check(SUCCEEDED(buffer->Lock(&data, nullptr, &length)), "decoded pixels");
                                check(length >= 320 * 180 && std::abs(int(data[50 * 320 + 160]) - 63) < 10 && data[140 * 320 + 160] > 160,
                                      "decoded video is upright and colors preserved");
                                buffer->Unlock();
                        }
                        ++count;
                }
                check(count == 30 && previous >= 11000000, "all frames survive finalize, with real-time sample spacing");
        }
        MFShutdown(); CoUninitialize();
        check(recorder.start("missing-folder/capture.mp4", 320, 180), "invalid destination reports asynchronously");
        recorder.finish(); result = recorder.takeResult();
        check(result && !result->ok && !result->error.empty(), "encoder failure is returned to UI");
        check(recorder.state() == Recorder::State::Idle, "recorder recovers after errors");
        check(recorder.start("empty.mp4", 320, 180), "recorder can restart");
        recorder.finish(); result = recorder.takeResult();
        check(result && !result->ok, "empty recording cannot claim success");
        puts("PASS: clipboard bitmap orientation, NV12 conversion, native H.264 encode/decode, resizing, timestamps, finalize, errors and restart");
}

int main() {
        module_init(); ImGui::CreateContext(); ImPlot::CreateContext();
        auto &io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = ImVec2(1400, 900); io.DeltaTime = 1.f / 60;
        unsigned char *pixels; int w, h; io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        g_lang = Lang::EN;
        int result = 0;
        try { VariableRewriteRegression::run(); captureTest(); }
        catch (const std::exception &e) { fprintf(stderr, "FAIL: %s\n", e.what()); result = 1; }
        ImPlot::DestroyContext(); ImGui::DestroyContext(); log_deinit(&g_log_ops); log_deinit(&g_log_perf);
        return result;
}
