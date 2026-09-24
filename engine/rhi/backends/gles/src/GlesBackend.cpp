/// Backend OpenGL ES — implementação.
/// Contexto EGL real, GLSL compilado/linkado real, VAO/VBO reais, estado
/// aplicado a partir da intenção (missão §37), pbuffer + eglSwapBuffers
/// reais, surface/context loss tratados (missão §38).

#include "eng/rhi/gles/GlesBackend.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "eng/log/Macros.hpp"
#include "eng/rhi/Progress.hpp"

// Surface Android (NDK API — NÃO é JNI; missão §II.4/§XII).
#ifdef __ANDROID__
#include <android/native_window.h>
#endif

ENG_LOG_CATEGORY("rhi.gles")

namespace eng::rhi::gles {
namespace {

using eng::core::Result;
using eng::core::StatusCode;

/// Platform surfaceless da MESA — headless REAL (missão §13: ausência de
/// janela ≠ ausência de GLES). Definido manualmente para não depender de
/// versão do eglext.h. Apenas no Linux (Android usa eglGetDisplay padrão).
#ifndef __ANDROID__
constexpr EGLenum kEglPlatformSurfacelessMesa = 0x31DD;
#endif

[[nodiscard]] eng::core::Error eglErr(std::string_view what, EGLint error) {
    return eng::core::Error{StatusCode::NotSupported,
                             std::string{what} + " (" + eglErrorName(error) + ")"};
}

/// Stride do binding declarado no VertexLayout.
[[nodiscard]] std::uint32_t bindingStrideOf(const eng::rhi::VertexLayout& layout,
                                            std::uint32_t binding) noexcept {
    for (const auto& entry : layout.bindings) {
        if (entry.binding == binding) {
            return entry.stride;
        }
    }
    return 0;
}

/// Software renderers conhecidos (honestidade — missão §47).
[[nodiscard]] bool rendererIsSoftware(const std::string& renderer) {
    return renderer.find("llvmpipe") != std::string::npos ||
           renderer.find("softpipe") != std::string::npos ||
           renderer.find("SwiftShader") != std::string::npos;
}

/// Compila UM estágio GLSL ES; info log completo no erro (missão §35).
[[nodiscard]] Result<GLuint> compileShader(const GlesFunctions& fn, GLenum stage,
                                           std::string_view source, std::string_view what) {
    const GLuint shader = fn.glCreateShader(stage);
    if (shader == 0) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::Unknown, std::string{what} + ": glCreateShader retornou 0"));
    }
    const char* sources[1] = {source.data()};
    const GLint lengths[1] = {static_cast<GLint>(source.size())};
    fn.glShaderSource(shader, 1, sources, lengths);
    fn.glCompileShader(shader);
    GLint status = GL_FALSE;
    fn.glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        GLint logLength = 0;
        fn.glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
        std::string log(static_cast<std::size_t>(logLength > 0 ? logLength : 1), '\0');
        if (logLength > 0) {
            fn.glGetShaderInfoLog(shader, logLength, nullptr, log.data());
        }
        fn.glDeleteShader(shader);
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument,
            std::string{what} + ": GLSL ES não compilou — info log: " + log));
    }
    return shader;
}

} // namespace

// =============================================================================
// Fábrica + probe (missão §10/§47)
// =============================================================================

std::unique_ptr<RhiBackend> createBackend() {
    return std::unique_ptr<RhiBackend>(new GlesBackend());
}

BackendProbe GlesBackend::probe() {
    // Sem efeitos colaterais: abre bibliotecas, fecha, reporta.
    GlesLibrary library;
    std::string error{};
    if (!library.open(error)) {
        return BackendProbe{eng::rhi::Availability::Unavailable, error};
    }
    return BackendProbe{eng::rhi::Availability::Detected,
                        "libEGL + libGLESv2 carregáveis"};
}

// =============================================================================
// initialize (missão §33/§34/§13)
// =============================================================================

Result<void> GlesBackend::makeContextCurrent(bool withSurface) {
    const EGLSurface surf = withSurface ? surface_ : EGL_NO_SURFACE;
    if (!library_.functions().eglMakeCurrent(display_, surf, surf, context_)) {
        const EGLint error = library_.functions().eglGetError();
        if (error == EGL_CONTEXT_LOST) {
            contextLost_ = true;
        }
        return eng::core::makeUnexpected(
            eglErr("rhi.gles: eglMakeCurrent", error));
    }
    return {};
}

Result<void> GlesBackend::createSurface(std::uint32_t width, std::uint32_t height) {
    const auto& fn = library_.functions();
#ifdef __ANDROID__
    // Surface de JANELA REAL a partir do ANativeWindow entregue pela camada
    // Android (missão §XII). O membro window_ é setado no initialize.
    EGLSurface surface =
        fn.eglCreateWindowSurface(display_, config_, window_, nullptr);
    if (surface == EGL_NO_SURFACE) {
        return eng::core::makeUnexpected(
            eglErr("rhi.gles: eglCreateWindowSurface", fn.eglGetError()));
    }
#else
    const EGLint attributes[] = {EGL_WIDTH, static_cast<EGLint>(width),
                                 EGL_HEIGHT, static_cast<EGLint>(height), EGL_NONE};
    EGLSurface surface = fn.eglCreatePbufferSurface(display_, config_, attributes);
    if (surface == EGL_NO_SURFACE) {
        return eng::core::makeUnexpected(
            eglErr("rhi.gles: eglCreatePbufferSurface", fn.eglGetError()));
    }
#endif
    if (surface_ != EGL_NO_SURFACE) {
        fn.eglDestroySurface(display_, surface_);
    }
    surface_ = surface;
    surfaceWidth_ = width;
    surfaceHeight_ = height;
    hasSurface_ = true;
    ++stats_.surfaceRecreations;
    return {};
}

Result<void> GlesBackend::initialize(const RendererConfig& config,
                                      RendererCapabilities& outCapabilities) {
    if (initialized_) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles: já inicializado"));
    }

    // --- bibliotecas reais -----------------------------------------------------
    std::string error{};
    if (!library_.open(error)) {
        return eng::core::makeUnexpected(makeError(StatusCode::NotFound, "rhi.gles: " + error));
    }
    const auto& fn = library_.functions();

    // --- display (missão §13/§33): Android usa o display padrão do sistema;
    // Linux headless usa a plataforma surfaceless da Mesa. ---------------
#ifdef __ANDROID__
    display_ = fn.eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) {
        return eng::core::makeUnexpected(
            eglErr("rhi.gles: eglGetDisplay(EGL_DEFAULT_DISPLAY)", fn.eglGetError()));
    }
#else
    // Linux usa a plataforma surfaceless da Mesa (kinds Xcb/Wayland vêm com
    // as fases de plataforma). GONI_GLES_DEFAULT_DISPLAY=1 é facility de
    // DEV/TESTE: usa o display padrão do EGL (X11/Xvfb) para ambientes de
    // desenvolvimento SEM surfaceless (CI e o app não usam este caminho).
    if (std::getenv("GONI_GLES_DEFAULT_DISPLAY") != nullptr) {
        display_ = fn.eglGetDisplay(EGL_DEFAULT_DISPLAY);
    } else {
        display_ = fn.eglGetPlatformDisplay(kEglPlatformSurfacelessMesa,
                                            EGL_DEFAULT_DISPLAY, nullptr);
    }
    if (display_ == EGL_NO_DISPLAY) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::NotSupported,
            "rhi.gles: plataforma surfaceless indisponível neste ambiente "
            "(kinds Xcb/Wayland/Android vêm com as fases de plataforma)"));
    }
#endif
    if (!fn.eglInitialize(display_, nullptr, nullptr)) {
        return eng::core::makeUnexpected(eglErr("rhi.gles: eglInitialize", fn.eglGetError()));
    }
    // Micro-mark — o display EGL vive (o "instance" do GLES).
    eng::rhi::reportProgress(eng::rhi::rhi_stage::Instance, "ok", "EGL display");

    // --- config ES3 (surface type por plataforma — missão §XII) -----------------
    // Android com janela: EGL_WINDOW_BIT; Linux headless: pbuffer.
    const EGLint windowSurfaceBit =
#ifdef __ANDROID__
        EGL_WINDOW_BIT;
#else
        EGL_PBUFFER_BIT;
#endif
    const EGLint configAttributes[] = {
        EGL_SURFACE_TYPE, config.surface.isValid() ? windowSurfaceBit : 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE};
    EGLint configCount = 0;
    if (!fn.eglChooseConfig(display_, configAttributes, &config_, 1, &configCount) ||
        configCount == 0) {
        return eng::core::makeUnexpected(
            eglErr("rhi.gles: eglChooseConfig (ES3 + RGBA8888)", fn.eglGetError()));
    }

    // --- contexto: 3.2 → 3.1 → 3.0 (REAL, detectado — missão §33) ------------------
    EGLint errorContext = EGL_SUCCESS;
    for (const EGLint minor : {2, 1, 0}) {
        const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3,
                                            EGL_CONTEXT_MINOR_VERSION, minor, EGL_NONE};
        context_ = fn.eglCreateContext(display_, config_, EGL_NO_CONTEXT, contextAttributes);
        if (context_ != EGL_NO_CONTEXT) {
            break;
        }
        errorContext = fn.eglGetError();
    }
    if (context_ == EGL_NO_CONTEXT) {
        fn.eglTerminate(display_);
        display_ = EGL_NO_DISPLAY;
        return eng::core::makeUnexpected(
            eglErr("rhi.gles: eglCreateContext (ES 3.2/3.1/3.0 — mínimo 3.0)", errorContext));
    }
    // Micro-mark — contexto ES3 criado (o "device" do GLES).
    eng::rhi::reportProgress(eng::rhi::rhi_stage::Device, "ok", "EGL context ES3");

    // --- surface (pbuffer) quando pedida (missão §38) --------------------------------
    if (config.surface.isValid()) {
        const bool kindSupported =
#ifdef __ANDROID__
            config.surface.window.kind == eng::rhi::NativeWindowKind::Android;
#else
            config.surface.window.kind == eng::rhi::NativeWindowKind::Headless;
#endif
        if (!kindSupported) {
            fn.eglDestroyContext(display_, context_);
            context_ = EGL_NO_CONTEXT;
            fn.eglTerminate(display_);
            display_ = EGL_NO_DISPLAY;
            return eng::core::makeUnexpected(makeError(
                StatusCode::NotSupported,
                "rhi.gles: surface kind não suportada neste build "
                "(Linux usa Headless; Android usa Android — demais kinds vêm "
                "com as fases de plataforma)"));
        }
#ifdef __ANDROID__
        window_ = static_cast<EGLNativeWindowType>(
            const_cast<void*>(config.surface.window.handle));
#endif
        const auto created = createSurface(config.surface.width, config.surface.height);
        if (!created) {
            fn.eglDestroyContext(display_, context_);
            context_ = EGL_NO_CONTEXT;
            fn.eglTerminate(display_);
            display_ = EGL_NO_DISPLAY;
            return eng::core::makeUnexpected(created.error());
        }
        // Micro-mark — EGLSurface da janela nativa pronta.
        {
            char detail[64];
            std::snprintf(detail, sizeof detail, "EGLSurface %ux%u",
                         config.surface.width, config.surface.height);
            eng::rhi::reportProgress(eng::rhi::rhi_stage::Surface, "ok",
                                     detail);
        }
    }

    // Contexto atual: com surface (render+present) ou sem (device-only —
    // EGL_KHR_surfaceless_context implícito na plataforma surfaceless).
    const auto current = makeContextCurrent(hasSurface_);
    if (!current) {
        return eng::core::makeUnexpected(current.error());
    }

    // --- capabilities REAIS (missão §34) ---------------------------------------------
    RendererCapabilities caps{};
    caps.backendName = "OpenGL ES";
    const auto readString = [&fn](GLenum name) {
        const char* value = reinterpret_cast<const char*>(fn.glGetString(name));
        return value == nullptr ? std::string{} : std::string{value};
    };
    const std::string version = readString(GL_VERSION);
    const std::string renderer = readString(GL_RENDERER);
    const std::string vendor = readString(GL_VENDOR);
    caps.apiVersion = version;
    caps.device.name = renderer;
    caps.device.vendor = vendor;
    caps.device.driver = readString(GL_SHADING_LANGUAGE_VERSION);
    caps.device.kind = rendererIsSoftware(renderer) ? eng::rhi::DeviceKind::Cpu
                                                     : eng::rhi::DeviceKind::Unknown;
    caps.softwareRendering = rendererIsSoftware(renderer);

    auto readInt = [&fn](GLenum name) {
        GLint value = 0;
        fn.glGetIntegerv(name, &value);
        return static_cast<std::uint32_t>(value);
    };
    caps.maxTextureSize = readInt(GL_MAX_TEXTURE_SIZE);
    caps.maxVertexAttributes = readInt(GL_MAX_VERTEX_ATTRIBS);
    caps.maxColorAttachments = readInt(GL_MAX_COLOR_ATTACHMENTS);
    caps.maxUniformBufferSize = readInt(GL_MAX_UNIFORM_BLOCK_SIZE);
    caps.instancing = true;  // core desde ES 3.0
    const std::string& v = version;  // "OpenGL ES 3.2 ..."
    const bool es31 = v.find("3.1") != std::string::npos || v.find("3.2") != std::string::npos;
    caps.compute = es31;  // compute desde ES 3.1
    caps.multisample = readInt(GL_MAX_SAMPLES) > 0;
    caps.wireframe = false;  // GLES 3.x NÃO tem polygon mode (auditoria F6 §3)
    // Formatos do conjunto CORE ES 3.0 (exigidos pela spec — REAIS).
    caps.supportedFormats = {eng::rhi::Format::R8G8B8A8Unorm,   eng::rhi::Format::R8G8B8A8Srgb,
                             eng::rhi::Format::R32G32B32A32Sfloat,
                             eng::rhi::Format::R16G16B16A16Sfloat, eng::rhi::Format::D32Sfloat,
                             eng::rhi::Format::D24UnormS8Uint};
    caps.presentation = hasSurface_;
    // OpenGL ES não possui validation layers (auditoria F6 §3): pedida →
    // Unavailable honesto, NUNCA mascarado (missão §18).
    caps.validationState = config.enableValidation
                               ? eng::rhi::ValidationState::Unavailable
                               : eng::rhi::ValidationState::DisabledByConfiguration;

    stats_.contextVersion = version;
    capabilities_ = caps;
    outCapabilities = caps;
    // --- uniforms do frame (P3 §2): UBO compartilhado, região por chamada ---
    fn.glGenBuffers(1, &uniformGlBuffer_);
    fn.glBindBuffer(GL_UNIFORM_BUFFER, uniformGlBuffer_);
    fn.glBufferData(GL_UNIFORM_BUFFER, kMaxFrameUniformData, nullptr, GL_DYNAMIC_DRAW);
    fn.glBindBuffer(GL_UNIFORM_BUFFER, 0);
    uniformCursor_ = 0;

    initialized_ = true;
    ENG_INFO("rhi.gles: inicializado — {} ({})", version,
             hasSurface_ ? "com pbuffer (present real)" : "device-only");
    if (config.enableValidation) {
        ENG_WARN("rhi.gles: validation pedida — OpenGL ES não possui layers; "
                 "estado reportado: Unavailable");
    }
    return {};
}

const RendererCapabilities& GlesBackend::capabilities() const {
    return capabilities_;
}

// =============================================================================
// Destruição (ownership — ADR-035)
// =============================================================================

void GlesBackend::destroyAll() noexcept {
    if (display_ != EGL_NO_DISPLAY) {
        const auto& fn = library_.functions();
        if (uniformGlBuffer_ != 0) {
            fn.glDeleteBuffers(1, &uniformGlBuffer_);
            uniformGlBuffer_ = 0;
        }
        fn.eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        for (auto entry : buffers_.drainAll()) {
            fn.glDeleteBuffers(1, &entry.buffer);
        }
        for (auto entry : shaders_.drainAll()) {
            fn.glDeleteProgram(entry.program);
        }
        for (auto entry : pipelines_.drainAll()) {
            fn.glDeleteProgram(entry.program);
            fn.glDeleteVertexArrays(1, &entry.vao);
        }
        for (auto entry : textures_.drainAll()) {
            fn.glDeleteTextures(1, &entry.texture);
        }
        // sampler GLES = parâmetros aplicados no bind (sem objeto GL).
        (void)samplers_.drainAll();
        if (context_ != EGL_NO_CONTEXT) {
            fn.eglDestroyContext(display_, context_);
            context_ = EGL_NO_CONTEXT;
        }
        if (surface_ != EGL_NO_SURFACE) {
            fn.eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
        }
        fn.eglTerminate(display_);
        display_ = EGL_NO_DISPLAY;
    }
    initialized_ = false;
    hasSurface_ = false;
    surfaceLost_ = false;
    contextLost_ = false;
    activeFrameId_ = 0;
    pendingPresents_ = 0;
    for (auto& slot : applied_) {
        slot = {};
    }
}

GlesBackend::~GlesBackend() {
    destroyAll();
}

// =============================================================================
// Estado auxiliar
// =============================================================================

void GlesBackend::configureVertexAttributes(const GlesFunctions& fn) {
    for (const auto& attribute : activePipelineDesc_.vertexLayout.attributes) {
        fn.glEnableVertexAttribArray(attribute.location);
        fn.glVertexAttribPointer(
            attribute.location, static_cast<GLint>(formatComponents(attribute.format)),
            toGlVertexType(attribute.format), isGlNormalizedFormat(attribute.format),
            static_cast<GLsizei>(
                bindingStrideOf(activePipelineDesc_.vertexLayout, attribute.binding)),
            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(attribute.offset)));
    }
}

Result<void> GlesBackend::requireInitialized() const {
    if (!initialized_) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles: não inicializado"));
    }
    return {};
}

bool GlesBackend::isRecording(std::uint64_t frameId) const {
    return frameId != 0 && frameId == activeFrameId_;
}

// =============================================================================
// Recursos (missão §36)
// =============================================================================

Result<BufferHandle> GlesBackend::createBuffer(const BufferDesc& desc) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    if (desc.size == 0) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles.buffer: tamanho zero"));
    }
    if (desc.initialData.size() > desc.size) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles.buffer: initialData > size"));
    }
    const auto& fn = library_.functions();
    BufferEntry entry{};
    entry.size = desc.size;
    entry.usage = desc.usage;
    fn.glGenBuffers(1, &entry.buffer);
    const GLenum target =
        (desc.usage & eng::rhi::BufferUsage::Index) != eng::rhi::BufferUsage::None
            ? GL_ELEMENT_ARRAY_BUFFER
            : GL_ARRAY_BUFFER;
    fn.glBindBuffer(target, entry.buffer);
    fn.glBufferData(target, static_cast<GLsizeiptr>(desc.size), desc.initialData.data(),
                    GL_STATIC_DRAW);
    fn.glBindBuffer(target, 0);
    return BufferHandle{buffers_.insert(std::move(entry))};
}

Result<void> GlesBackend::updateBuffer(BufferHandle handle, std::size_t offset,
                                        std::span<const std::byte> data) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    BufferEntry* entry = buffers_.find(handle.id);
    if (entry == nullptr) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles.buffer: handle nulo/stale"));
    }
    if (offset > entry->size || data.size() > entry->size - offset) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument,
            "rhi.gles.buffer: update fora dos limites (offset=" + std::to_string(offset) +
                " tamanho=" + std::to_string(data.size()) + " buffer=" +
                std::to_string(entry->size) + ")"));
    }
    const auto& fn = library_.functions();
    const GLenum target =
        (entry->usage & eng::rhi::BufferUsage::Index) != eng::rhi::BufferUsage::None
            ? GL_ELEMENT_ARRAY_BUFFER
            : GL_ARRAY_BUFFER;
    fn.glBindBuffer(target, entry->buffer);
    fn.glBufferSubData(target, static_cast<GLintptr>(offset),
                       static_cast<GLsizeiptr>(data.size()), data.data());
    fn.glBindBuffer(target, 0);
    return {};
}

Result<void> GlesBackend::destroyBuffer(BufferHandle handle) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    BufferEntry entry{};
    if (!buffers_.remove(handle.id, entry)) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles.buffer: handle nulo/stale/double"));
    }
    library_.functions().glDeleteBuffers(1, &entry.buffer);
    return {};
}

// =============================================================================
// Shaders — GLSL ES REAL com info logs (missão §35)
// =============================================================================

Result<ShaderHandle> GlesBackend::createShader(const ShaderDesc& desc) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    if (desc.vertexGlsl.empty() || desc.fragmentGlsl.empty()) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::NotSupported,
            "rhi.gles.shader: backend OpenGL ES exige GLSL ES (vertexGlsl e "
            "fragmentGlsl) — a representação SPIR-V é do backend Vulkan"));
    }
    const auto& fn = library_.functions();

    auto vertex = compileShader(fn, GL_VERTEX_SHADER, desc.vertexGlsl, "vertex");
    if (!vertex) {
        return eng::core::makeUnexpected(vertex.error());
    }
    auto fragment = compileShader(fn, GL_FRAGMENT_SHADER, desc.fragmentGlsl, "fragment");
    if (!fragment) {
        fn.glDeleteShader(vertex.value());
        return eng::core::makeUnexpected(fragment.error());
    }

    const GLuint program = fn.glCreateProgram();
    if (program == 0) {
        fn.glDeleteShader(vertex.value());
        fn.glDeleteShader(fragment.value());
        return eng::core::makeUnexpected(
            makeError(StatusCode::Unknown, "rhi.gles.shader: glCreateProgram retornou 0"));
    }
    fn.glAttachShader(program, vertex.value());
    fn.glAttachShader(program, fragment.value());
    fn.glLinkProgram(program);
    GLint status = GL_FALSE;
    fn.glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        GLint logLength = 0;
        fn.glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
        std::string log(static_cast<std::size_t>(logLength > 0 ? logLength : 1), '\0');
        if (logLength > 0) {
            fn.glGetProgramInfoLog(program, logLength, nullptr, log.data());
        }
        fn.glDeleteProgram(program);
        fn.glDeleteShader(vertex.value());
        fn.glDeleteShader(fragment.value());
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument, "rhi.gles.shader: link falhou — info log: " + log));
    }
    // Estágias podem ser liberadas após o link (spec ES 3).
    fn.glDeleteShader(vertex.value());
    fn.glDeleteShader(fragment.value());
    // Bloco de uniforms do frame (P3 §2): GLSL ES 3.00 não tem qualifier
    // binding — o backend atribui o bloco nomeado ao índice 0 de
    // GL_UNIFORM_BUFFER (contrato do ShaderDesc::uniformBlockName).
    if (!desc.uniformBlockName.empty()) {
        const GLuint blockIndex = fn.glGetUniformBlockIndex(
            program, std::string(desc.uniformBlockName).c_str());
        if (blockIndex == 0xFFFFFFFFu) {
            fn.glDeleteProgram(program);
            return eng::core::makeUnexpected(makeError(
                StatusCode::InvalidArgument,
                "rhi.gles.shader: bloco de uniform '" +
                    std::string(desc.uniformBlockName) +
                    "' não existe no shader (GL_INVALID_INDEX)"));
        }
        fn.glUniformBlockBinding(program, blockIndex, 0);
    }
    return ShaderHandle{shaders_.insert(ShaderEntry{program})};
}

Result<void> GlesBackend::destroyShader(ShaderHandle handle) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    ShaderEntry entry{};
    if (!shaders_.remove(handle.id, entry)) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles.shader: handle nulo/stale/double"));
    }
    library_.functions().glDeleteProgram(entry.program);
    return {};
}

// =============================================================================
// Pipeline — intenção traduzida: VAO + estado aplicado no bind (missão §37)
// =============================================================================

Result<GraphicsPipelineHandle> GlesBackend::createGraphicsPipeline(
    const GraphicsPipelineDesc& desc) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    ShaderEntry* shader = shaders_.find(desc.shader.id);
    if (shader == nullptr) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles.pipeline: shader nulo/stale"));
    }
    if (desc.raster.fill == eng::rhi::FillMode::Wireframe) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument,
            "rhi.gles.pipeline: wireframe não existe em OpenGL ES (polygon mode ausente)"));
    }
    if (desc.renderTarget.depthFormat != eng::rhi::Format::Undefined) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::NotSupported,
            "rhi.gles.pipeline: depth attachment entra com o render-graph futuro"));
    }
    if (desc.depth.test || desc.depth.write) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument,
            "rhi.gles.pipeline: DepthState com render target color-only"));
    }
    // L1: formato explícito deve coincidir com o do pbuffer (RGBA8).
    if (desc.renderTarget.colorFormat != eng::rhi::Format::Undefined &&
        toGlFormat(desc.renderTarget.colorFormat) != GL_RGBA8) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument,
            "rhi.gles.pipeline: colorFormat explícito difere do formato real (RGBA8)"));
    }

    const auto& fn = library_.functions();
    PipelineEntry entry{};
    entry.program = shader->program;
    entry.desc = desc;

    // Valida mapeamentos AGORA; a configuração REAL dos atributos do VAO
    // acontece no DRAW com o VBO bound (modelo clássico GL:
    // glVertexAttribPointer captura o buffer bound no momento da chamada —
    // a pipeline ainda não conhece VBOs).
    for (const auto& attribute : desc.vertexLayout.attributes) {
        if (toGlVertexType(attribute.format) == 0 ||
            formatComponents(attribute.format) == 0 ||
            bindingStrideOf(desc.vertexLayout, attribute.binding) == 0) {
            return eng::core::makeUnexpected(
                makeError(StatusCode::InvalidArgument,
                          "rhi.gles.pipeline: layout de vértice sem mapeamento GL "
                          "(formato/stride do binding)"));
        }
    }
    fn.glGenVertexArrays(1, &entry.vao);
    return GraphicsPipelineHandle{pipelines_.insert(std::move(entry))};
}

Result<void> GlesBackend::destroyGraphicsPipeline(GraphicsPipelineHandle handle) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    PipelineEntry entry{};
    if (!pipelines_.remove(handle.id, entry)) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument, "rhi.gles.pipeline: handle nulo/stale/double"));
    }
    // program pertence ao ShaderHandle (compartilhado): não destruímos aqui.
    library_.functions().glDeleteVertexArrays(1, &entry.vao);
    return {};
}

// =============================================================================
// Texturas e samplers (evolução — sprites/UI/preview)
// =============================================================================

Result<TextureHandle> GlesBackend::createTexture(const TextureDesc& desc) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    if (desc.width == 0 || desc.height == 0) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument,
            "rhi.gles.texture: dimensões zero (" + std::to_string(desc.width) + "x" +
                std::to_string(desc.height) + ")"));
    }
    if (desc.initialData.size() != desc.expectedDataSize()) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument,
            "rhi.gles.texture: initialData (" + std::to_string(desc.initialData.size()) +
                " bytes) != width*height*4 (" +
                std::to_string(desc.expectedDataSize()) + " bytes)"));
    }
    const GLenum internalFormat = toGlFormat(desc.format);
    if (internalFormat == 0) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::NotSupported,
            "rhi.gles.texture: formato sem mapeamento GLES (use R8G8B8A8Unorm/Srgb)"));
    }
    auto current = makeContextCurrent(hasSurface_);
    if (!current) {
        return eng::core::makeUnexpected(current.error());
    }
    const auto& fn = library_.functions();

    TextureEntry entry{};
    entry.width = desc.width;
    entry.height = desc.height;
    entry.format = desc.format;
    entry.mipmaps = desc.generateMipmaps;
    fn.glGenTextures(1, &entry.texture);
    if (fn.glGetError() != GL_NO_ERROR) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::OutOfMemory, "rhi.gles.texture: glGenTextures falhou"));
    }
    fn.glBindTexture(GL_TEXTURE_2D, entry.texture);
    // Upload tight-packed: alinhamento de linha = 1 (RGBA8 já é alinhado,
    // mas o contrato é explícito — sem padding de linha).
    fn.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    fn.glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat),
                    static_cast<GLsizei>(desc.width), static_cast<GLsizei>(desc.height), 0,
                    GL_RGBA, GL_UNSIGNED_BYTE, desc.initialData.data());
    const GLenum uploadError = fn.glGetError();
    if (uploadError != GL_NO_ERROR) {
        fn.glDeleteTextures(1, &entry.texture);
        return eng::core::makeUnexpected(makeError(
            StatusCode::Unknown, "rhi.gles.texture: glTexImage2D falhou (" +
                                     std::to_string(uploadError) + ")"));
    }
    if (desc.generateMipmaps) {
        fn.glGenerateMipmap(GL_TEXTURE_2D);
        if (fn.glGetError() != GL_NO_ERROR) {
            fn.glDeleteTextures(1, &entry.texture);
            return eng::core::makeUnexpected(
                makeError(StatusCode::Unknown, "rhi.gles.texture: glGenerateMipmap falhou"));
        }
    }
    fn.glBindTexture(GL_TEXTURE_2D, 0);
    return TextureHandle{textures_.insert(std::move(entry))};
}

Result<SamplerHandle> GlesBackend::createSampler(const SamplerDesc& desc) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    // GLES: sampler = parâmetros aplicados no bind (sem objeto sampler —
    // decisão documentada: interface pequena, estado por textura).
    SamplerEntry entry{};
    entry.desc = desc;
    return SamplerHandle{samplers_.insert(std::move(entry))};
}

Result<void> GlesBackend::destroyTexture(TextureHandle handle) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    TextureEntry entry{};
    if (!textures_.remove(handle.id, entry)) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument, "rhi.gles.texture: handle nulo/stale/double"));
    }
    auto current = makeContextCurrent(hasSurface_);
    if (!current) {
        return eng::core::makeUnexpected(current.error());
    }
    library_.functions().glDeleteTextures(1, &entry.texture);
    for (auto& slot : applied_) {
        slot = {};
    }
    return {};
}

Result<void> GlesBackend::destroySampler(SamplerHandle handle) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    SamplerEntry entry{};
    if (!samplers_.remove(handle.id, entry)) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument, "rhi.gles.sampler: handle nulo/stale/double"));
    }
    for (auto& slot : applied_) {
        slot = {};
    }
    return {};
}

// =============================================================================
// Frame lifecycle (missão §12/§38)
// =============================================================================

Result<BeginFrameResult> GlesBackend::beginFrame() {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    if (!hasSurface_) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::NotSupported,
            "rhi.gles: beginFrame sem surface (modo device-only — missão §13)"));
    }
    if (surfaceLost_ || contextLost_) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::NotSupported,
            "rhi.gles: surface/contexto perdidos — recrie via resize() ou novo Renderer"));
    }
    if (activeFrameId_ != 0) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument,
            "rhi.gles: frame anterior não finalizado (uma sessão de gravação)"));
    }
    const auto& fn = library_.functions();

    // Comportamento alinhado ao Vulkan (auditoria F6 §3.2): clear PRETO no
    // início do frame; frameClear sobrepõe com a cor exata.
    fn.glClearColor(0.f, 0.f, 0.f, 1.f);
    fn.glClear(GL_COLOR_BUFFER_BIT);
    activeFrameId_ = ++nextFrameId_;
    uniformCursor_ = 0;  // orçamento de uniforms do frame recomeça
    pipelineSet_ = false;
    boundVbo_ = 0;
    boundEbo_ = 0;
    return BeginFrameResult{eng::rhi::FrameAcquireStatus::Renderable, activeFrameId_};
}

Result<void> GlesBackend::frameClear(std::uint64_t frameId, const ClearDesc& clear) {
    if (!isRecording(frameId)) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles.frame: sessão inválida"));
    }
    if (clear.clearColor) {
        library_.functions().glClearColor(clear.color[0], clear.color[1], clear.color[2],
                                          clear.color[3]);
        library_.functions().glClear(GL_COLOR_BUFFER_BIT);
    }
    return {};
}

Result<void> GlesBackend::frameSetViewport(std::uint64_t frameId, const Viewport& viewport) {
    if (!isRecording(frameId)) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles.frame: sessão inválida"));
    }
    library_.functions().glViewport(
        static_cast<GLint>(viewport.x), static_cast<GLint>(viewport.y),
        static_cast<GLsizei>(viewport.width), static_cast<GLsizei>(viewport.height));
    return {};
}

Result<void> GlesBackend::frameSetPipeline(std::uint64_t frameId,
                                            GraphicsPipelineHandle pipeline) {
    if (!isRecording(frameId)) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles.frame: sessão inválida"));
    }
    PipelineEntry* entry = pipelines_.find(pipeline.id);
    if (entry == nullptr) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles.frame: pipeline nulo/stale"));
    }
    const auto& fn = library_.functions();
    // INTENÇÃO → estado GL (missão §37). VAO do pipeline; atributos são
    // (re)configurados no draw com o VBO bound (modelo clássico GL).
    fn.glUseProgram(entry->program);
    fn.glBindVertexArray(entry->vao);
    pipelineSet_ = true;
    activePipelineDesc_ = entry->desc;
    const auto& desc = entry->desc;
    if (desc.raster.cull == eng::rhi::CullMode::None) {
        fn.glDisable(GL_CULL_FACE);
    } else {
        fn.glEnable(GL_CULL_FACE);
        fn.glCullFace(desc.raster.cull == eng::rhi::CullMode::Front ? GL_FRONT : GL_BACK);
    }
    fn.glFrontFace(desc.raster.front == eng::rhi::FrontFace::Clockwise ? GL_CW : GL_CCW);
    if (desc.depth.test) {
        fn.glEnable(GL_DEPTH_TEST);
    } else {
        fn.glDisable(GL_DEPTH_TEST);
    }
    fn.glDepthFunc(toGlDepthFunc(desc.depth.compare));
    fn.glDepthMask(desc.depth.write ? GL_TRUE : GL_FALSE);
    if (desc.blend.enabled) {
        fn.glEnable(GL_BLEND);
        fn.glBlendFuncSeparate(toGlBlendFactor(desc.blend.srcColor),
                               toGlBlendFactor(desc.blend.dstColor),
                               toGlBlendFactor(desc.blend.srcColor),
                               toGlBlendFactor(desc.blend.dstColor));
        fn.glBlendEquationSeparate(toGlBlendEquation(desc.blend.colorOp),
                                   toGlBlendEquation(desc.blend.colorOp));
    } else {
        fn.glDisable(GL_BLEND);
    }
    return {};
}

Result<void> GlesBackend::frameBindVertexBuffer(std::uint64_t frameId, BufferHandle buffer) {
    if (!isRecording(frameId)) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles.frame: sessão inválida"));
    }
    BufferEntry* entry = buffers_.find(buffer.id);
    if (entry == nullptr) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles.frame: buffer nulo/stale"));
    }
    if ((entry->usage & eng::rhi::BufferUsage::Vertex) == eng::rhi::BufferUsage::None) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles.frame: buffer sem uso Vertex"));
    }
    boundVbo_ = entry->buffer;  // aplicado no draw (com atributos do VAO)
    return {};
}

Result<void> GlesBackend::frameBindIndexBuffer(std::uint64_t frameId, BufferHandle buffer,
                                                IndexType indexType) {
    if (!isRecording(frameId)) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles.frame: sessão inválida"));
    }
    BufferEntry* entry = buffers_.find(buffer.id);
    if (entry == nullptr) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles.frame: buffer nulo/stale"));
    }
    if ((entry->usage & eng::rhi::BufferUsage::Index) == eng::rhi::BufferUsage::None) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles.frame: buffer sem uso Index"));
    }
    boundEbo_ = entry->buffer;  // aplicado no drawIndexed (dentro do VAO)
    currentIndexType_ = indexType;
    return {};
}

Result<void> GlesBackend::frameBindTexture(std::uint64_t frameId, TextureHandle texture,
                                           SamplerHandle sampler, std::uint32_t slot) {
    if (!isRecording(frameId)) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles.frame: sessão inválida"));
    }
    if (slot >= kMaxTextureSlots) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument,
            "rhi.gles.frame: slot de textura inválido (kMaxTextureSlots=" +
                std::to_string(kMaxTextureSlots) + ")"));
    }
    TextureEntry* textureEntry = textures_.find(texture.id);
    if (textureEntry == nullptr) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles.frame: textura nula/stale"));
    }
    SamplerEntry* samplerEntry = samplers_.find(sampler.id);
    if (samplerEntry == nullptr) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles.frame: sampler nulo/stale"));
    }
    const auto& fn = library_.functions();
    fn.glActiveTexture(GL_TEXTURE0 + slot);
    fn.glBindTexture(GL_TEXTURE_2D, textureEntry->texture);

    // Parâmetros do sampler aplicados na textura bound (modelo GL clássico;
    // objetos sampler separados entram com materiais 3D).
    using F = eng::rhi::FilterMode;
    using A = eng::rhi::AddressMode;
    const SamplerDesc& s = samplerEntry->desc;
    const GLint mag = s.magFilter == F::Nearest ? GL_NEAREST : GL_LINEAR;
    GLint min = s.minFilter == F::Nearest ? GL_NEAREST : GL_LINEAR;
    if (textureEntry->mipmaps) {
        // Filtro entre níveis só existe com a cadeia de mips.
        min = s.mipFilter == F::Nearest
                  ? (s.minFilter == F::Nearest ? GL_NEAREST_MIPMAP_NEAREST
                                               : GL_LINEAR_MIPMAP_NEAREST)
                  : (s.minFilter == F::Nearest ? GL_NEAREST_MIPMAP_LINEAR
                                               : GL_LINEAR_MIPMAP_LINEAR);
    }
    fn.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
    fn.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min);
    fn.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                       s.addressU == A::ClampToEdge ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    fn.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                       s.addressV == A::ClampToEdge ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    applied_[slot] = AppliedTexture{texture.id, sampler.id};
    return {};
}

Result<void> GlesBackend::frameSetUniformData(std::uint64_t frameId,
                                                std::span<const std::byte> data) {
    if (!isRecording(frameId)) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles.frame: sessão inválida"));
    }
    if (data.empty()) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument, "rhi.gles.frame: setUniformData com dados vazios"));
    }
    if (data.size() > kMaxFrameUniformData) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument,
                      "rhi.gles.frame: setUniformData excede kMaxFrameUniformData"));
    }
    // Região bump-alocada (análogo do dynamic offset Vulkan).
    const GLsizeiptr cursor = uniformCursor_;
    const GLsizeiptr aligned =
        (cursor + kUniformRegionAlign - 1) & ~(kUniformRegionAlign - 1);
    const GLsizeiptr end = aligned + static_cast<GLsizeiptr>(data.size());
    if (end > kMaxFrameUniformData) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument,
            "rhi.gles.frame: orçamento de uniforms do frame exaurido"));
    }
    const auto& fn = library_.functions();
    fn.glBindBuffer(GL_UNIFORM_BUFFER, uniformGlBuffer_);
    fn.glBufferSubData(GL_UNIFORM_BUFFER, aligned,
                       static_cast<GLsizeiptr>(data.size()), data.data());
    // O bloco "PerFrame" do program vive no binding 0 (atribuído no link);
    // a REGIÃO seleciona o conjunto de dados (luzes por camada, ...).
    fn.glBindBufferRange(GL_UNIFORM_BUFFER, 0, uniformGlBuffer_, aligned,
                         static_cast<GLsizeiptr>(data.size()));
    uniformCursor_ = end;
    return {};
}

Result<void> GlesBackend::frameDraw(std::uint64_t frameId, std::uint32_t vertexCount,
                                    std::uint32_t firstVertex) {
    if (!isRecording(frameId)) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles.frame: sessão inválida"));
    }
    if (vertexCount == 0) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles.frame: draw com vertexCount 0"));
    }
    if (!pipelineSet_ || boundVbo_ == 0) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles.frame: draw exige pipeline+VBO"));
    }
    const auto& fn = library_.functions();
    fn.glBindBuffer(GL_ARRAY_BUFFER, boundVbo_);
    configureVertexAttributes(fn);
    fn.glDrawArrays(GL_TRIANGLES, static_cast<GLint>(firstVertex),
                    static_cast<GLsizei>(vertexCount));
    return {};
}

Result<void> GlesBackend::frameDrawIndexed(std::uint64_t frameId, std::uint32_t indexCount,
                                           std::uint32_t firstIndex) {
    if (!isRecording(frameId)) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.gles.frame: sessão inválida"));
    }
    if (indexCount == 0) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument, "rhi.gles.frame: drawIndexed com indexCount 0"));
    }
    if (!pipelineSet_ || boundVbo_ == 0 || boundEbo_ == 0) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument, "rhi.gles.frame: drawIndexed exige pipeline+VBO+EBO"));
    }
    const auto& fn = library_.functions();
    fn.glBindBuffer(GL_ARRAY_BUFFER, boundVbo_);
    configureVertexAttributes(fn);
    fn.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, boundEbo_);
    const GLenum type = currentIndexType_ == eng::rhi::IndexType::Uint32 ? GL_UNSIGNED_INT
                                                                        : GL_UNSIGNED_SHORT;
    const void* offset =
        reinterpret_cast<const void*>(static_cast<std::uintptr_t>(firstIndex) *
                                       (type == GL_UNSIGNED_INT ? 4u : 2u));
    library_.functions().glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indexCount), type,
                                        offset);
    return {};
}

Result<void> GlesBackend::endFrame(std::uint64_t frameId) {
    if (!isRecording(frameId)) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::InvalidArgument, "rhi.gles: endFrame sem sessão ativa/end duplo"));
    }
    library_.functions().glFinish();
    activeFrameId_ = 0;
    ++stats_.framesSubmitted;
    ++pendingPresents_;  // L3: aguarda present()
    return {};
}

Result<void> GlesBackend::present() {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    if (!hasSurface_) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::NotSupported, "rhi.gles: present sem surface (device-only)"));
    }
    if (pendingPresents_ == 0) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::NotSupported, "rhi.gles: present sem frame submetido (missão §12)"));
    }
    const auto& fn = library_.functions();
    while (pendingPresents_ > 0) {
        --pendingPresents_;
        ++stats_.presentsSubmitted;
        if (!fn.eglSwapBuffers(display_, surface_)) {
            const EGLint error = fn.eglGetError();
            if (error == EGL_CONTEXT_LOST) {
                contextLost_ = true;
                return eng::core::makeUnexpected(
                    eglErr("rhi.gles: contexto perdido ao apresentar", error));
            }
            if (error == EGL_BAD_SURFACE) {
                surfaceLost_ = true;
                return eng::core::makeUnexpected(
                    eglErr("rhi.gles: surface perdida ao apresentar", error));
            }
            return eng::core::makeUnexpected(eglErr("rhi.gles: eglSwapBuffers", error));
        }
        ++stats_.presentsOk;
    }
    return {};
}

Result<void> GlesBackend::resize(std::uint32_t width, std::uint32_t height) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    if (!hasSurface_) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::NotSupported, "rhi.gles: resize sem surface"));
    }
    // Recrea o pbuffer (missão §38: surface recriada); contexto preservado.
    const auto created = createSurface(width, height);
    if (created) {
        surfaceLost_ = false;
        const auto current = makeContextCurrent(true);
        if (!current) {
            return eng::core::makeUnexpected(current.error());
        }
        ENG_INFO("rhi.gles: pbuffer recriado para {}x{}", width, height);
    }
    return created;
}

bool GlesBackend::surfaceLost() const {
    return surfaceLost_ || contextLost_;
}

Result<void> GlesBackend::readCenterPixel(std::uint8_t outRgba[4]) {
    if (auto ready = requireInitialized(); !ready) {
        return eng::core::makeUnexpected(ready.error());
    }
    if (!hasSurface_) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::NotSupported, "rhi.gles: readback sem surface"));
    }
    std::memset(outRgba, 0, 4);
    library_.functions().glReadPixels(static_cast<GLint>(surfaceWidth_ / 2),
                                      static_cast<GLint>(surfaceHeight_ / 2), 1, 1, GL_RGBA,
                                      GL_UNSIGNED_BYTE, outRgba);
    return {};
}

} // namespace eng::rhi::gles
