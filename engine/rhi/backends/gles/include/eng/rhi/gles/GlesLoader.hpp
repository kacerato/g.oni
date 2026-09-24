#pragma once

/// Loader dinâmico de EGL/OpenGL ES — mesmo padrão do
/// VulkanLoader: dlopen + tabela de ponteiros, zero link com
/// libEGL/libGLESv2. O mesmo binário serve a desktop (libEGL.so.1) e
/// Android (libEGL.so — FASE 7).

#include <cstdint>
#include <string>

// Sem dependência de headers de plataforma (missão §14): EGL_NO_X11 evita
// Xlib; KHR/khrplatform.h vem dos registries Khronos (FetchContent).
#define EGL_NO_X11 1

#include <EGL/egl.h>
#include <GLES3/gl3.h>

namespace eng::rhi::gles {

// Os headers Khronos (EGL-Registry/OpenGL-Registry) NÃO exportam typedefs
// PFN_* (diferente de vulkan.h) — assinaturas canônicas declaradas aqui.
using EglGetErrorFn = EGLint (*)();
using EglQueryStringFn = const char* (*)(EGLDisplay, EGLint);
using EglGetDisplayFn = EGLDisplay (*)(EGLNativeDisplayType);
using EglGetPlatformDisplayFn = EGLDisplay (*)(EGLenum, void*, const EGLAttrib*);
using EglInitializeFn = EGLBoolean (*)(EGLDisplay, EGLint*, EGLint*);
using EglTerminateFn = EGLBoolean (*)(EGLDisplay);
using EglChooseConfigFn = EGLBoolean (*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint,
                                         EGLint*);
using EglGetConfigAttribFn = EGLBoolean (*)(EGLDisplay, EGLConfig, EGLint, EGLint*);
using EglCreatePbufferSurfaceFn = EGLSurface (*)(EGLDisplay, EGLConfig, const EGLint*);
using EglCreateWindowSurfaceFn = EGLSurface (*)(EGLDisplay, EGLConfig, EGLNativeWindowType,
                                                const EGLint*);
using EglDestroySurfaceFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
using EglCreateContextFn = EGLContext (*)(EGLDisplay, EGLConfig, EGLContext,
                                            const EGLint*);
using EglDestroyContextFn = EGLBoolean (*)(EGLDisplay, EGLContext);
using EglMakeCurrentFn = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
using EglSwapBuffersFn = EGLBoolean (*)(EGLDisplay, EGLSurface);

using GlGetStringFn = const GLubyte* (*)(GLenum);
using GlGetStringiFn = const GLubyte* (*)(GLenum, GLuint);
using GlGetErrorFn = GLenum (*)();
using GlGetIntegervFn = void (*)(GLenum, GLint*);
using GlClearColorFn = void (*)(GLfloat, GLfloat, GLfloat, GLfloat);
using GlClearFn = void (*)(GLbitfield);
using GlViewportFn = void (*)(GLint, GLint, GLsizei, GLsizei);
using GlGenVertexArraysFn = void (*)(GLsizei, GLuint*);
using GlBindVertexArrayFn = void (*)(GLuint);
using GlDeleteVertexArraysFn = void (*)(GLsizei, const GLuint*);
using GlEnableVertexAttribArrayFn = void (*)(GLuint);
using GlVertexAttribPointerFn = void (*)(GLuint, GLint, GLenum, GLboolean, GLsizei,
                                          const void*);
using GlGenBuffersFn = void (*)(GLsizei, GLuint*);
using GlBindBufferFn = void (*)(GLenum, GLuint);
using GlBufferDataFn = void (*)(GLenum, GLsizeiptr, const void*, GLenum);
using GlBufferSubDataFn = void (*)(GLenum, GLintptr, GLsizeiptr, const void*);
using GlDeleteBuffersFn = void (*)(GLsizei, const GLuint*);
using GlCreateShaderFn = GLuint (*)(GLenum);
using GlShaderSourceFn = void (*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using GlCompileShaderFn = void (*)(GLuint);
using GlGetShaderivFn = void (*)(GLuint, GLenum, GLint*);
using GlGetShaderInfoLogFn = void (*)(GLuint, GLsizei, GLsizei*, GLchar*);
using GlDeleteShaderFn = void (*)(GLuint);
using GlCreateProgramFn = GLuint (*)();
using GlAttachShaderFn = void (*)(GLuint, GLuint);
using GlLinkProgramFn = void (*)(GLuint);
using GlGetProgramivFn = void (*)(GLuint, GLenum, GLint*);
using GlGetProgramInfoLogFn = void (*)(GLuint, GLsizei, GLsizei*, GLchar*);
using GlDeleteProgramFn = void (*)(GLuint);
using GlUseProgramFn = void (*)(GLuint);
using GlCullFaceFn = void (*)(GLenum);
using GlFrontFaceFn = void (*)(GLenum);
using GlEnableFn = void (*)(GLenum);
using GlDisableFn = void (*)(GLenum);
using GlDepthFuncFn = void (*)(GLenum);
using GlDepthMaskFn = void (*)(GLboolean);
using GlBlendFuncSeparateFn = void (*)(GLenum, GLenum, GLenum, GLenum);
using GlBlendEquationSeparateFn = void (*)(GLenum, GLenum);
using GlDrawArraysFn = void (*)(GLenum, GLint, GLsizei);
using GlDrawElementsFn = void (*)(GLenum, GLsizei, GLenum, const void*);
using GlFinishFn = void (*)();
using GlReadPixelsFn = void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
using GlGenTexturesFn = void (*)(GLsizei, GLuint*);
using GlDeleteTexturesFn = void (*)(GLsizei, const GLuint*);
using GlBindTextureFn = void (*)(GLenum, GLuint);
using GlTexImage2DFn = void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                                 const void*);
using GlTexParameteriFn = void (*)(GLenum, GLenum, GLint);
using GlGenerateMipmapFn = void (*)(GLenum);
using GlActiveTextureFn = void (*)(GLenum);
using GlPixelStoreiFn = void (*)(GLenum, GLint);
// UBO do frame (P3 §2 — luzes 2D; core de GLES 3.0):
using GlBindBufferRangeFn = void (*)(GLenum, GLuint, GLuint, GLintptr, GLsizeiptr);
using GlGetUniformBlockIndexFn = GLuint (*)(GLuint, const GLchar*);
using GlUniformBlockBindingFn = void (*)(GLuint, GLuint, GLuint);

/// Tabela de funções EGL + GLES usadas pelo backend.
struct GlesFunctions {
    // --- EGL ---------------------------------------------------------------
    EglGetErrorFn eglGetError{nullptr};
    EglQueryStringFn eglQueryString{nullptr};
    EglGetDisplayFn eglGetDisplay{nullptr};
    EglGetPlatformDisplayFn eglGetPlatformDisplay{nullptr};
    EglInitializeFn eglInitialize{nullptr};
    EglTerminateFn eglTerminate{nullptr};
    EglChooseConfigFn eglChooseConfig{nullptr};
    EglGetConfigAttribFn eglGetConfigAttrib{nullptr};
    EglCreatePbufferSurfaceFn eglCreatePbufferSurface{nullptr};
    EglCreateWindowSurfaceFn eglCreateWindowSurface{nullptr};
    EglDestroySurfaceFn eglDestroySurface{nullptr};
    EglCreateContextFn eglCreateContext{nullptr};
    EglDestroyContextFn eglDestroyContext{nullptr};
    EglMakeCurrentFn eglMakeCurrent{nullptr};
    EglSwapBuffersFn eglSwapBuffers{nullptr};

    // --- GLES 3 ---------------------------------------------------------------
    GlGetStringFn glGetString{nullptr};
    GlGetStringiFn glGetStringi{nullptr};
    GlGetErrorFn glGetError{nullptr};
    GlGetIntegervFn glGetIntegerv{nullptr};
    GlClearColorFn glClearColor{nullptr};
    GlClearFn glClear{nullptr};
    GlViewportFn glViewport{nullptr};
    GlGenVertexArraysFn glGenVertexArrays{nullptr};
    GlBindVertexArrayFn glBindVertexArray{nullptr};
    GlDeleteVertexArraysFn glDeleteVertexArrays{nullptr};
    GlEnableVertexAttribArrayFn glEnableVertexAttribArray{nullptr};
    GlVertexAttribPointerFn glVertexAttribPointer{nullptr};
    GlGenBuffersFn glGenBuffers{nullptr};
    GlBindBufferFn glBindBuffer{nullptr};
    GlBufferDataFn glBufferData{nullptr};
    GlBufferSubDataFn glBufferSubData{nullptr};
    GlDeleteBuffersFn glDeleteBuffers{nullptr};
    GlCreateShaderFn glCreateShader{nullptr};
    GlShaderSourceFn glShaderSource{nullptr};
    GlCompileShaderFn glCompileShader{nullptr};
    GlGetShaderivFn glGetShaderiv{nullptr};
    GlGetShaderInfoLogFn glGetShaderInfoLog{nullptr};
    GlDeleteShaderFn glDeleteShader{nullptr};
    GlCreateProgramFn glCreateProgram{nullptr};
    GlAttachShaderFn glAttachShader{nullptr};
    GlLinkProgramFn glLinkProgram{nullptr};
    GlGetProgramivFn glGetProgramiv{nullptr};
    GlGetProgramInfoLogFn glGetProgramInfoLog{nullptr};
    GlDeleteProgramFn glDeleteProgram{nullptr};
    GlUseProgramFn glUseProgram{nullptr};
    GlCullFaceFn glCullFace{nullptr};
    GlFrontFaceFn glFrontFace{nullptr};
    GlEnableFn glEnable{nullptr};
    GlDisableFn glDisable{nullptr};
    GlDepthFuncFn glDepthFunc{nullptr};
    GlDepthMaskFn glDepthMask{nullptr};
    GlBlendFuncSeparateFn glBlendFuncSeparate{nullptr};
    GlBlendEquationSeparateFn glBlendEquationSeparate{nullptr};
    GlDrawArraysFn glDrawArrays{nullptr};
    GlDrawElementsFn glDrawElements{nullptr};
    GlFinishFn glFinish{nullptr};
    GlReadPixelsFn glReadPixels{nullptr};
    GlGenTexturesFn glGenTextures{nullptr};
    GlDeleteTexturesFn glDeleteTextures{nullptr};
    GlBindTextureFn glBindTexture{nullptr};
    GlTexImage2DFn glTexImage2D{nullptr};
    GlTexParameteriFn glTexParameteri{nullptr};
    GlGenerateMipmapFn glGenerateMipmap{nullptr};
    GlActiveTextureFn glActiveTexture{nullptr};
    GlPixelStoreiFn glPixelStorei{nullptr};
    GlBindBufferRangeFn glBindBufferRange{nullptr};
    GlGetUniformBlockIndexFn glGetUniformBlockIndex{nullptr};
    GlUniformBlockBindingFn glUniformBlockBinding{nullptr};
};

/// Bibliotecas carregadas (EGL + GLESv2) + tabela.
class GlesLibrary {
public:
    GlesLibrary() = default;
    ~GlesLibrary();

    GlesLibrary(const GlesLibrary&) = delete;
    GlesLibrary& operator=(const GlesLibrary&) = delete;

    /// Abre libEGL + libGLESv2 e resolve TODAS as funções da tabela.
    /// `false` + motivo quando ausentes (probe Unavailable — missão §47).
    [[nodiscard]] bool open(std::string& outError);

    [[nodiscard]] bool isOpen() const noexcept { return eglLibrary_ != nullptr; }
    [[nodiscard]] const GlesFunctions& functions() const noexcept { return functions_; }
    [[nodiscard]] GlesFunctions& functions() noexcept { return functions_; }

private:
    void close() noexcept;

    void* eglLibrary_{nullptr};
    void* glesLibrary_{nullptr};
    GlesFunctions functions_{};
};

/// Nome canônico do código de erro EGL/GL (mensagens precisas — missão §35).
[[nodiscard]] std::string eglErrorName(EGLint error);

} // namespace eng::rhi::gles
