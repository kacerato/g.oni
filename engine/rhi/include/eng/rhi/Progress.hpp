#ifndef ENG_RHI_PROGRESS_HPP
#define ENG_RHI_PROGRESS_HPP

/// \file Progress.hpp
/// \brief P3.5 — hook de progresso da criação do backend RHI.
///
/// Micro-marks da janela resume→surface (RHI_BACKEND_SELECT /
/// RHI_INSTANCE / RHI_DEVICE / RHI_SURFACE / RHI_SWAPCHAIN): o Realme C33
/// morria entre STARTUP_RESUME ok e STARTUP_SURFACE — cada sub-passo da
/// criação do renderer agora deixa rastro persistido.
///
/// Mesmo padrão do eng::audio: a camada de RHI não conhece o
/// diagnóstico (grafo acíclico) — o HOST instala o hook antes de
/// Renderer::create e remove depois. Header separado do Renderer.hpp
/// porque os BACKENDES também emitem estágios de DENTRO do initialize()
/// (vkCreateInstance etc.) sem precisar do resto da API do Renderer.
///
/// Contratos:
/// - chamado na MESMA thread de Renderer::create (sem concorrência);
/// - o hook NÃO pode chamar a API de RHI (reentrância proibida);
/// - não pode lançar (função C).

namespace eng::rhi {

namespace rhi_stage {
inline constexpr char BackendSelect[] = "RHI_BACKEND_SELECT";
inline constexpr char Instance[] = "RHI_INSTANCE";
inline constexpr char Device[] = "RHI_DEVICE";
inline constexpr char Surface[] = "RHI_SURFACE";
inline constexpr char Swapchain[] = "RHI_SWAPCHAIN";
}  // namespace rhi_stage

/// Callback de progresso instalado pelo HOST (nullptr = sem marks).
using ProgressHook = void (*)(void* userdata, const char* stage,
                              const char* status, const char* detail);

/// Registra (ou limpa com nullptr) o hook de progresso da criação.
void setProgressHook(ProgressHook hook, void* userdata);

/// (uso interno dos backends/Renderer) Emite um estágio pelo hook
/// instalado (no-op sem hook). `detail` pode ser nullptr.
void reportProgress(const char* stage, const char* status,
                     const char* detail) noexcept;

}  // namespace eng::rhi

#endif  // ENG_RHI_PROGRESS_HPP
