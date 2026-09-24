#include <jni.h>

#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <cstdint>
#include <memory>
#include <string>

#include "eng/editor/Diagnostics.hpp"
#include "eng/editor/EditorHost.hpp"
#include "eng/editor/EditorProtocol.hpp"

/// Ponte JNI do editor — espelha com.goni.app.NativeBridge.
///
/// Só três tipos de chamada cruzam a fronteira:
///   - ciclo de vida/surface/frame do host;
///   - estado e operações do editor, em JSON (snapshot/call — ver
///     EditorProtocol.hpp; a lógica mora lá e é testada no Linux);
///   - o caminho quente dos gestos do viewport (toque/gizmo/pan/zoom), que
///     roda a cada evento de movimento e não passa por JSON.
///
/// Todas as chamadas chegam da thread de UI; o handle é um Session*.

namespace {

using eng::editor::EditorDocument;
using eng::editor::EditorHost;
using eng::editor::EditorProtocol;

struct Session {
    std::unique_ptr<EditorHost> host;
    std::unique_ptr<EditorProtocol> protocol;
};

Session* session(jlong handle)
{
    return reinterpret_cast<Session*>(static_cast<std::uintptr_t>(handle));
}

EditorHost* hostOf(jlong handle)
{
    Session* s = session(handle);
    return s != nullptr ? s->host.get() : nullptr;
}

std::string toStd(JNIEnv* env, jstring value)
{
    if (value == nullptr) {
        return {};
    }
    const jsize bytes = env->GetStringUTFLength(value);
    const jsize units = env->GetStringLength(value);
    if (bytes <= 0 || units < 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(bytes), '\0');
    env->GetStringUTFRegion(value, 0, units, out.data());
    return out;
}

jstring toJava(JNIEnv* env, const std::string& text)
{
    return env->NewStringUTF(text.c_str());
}

eng::ecs::Entity unpack(jlong packed)
{
    return EditorDocument::unpackEntity(static_cast<std::uint64_t>(packed));
}

} // namespace

#ifdef __ANDROID__
#include <android/thermal.h>
#include <dlfcn.h>

namespace {

/// Status térmico (ADPF) via dlsym: a API só existe no Android 11+.
class ThermalProvider final {
public:
    ThermalProvider()
    {
        void* lib = dlopen("libandroid.so", RTLD_NOW);
        if (lib == nullptr) {
            return;
        }
        auto acquire = reinterpret_cast<AThermalManager* (*)()>(
            dlsym(lib, "AThermal_acquireManager"));
        getStatus_ = reinterpret_cast<int (*)(AThermalManager*)>(
            dlsym(lib, "AThermal_getCurrentThermalStatus"));
        if (acquire != nullptr) {
            manager_ = acquire();
        }
    }
    [[nodiscard]] int status() const noexcept
    {
        return (manager_ != nullptr && getStatus_ != nullptr)
                   ? getStatus_(manager_)
                   : -1;
    }

private:
    AThermalManager* manager_ = nullptr;
    int (*getStatus_)(AThermalManager*) = nullptr;
};

void installThermal(EditorHost& host)
{
    static ThermalProvider provider;
    host.setThermalProvider(
        [](void* user) -> int {
            return static_cast<ThermalProvider*>(user)->status();
        },
        &provider);
}

} // namespace
#else
namespace {
void installThermal(EditorHost&) {}
} // namespace
#endif

extern "C" {

// --- processo -------------------------------------------------------------------

/// Abre o log de diagnóstico e instala o crash handler. Devolve true se a
/// execução anterior terminou em crash (a UI oferece exportar o relatório).
JNIEXPORT jboolean JNICALL
Java_com_goni_app_NativeBridge_nativeInit(JNIEnv* env, jobject, jstring dir)
{
    const std::string path = toStd(env, dir);
    eng::editor::diag::init(path.c_str());
    eng::editor::diag::mark("NATIVE_READY", "ok", "libgoni.so");
    return eng::editor::diag::hasPreviousCrashReport() ? JNI_TRUE : JNI_FALSE;
}

// --- sessão -----------------------------------------------------------------------

JNIEXPORT jlong JNICALL
Java_com_goni_app_NativeBridge_nativeCreate(JNIEnv* env, jobject,
                                            jstring workspace)
{
    const std::string root = toStd(env, workspace);
    auto created = EditorHost::create("auto", root.c_str());
    if (created.isError()) {
        return 0;
    }
    auto* s = new Session;
    s->host.reset(created.value());
    s->protocol = std::make_unique<EditorProtocol>(s->host->document(),
                                                   s->host.get());
    installThermal(*s->host);
    return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(s));
}

JNIEXPORT void JNICALL
Java_com_goni_app_NativeBridge_nativeDestroy(JNIEnv*, jobject, jlong handle)
{
    delete session(handle);
}

JNIEXPORT jstring JNICALL
Java_com_goni_app_NativeBridge_nativeSnapshot(JNIEnv* env, jobject,
                                              jlong handle, jlong sinceKey)
{
    Session* s = session(handle);
    if (s == nullptr) {
        return nullptr;
    }
    const std::string snap =
        s->protocol->snapshot(static_cast<std::uint64_t>(sinceKey));
    return snap.empty() ? nullptr : toJava(env, snap);
}

JNIEXPORT jstring JNICALL
Java_com_goni_app_NativeBridge_nativeCall(JNIEnv* env, jobject, jlong handle,
                                          jstring request)
{
    Session* s = session(handle);
    if (s == nullptr) {
        return toJava(env, R"({"ok":false,"error":"editor não iniciado"})");
    }
    return toJava(env, s->protocol->call(toStd(env, request)));
}

// --- surface / ciclo de vida -------------------------------------------------------

JNIEXPORT void JNICALL
Java_com_goni_app_NativeBridge_nativeSurfaceCreated(JNIEnv* env, jobject,
                                                    jlong handle,
                                                    jobject surface)
{
    EditorHost* host = hostOf(handle);
    if (host == nullptr) {
        return;
    }
    // fromSurface adquire uma referência; o host guarda a própria.
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (window == nullptr) {
        return;
    }
    host->surfaceCreated(window, eng::rhi::NativeWindowKind::Android, 0, 0);
    ANativeWindow_release(window);
}

JNIEXPORT void JNICALL
Java_com_goni_app_NativeBridge_nativeSurfaceChanged(JNIEnv*, jobject,
                                                    jlong handle, jint width,
                                                    jint height)
{
    if (EditorHost* host = hostOf(handle)) {
        host->surfaceChanged(static_cast<std::uint32_t>(width),
                             static_cast<std::uint32_t>(height));
        host->document().setGameViewportSize(static_cast<float>(width),
                                             static_cast<float>(height));
    }
}

JNIEXPORT void JNICALL
Java_com_goni_app_NativeBridge_nativeSurfaceDestroyed(JNIEnv*, jobject,
                                                      jlong handle)
{
    if (EditorHost* host = hostOf(handle)) {
        host->surfaceDestroyed();
    }
}

JNIEXPORT void JNICALL
Java_com_goni_app_NativeBridge_nativeOnPause(JNIEnv*, jobject, jlong handle)
{
    if (EditorHost* host = hostOf(handle)) {
        host->onPause();
    }
}

JNIEXPORT void JNICALL
Java_com_goni_app_NativeBridge_nativeOnResume(JNIEnv*, jobject, jlong handle)
{
    if (EditorHost* host = hostOf(handle)) {
        host->onResume();
    }
}

JNIEXPORT jboolean JNICALL
Java_com_goni_app_NativeBridge_nativeRenderFrame(JNIEnv*, jobject,
                                                 jlong handle, jfloat dt)
{
    EditorHost* host = hostOf(handle);
    return (host != nullptr && host->renderFrame(dt)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_goni_app_NativeBridge_nativeSetUiScale(JNIEnv*, jobject,
                                                jlong handle, jfloat scale)
{
    if (EditorHost* host = hostOf(handle)) {
        host->document().viewport().setUiScale(scale);
    }
}

// --- gestos do viewport (caminho quente) --------------------------------------------

JNIEXPORT jlong JNICALL
Java_com_goni_app_NativeBridge_nativeTap(JNIEnv*, jobject, jlong handle,
                                         jfloat x, jfloat y)
{
    EditorHost* host = hostOf(handle);
    if (host == nullptr) {
        return 0;
    }
    const auto hit = host->document().viewportTap(x, y, &host->textureCache());
    return hit.has_value()
               ? static_cast<jlong>(EditorDocument::packEntity(*hit))
               : 0;
}

/// Entidade sob o dedo sem mudar a seleção (0 = nenhuma).
JNIEXPORT jlong JNICALL
Java_com_goni_app_NativeBridge_nativePick(JNIEnv*, jobject, jlong handle,
                                          jfloat x, jfloat y)
{
    EditorHost* host = hostOf(handle);
    if (host == nullptr) {
        return 0;
    }
    const auto hit = host->document().viewportPick(x, y, &host->textureCache());
    return hit.has_value()
               ? static_cast<jlong>(EditorDocument::packEntity(*hit))
               : 0;
}

JNIEXPORT void JNICALL
Java_com_goni_app_NativeBridge_nativePan(JNIEnv*, jobject, jlong handle,
                                         jfloat dx, jfloat dy)
{
    if (EditorHost* host = hostOf(handle)) {
        host->document().viewportPan(dx, dy);
    }
}

JNIEXPORT void JNICALL
Java_com_goni_app_NativeBridge_nativeZoom(JNIEnv*, jobject, jlong handle,
                                          jfloat factor, jfloat fx, jfloat fy)
{
    if (EditorHost* host = hostOf(handle)) {
        host->document().viewportZoom(factor, fx, fy);
    }
}

/// Começa um arrasto de gizmo. Devolve true se o toque pegou uma alça.
JNIEXPORT jboolean JNICALL
Java_com_goni_app_NativeBridge_nativeGizmoBegin(JNIEnv*, jobject, jlong handle,
                                                jfloat x, jfloat y)
{
    EditorHost* host = hostOf(handle);
    if (host == nullptr) {
        return JNI_FALSE;
    }
    return host->document().gizmoDragBegin(x, y, &host->textureCache()) !=
                   eng::editor::GizmoHandle::None
               ? JNI_TRUE
               : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_goni_app_NativeBridge_nativeGizmoDrag(JNIEnv*, jobject, jlong handle,
                                               jfloat x, jfloat y)
{
    if (EditorHost* host = hostOf(handle)) {
        (void)host->document().gizmoDragTo(x, y);
    }
}

JNIEXPORT void JNICALL
Java_com_goni_app_NativeBridge_nativeGizmoEnd(JNIEnv*, jobject, jlong handle)
{
    if (EditorHost* host = hostOf(handle)) {
        host->document().gizmoDragEnd();
    }
}

/// Arrasta a entidade pelo corpo (sem alça): delta em pixels de tela.
JNIEXPORT void JNICALL
Java_com_goni_app_NativeBridge_nativeDragEntity(JNIEnv*, jobject, jlong handle,
                                                jlong packed, jfloat dx,
                                                jfloat dy)
{
    if (EditorHost* host = hostOf(handle)) {
        (void)host->document().moveEntityScreen(unpack(packed), dx, dy);
    }
}

/// Toque do jogo em Play. phase: 0 down, 1 move, 2 up, 3 cancel.
JNIEXPORT void JNICALL
Java_com_goni_app_NativeBridge_nativeGameTouch(JNIEnv*, jobject, jlong handle,
                                               jint phase, jint pointer,
                                               jfloat x, jfloat y,
                                               jfloat pressure)
{
    if (EditorHost* host = hostOf(handle)) {
        host->document().gameTouch(phase, static_cast<std::uint32_t>(pointer),
                                   x, y, pressure);
    }
}

} // extern "C"
