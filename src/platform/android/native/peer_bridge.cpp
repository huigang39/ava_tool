#include "cJSON.h"
#include "core/remote_device.hpp"
#include <array>
#include <cstring>
#include <jni.h>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace
{
JavaVM *vm{};
struct Attached {
        JNIEnv *env{};
        bool    detach{};
        Attached()
        {
                if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
#ifdef __ANDROID__
                        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
                                throw std::runtime_error("Cannot attach peer worker");
#else
                        if (vm->AttachCurrentThread(reinterpret_cast<void **>(&env), nullptr) != JNI_OK)
                                throw std::runtime_error("Cannot attach peer worker");
#endif
                        detach = true;
                }
        }
        ~Attached()
        {
                if (detach)
                        vm->DetachCurrentThread();
        }
};
std::string
utf(JNIEnv *env, jstring text)
{
        if (!text)
                return {};
        const char *p = env->GetStringUTFChars(text, nullptr);
        if (!p)
                throw std::runtime_error("Missing string");
        std::string s(p);
        env->ReleaseStringUTFChars(text, p);
        return s;
}
void
fail(JNIEnv *e, const std::exception &error)
{
        e->ThrowNew(e->FindClass("java/io/IOException"), error.what());
}
void
check(JNIEnv *e)
{
        if (e->ExceptionCheck()) {
                e->ExceptionClear();
                throw std::runtime_error("Local USB device operation failed");
        }
}
std::string
json(cJSON *value)
{
        char       *p = cJSON_PrintUnformatted(value);
        std::string s = p ? p : "{}";
        cJSON_free(p);
        cJSON_Delete(value);
        return s;
}
struct JavaBackend {
        jobject   object{};
        jmethodID info{}, read{}, write{}, reset{};
        JavaBackend(JNIEnv *e, jobject o)
        {
                object   = e->NewGlobalRef(o);
                auto cls = e->GetObjectClass(o);
                info     = e->GetMethodID(cls, "peerDevice", "()Ljava/lang/String;");
                read     = e->GetMethodID(cls, "peerRead", "(JJ[B)Z");
                write    = e->GetMethodID(cls, "peerWrite", "(JJ[B)Z");
                reset    = e->GetMethodID(cls, "peerReset", "(J)Z");
                e->DeleteLocalRef(cls);
                if (!info || !read || !write || !reset)
                        throw std::runtime_error("Missing peer callbacks");
        }
        ~JavaBackend()
        {
                Attached a;
                a.env->DeleteGlobalRef(object);
        }
        ava_peer::Device device()
        {
                Attached a;
                auto     str = static_cast<jstring>(a.env->CallObjectMethod(object, info));
                check(a.env);
                auto bytes = utf(a.env, str);
                a.env->DeleteLocalRef(str);
                cJSON *j = cJSON_Parse(bytes.c_str());
                if (!j)
                        throw std::runtime_error("Invalid device metadata");
                auto             get   = [&](const char *k) { return cJSON_GetObjectItemCaseSensitive(j, k); };
                auto            *name  = get("name");
                auto            *epoch = get("epoch");
                ava_peer::Device d{name && cJSON_IsString(name) ? name->valuestring : "USB J-Link",
                                   epoch && cJSON_IsNumber(epoch) ? uint64_t(epoch->valuedouble) : 0,
                                   cJSON_IsTrue(get("ready")) != 0,
                                   true,
                                   true};
                cJSON_Delete(j);
                return d;
        }
        bool transfer(bool writing, uint64_t epoch, uint32_t address, uint32_t size, void *data)
        {
                Attached   a;
                jbyteArray bytes = a.env->NewByteArray(jsize(size));
                if (!bytes)
                        throw std::runtime_error("Out of memory");
                if (writing)
                        a.env->SetByteArrayRegion(bytes, 0, jsize(size), static_cast<jbyte *>(data));
                jboolean ok = a.env->CallBooleanMethod(object, writing ? write : read, jlong(epoch), jlong(address), bytes);
                if (!writing && ok)
                        a.env->GetByteArrayRegion(bytes, 0, jsize(size), static_cast<jbyte *>(data));
                a.env->DeleteLocalRef(bytes);
                check(a.env);
                return ok;
        }
        bool doReset(uint64_t epoch)
        {
                Attached a;
                bool     ok = a.env->CallBooleanMethod(object, reset, jlong(epoch));
                check(a.env);
                return ok;
        }
};
std::mutex        backendMutex;
ava_peer::Backend localBackend;
ava_peer::Backend
local()
{
        std::lock_guard lock(backendMutex);
        return localBackend;
}
} // namespace
extern "C" JNIEXPORT jstring JNICALL
Java_com_ava_tool_NativePeer_key(JNIEnv *e, jclass)
{
        try {
                return e->NewStringUTF(ava_peer::randomKey().c_str());
        } catch (const std::exception &x) {
                fail(e, x);
                return nullptr;
        }
}
extern "C" JNIEXPORT jstring JNICALL
Java_com_ava_tool_NativePeer_discover(JNIEnv *e, jclass)
{
        try {
                auto *list = cJSON_CreateArray();
                for (auto p : ava_peer::discover()) {
                        auto *j = cJSON_CreateObject();
                        cJSON_AddStringToObject(j, "name", p.name.c_str());
                        cJSON_AddStringToObject(j, "host", p.host.c_str());
                        cJSON_AddNumberToObject(j, "port", p.port);
                        cJSON_AddItemToArray(list, j);
                }
                return e->NewStringUTF(json(list).c_str());
        } catch (const std::exception &x) {
                fail(e, x);
                return nullptr;
        }
}
extern "C" JNIEXPORT void JNICALL
Java_com_ava_tool_NativePeer_startServer(
    JNIEnv *e, jclass, jobject callbacks, jstring name, jint port, jstring key, jboolean writes)
{
        try {
                if (port < 1 || port > 65535)
                        throw std::runtime_error("Invalid port");
                auto              java = std::make_shared<JavaBackend>(e, callbacks);
                ava_peer::Backend backend;
                backend.device = [java] { return java->device(); };
                backend.read   = [java](uint64_t ep, uint32_t a, uint32_t n, void *p) {
                        return java->transfer(false, ep, a, n, p);
                };
                backend.write = [java](uint64_t ep, uint32_t a, uint32_t n, const void *p) {
                        return java->transfer(true, ep, a, n, const_cast<void *>(p));
                };
                backend.reset = [java](uint64_t ep) { return java->doReset(ep); };
                ava_peer::server().start(backend, utf(e, name), uint16_t(port), utf(e, key), writes);
                std::lock_guard lock(backendMutex);
                localBackend = std::move(backend);
        } catch (const std::exception &x) {
                fail(e, x);
        }
}
extern "C" JNIEXPORT void JNICALL
Java_com_ava_tool_NativePeer_stopServer(JNIEnv *, jclass)
{
        ava_peer::server().stop();
        std::lock_guard lock(backendMutex);
        localBackend = {};
}
extern "C" JNIEXPORT jint JNICALL
Java_com_ava_tool_NativePeer_viewers(JNIEnv *, jclass)
{
        return ava_peer::server().viewers();
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_ava_tool_NativePeer_remoteOwnsWrites(JNIEnv *, jclass)
{
        return ava_peer::remoteOwnsWrites();
}
extern "C" JNIEXPORT void JNICALL
Java_com_ava_tool_NativePeer_connect(JNIEnv *e, jclass, jstring host, jint port, jstring key)
{
        try {
                if (port < 1 || port > 65535)
                        throw std::runtime_error("Invalid port");
                ava_peer::client().connect(utf(e, host), uint16_t(port), utf(e, key));
        } catch (const std::exception &x) {
                fail(e, x);
        }
}
extern "C" JNIEXPORT void JNICALL
Java_com_ava_tool_NativePeer_close(JNIEnv *, jclass)
{
        ava_peer::client().close();
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_ava_tool_NativePeer_connected(JNIEnv *, jclass)
{
        return ava_peer::client().connected();
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_ava_tool_NativePeer_ownsControl(JNIEnv *, jclass)
{
        return ava_peer::client().ownsControl();
}
extern "C" JNIEXPORT void JNICALL
Java_com_ava_tool_NativePeer_claim(JNIEnv *e, jclass, jboolean acquire)
{
        try {
                ava_peer::client().claim(acquire);
        } catch (const std::exception &x) {
                fail(e, x);
        }
}
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_ava_tool_NativePeer_read(JNIEnv *e, jclass, jlong address, jint size)
{
        try {
                if (address < 0 || address > 0xffffffffLL || size < 1 || size > 4096)
                        throw std::runtime_error("Invalid memory range");
                std::vector<uint8_t> bytes(size);
                if (!ava_peer::client().read(uint32_t(address), size, bytes.data()))
                        throw std::runtime_error(ava_peer::client().error());
                auto result = e->NewByteArray(size);
                e->SetByteArrayRegion(result, 0, size, reinterpret_cast<const jbyte *>(bytes.data()));
                return result;
        } catch (const std::exception &x) {
                fail(e, x);
                return nullptr;
        }
}
extern "C" JNIEXPORT void JNICALL
Java_com_ava_tool_NativePeer_write(JNIEnv *e, jclass, jlong address, jbyteArray value, jboolean localDevice)
{
        try {
                int size = e->GetArrayLength(value);
                if (address < 0 || address > 0xffffffffLL || size < 1 || size > 4096 ||
                    uint64_t(address) + size > 0x100000000ULL)
                        throw std::runtime_error("Invalid memory range");
                std::vector<uint8_t> bytes(size);
                e->GetByteArrayRegion(value, 0, size, reinterpret_cast<jbyte *>(bytes.data()));
                if (localDevice) {
                        ava_peer::WriteGuard guard;
                        if (!guard)
                                throw std::runtime_error("Remote controller owns write access");
                        auto b = local();
                        if (!b.device || !b.write(b.device().epoch, uint32_t(address), size, bytes.data()))
                                throw std::runtime_error("Local write failed");
                } else if (!ava_peer::client().write(uint32_t(address), size, bytes.data()))
                        throw std::runtime_error(ava_peer::client().error());
        } catch (const std::exception &x) {
                fail(e, x);
        }
}
extern "C" JNIEXPORT void JNICALL
Java_com_ava_tool_NativePeer_reset(JNIEnv *e, jclass, jboolean localDevice)
{
        try {
                if (localDevice) {
                        ava_peer::WriteGuard guard;
                        if (!guard)
                                throw std::runtime_error("Remote controller owns write access");
                        auto b = local();
                        if (!b.device || !b.reset(b.device().epoch))
                                throw std::runtime_error("Reset failed");
                } else if (!ava_peer::client().reset())
                        throw std::runtime_error(ava_peer::client().error());
        } catch (const std::exception &x) {
                fail(e, x);
        }
}
extern "C" JNIEXPORT jint JNICALL
Java_com_ava_tool_NativePeer_startSamples(JNIEnv *e, jclass, jstring blocks, jint periodUs)
{
        try {
                auto                                            value = utf(e, blocks);
                std::unique_ptr<cJSON, decltype(&cJSON_Delete)> j(cJSON_Parse(value.c_str()), cJSON_Delete);
                if (!j || !cJSON_IsArray(j.get()) || cJSON_GetArraySize(j.get()) > 32)
                        throw std::runtime_error("Invalid sample blocks");
                std::vector<ava_peer::Block> list;
                for (auto *b = j->child; b; b = b->next) {
                        auto *a = cJSON_GetObjectItem(b, "address");
                        auto *n = cJSON_GetObjectItem(b, "size");
                        if (!cJSON_IsNumber(a) || !cJSON_IsNumber(n) || a->valuedouble < 0 || a->valuedouble > 4294967295. ||
                            n->valueint < 1 || n->valueint > 4096)
                                throw std::runtime_error("Invalid block");
                        list.push_back({uint32_t(a->valuedouble), uint32_t(n->valueint)});
                }
                return ava_peer::client().startSamples(list, periodUs);
        } catch (const std::exception &x) {
                fail(e, x);
                return 0;
        }
}
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_ava_tool_NativePeer_samples(JNIEnv *e, jclass)
{
        try {
                std::array<uint8_t, 32768> bytes{};
                int                        n = ava_peer::client().samples(bytes.data(), bytes.size());
                if (n < 0)
                        throw std::runtime_error(ava_peer::client().error());
                auto result = e->NewByteArray(n);
                e->SetByteArrayRegion(result, 0, n, reinterpret_cast<const jbyte *>(bytes.data()));
                return result;
        } catch (const std::exception &x) {
                fail(e, x);
                return nullptr;
        }
}
extern "C" JNIEXPORT void JNICALL
Java_com_ava_tool_NativePeer_stopSamples(JNIEnv *, jclass)
{
        ava_peer::client().stopSamples();
}

extern "C" JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM *machine, void *)
{
        vm = machine;
        return JNI_VERSION_1_6;
}
