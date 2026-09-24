/// Loader EGL/GLES — implementação. Ver GlesLoader.hpp.

#include "eng/rhi/gles/GlesLoader.hpp"

#include <dlfcn.h>

#include "eng/log/Macros.hpp"

ENG_LOG_CATEGORY("rhi.gles")

namespace eng::rhi::gles {
namespace {

constexpr const char* kEglCandidates[] = {"libEGL.so.1", "libEGL.so"};
constexpr const char* kGlesCandidates[] = {"libGLESv2.so.2", "libGLESv2.so"};

template <typename Fn>
Fn resolve(void* library, const char* name) {
    return reinterpret_cast<Fn>(dlsym(library, name));
}

} // namespace

GlesLibrary::~GlesLibrary() {
    close();
}

void GlesLibrary::close() noexcept {
    // NÃO dlclose: libEGL/libGLESv2 carregam drivers (libEGL_mesa +
    // libgallium) que retêm threads/estados além do eglTerminate — o
    // dlclose desmapeia os módulos e quebra a atribuição de stacks do LSan
    // (alocações legítimas do driver viram "leaks" com módulo aleatório).
    // Prática consolidada de loaders gráficos (volk idem): handles vivem
    // até o fim do processo; o OS recupera a memória.
    eglLibrary_ = nullptr;
    glesLibrary_ = nullptr;
    functions_ = GlesFunctions{};
}

bool GlesLibrary::open(std::string& outError) {
    if (eglLibrary_ != nullptr) {
        return true;
    }
    for (const char* candidate : kEglCandidates) {
        eglLibrary_ = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
        if (eglLibrary_ != nullptr) {
            break;
        }
    }
    if (eglLibrary_ == nullptr) {
        outError = std::string{"libEGL ausente: "} + dlerror();
        return false;
    }
    for (const char* candidate : kGlesCandidates) {
        glesLibrary_ = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
        if (glesLibrary_ != nullptr) {
            break;
        }
    }
    if (glesLibrary_ == nullptr) {
        outError = std::string{"libGLESv2 ausente: "} + dlerror();
        close();
        return false;
    }

    auto& fn = functions_;
#define ENG_GL_LOAD(lib, name, Fn)                                      \
    do {                                                                \
        fn.name = resolve<Fn>(lib, #name);                             \
        if (fn.name == nullptr) {                                       \
            outError = "função " #name " ausente";                      \
            close();                                                    \
            return false;                                               \
        }                                                               \
    } while (false)

    ENG_GL_LOAD(eglLibrary_, eglGetError, EglGetErrorFn);
    ENG_GL_LOAD(eglLibrary_, eglQueryString, EglQueryStringFn);
    ENG_GL_LOAD(eglLibrary_, eglGetDisplay, EglGetDisplayFn);
    ENG_GL_LOAD(eglLibrary_, eglGetPlatformDisplay, EglGetPlatformDisplayFn);
    ENG_GL_LOAD(eglLibrary_, eglInitialize, EglInitializeFn);
    ENG_GL_LOAD(eglLibrary_, eglTerminate, EglTerminateFn);
    ENG_GL_LOAD(eglLibrary_, eglChooseConfig, EglChooseConfigFn);
    ENG_GL_LOAD(eglLibrary_, eglGetConfigAttrib, EglGetConfigAttribFn);
    ENG_GL_LOAD(eglLibrary_, eglCreatePbufferSurface, EglCreatePbufferSurfaceFn);
    ENG_GL_LOAD(eglLibrary_, eglCreateWindowSurface, EglCreateWindowSurfaceFn);
    ENG_GL_LOAD(eglLibrary_, eglDestroySurface, EglDestroySurfaceFn);
    ENG_GL_LOAD(eglLibrary_, eglCreateContext, EglCreateContextFn);
    ENG_GL_LOAD(eglLibrary_, eglDestroyContext, EglDestroyContextFn);
    ENG_GL_LOAD(eglLibrary_, eglMakeCurrent, EglMakeCurrentFn);
    ENG_GL_LOAD(eglLibrary_, eglSwapBuffers, EglSwapBuffersFn);

    ENG_GL_LOAD(glesLibrary_, glGetString, GlGetStringFn);
    ENG_GL_LOAD(glesLibrary_, glGetStringi, GlGetStringiFn);
    ENG_GL_LOAD(glesLibrary_, glGetError, GlGetErrorFn);
    ENG_GL_LOAD(glesLibrary_, glGetIntegerv, GlGetIntegervFn);
    ENG_GL_LOAD(glesLibrary_, glClearColor, GlClearColorFn);
    ENG_GL_LOAD(glesLibrary_, glClear, GlClearFn);
    ENG_GL_LOAD(glesLibrary_, glViewport, GlViewportFn);
    ENG_GL_LOAD(glesLibrary_, glGenVertexArrays, GlGenVertexArraysFn);
    ENG_GL_LOAD(glesLibrary_, glBindVertexArray, GlBindVertexArrayFn);
    ENG_GL_LOAD(glesLibrary_, glDeleteVertexArrays, GlDeleteVertexArraysFn);
    ENG_GL_LOAD(glesLibrary_, glEnableVertexAttribArray, GlEnableVertexAttribArrayFn);
    ENG_GL_LOAD(glesLibrary_, glVertexAttribPointer, GlVertexAttribPointerFn);
    ENG_GL_LOAD(glesLibrary_, glGenBuffers, GlGenBuffersFn);
    ENG_GL_LOAD(glesLibrary_, glBindBuffer, GlBindBufferFn);
    ENG_GL_LOAD(glesLibrary_, glBufferData, GlBufferDataFn);
    ENG_GL_LOAD(glesLibrary_, glBufferSubData, GlBufferSubDataFn);
    ENG_GL_LOAD(glesLibrary_, glDeleteBuffers, GlDeleteBuffersFn);
    ENG_GL_LOAD(glesLibrary_, glCreateShader, GlCreateShaderFn);
    ENG_GL_LOAD(glesLibrary_, glShaderSource, GlShaderSourceFn);
    ENG_GL_LOAD(glesLibrary_, glCompileShader, GlCompileShaderFn);
    ENG_GL_LOAD(glesLibrary_, glGetShaderiv, GlGetShaderivFn);
    ENG_GL_LOAD(glesLibrary_, glGetShaderInfoLog, GlGetShaderInfoLogFn);
    ENG_GL_LOAD(glesLibrary_, glDeleteShader, GlDeleteShaderFn);
    ENG_GL_LOAD(glesLibrary_, glCreateProgram, GlCreateProgramFn);
    ENG_GL_LOAD(glesLibrary_, glAttachShader, GlAttachShaderFn);
    ENG_GL_LOAD(glesLibrary_, glLinkProgram, GlLinkProgramFn);
    ENG_GL_LOAD(glesLibrary_, glGetProgramiv, GlGetProgramivFn);
    ENG_GL_LOAD(glesLibrary_, glGetProgramInfoLog, GlGetProgramInfoLogFn);
    ENG_GL_LOAD(glesLibrary_, glDeleteProgram, GlDeleteProgramFn);
    ENG_GL_LOAD(glesLibrary_, glUseProgram, GlUseProgramFn);
    ENG_GL_LOAD(glesLibrary_, glCullFace, GlCullFaceFn);
    ENG_GL_LOAD(glesLibrary_, glFrontFace, GlFrontFaceFn);
    ENG_GL_LOAD(glesLibrary_, glEnable, GlEnableFn);
    ENG_GL_LOAD(glesLibrary_, glDisable, GlDisableFn);
    ENG_GL_LOAD(glesLibrary_, glDepthFunc, GlDepthFuncFn);
    ENG_GL_LOAD(glesLibrary_, glDepthMask, GlDepthMaskFn);
    ENG_GL_LOAD(glesLibrary_, glBlendFuncSeparate, GlBlendFuncSeparateFn);
    ENG_GL_LOAD(glesLibrary_, glBlendEquationSeparate, GlBlendEquationSeparateFn);
    ENG_GL_LOAD(glesLibrary_, glDrawArrays, GlDrawArraysFn);
    ENG_GL_LOAD(glesLibrary_, glDrawElements, GlDrawElementsFn);
    ENG_GL_LOAD(glesLibrary_, glFinish, GlFinishFn);
    ENG_GL_LOAD(glesLibrary_, glReadPixels, GlReadPixelsFn);
    ENG_GL_LOAD(glesLibrary_, glGenTextures, GlGenTexturesFn);
    ENG_GL_LOAD(glesLibrary_, glDeleteTextures, GlDeleteTexturesFn);
    ENG_GL_LOAD(glesLibrary_, glBindTexture, GlBindTextureFn);
    ENG_GL_LOAD(glesLibrary_, glTexImage2D, GlTexImage2DFn);
    ENG_GL_LOAD(glesLibrary_, glTexParameteri, GlTexParameteriFn);
    ENG_GL_LOAD(glesLibrary_, glGenerateMipmap, GlGenerateMipmapFn);
    ENG_GL_LOAD(glesLibrary_, glActiveTexture, GlActiveTextureFn);
    ENG_GL_LOAD(glesLibrary_, glPixelStorei, GlPixelStoreiFn);
    ENG_GL_LOAD(glesLibrary_, glBindBufferRange, GlBindBufferRangeFn);
    ENG_GL_LOAD(glesLibrary_, glGetUniformBlockIndex, GlGetUniformBlockIndexFn);
    ENG_GL_LOAD(glesLibrary_, glUniformBlockBinding, GlUniformBlockBindingFn);
#undef ENG_GL_LOAD

    ENG_INFO("rhi.gles: libEGL + libGLESv2 carregadas");
    return true;
}

std::string eglErrorName(EGLint error) {
    switch (error) {
    case EGL_SUCCESS: return "EGL_SUCCESS";
    case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
    case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
    case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
    case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
    case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
    case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
    case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
    case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
    case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
    case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
    case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
    case EGL_CONTEXT_LOST: return "EGL_CONTEXT_LOST";
    default: return "EGL_<código " + std::to_string(error) + ">";
    }
}

} // namespace eng::rhi::gles
