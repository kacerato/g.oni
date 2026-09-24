#pragma once

/// eng::scene::SceneEvents — eventos de gameplay tipados.
///
/// Publicados no barramento da cena (`Scene::events()`, eng::events —
/// ADR-022: ordem de inscrição, reentrante na mesma thread, sem alocação
/// no publish). Sistemas PUBLICAM; consumidores (bridge NI-Script do
/// editor, métricas, futuro áudio) INSCREVEM.
///
/// Nomes reservados de handler NI-Script (mesmos eventos no lado script):
///   - "on_hit"       — contato resolvido entre o self e outro corpo
///                      (não-trigger, máscaras com overlap bidirecional).
///   - "on_enter"     — par de trigger começou a sobrepor neste passo.
///   - "on_exit"      — par de trigger deixou de sobrepor neste passo.
///   - "on_visible"   — sprite do self entrou no retângulo da câmera.
///   - "on_invisible" — sprite do self saiu do retângulo da câmera.
///
/// Eventos são ESTADO DE TRANSIÇÃO (não fila): quem perdeu o passo
/// (script sem handler no passo, entidade destruída) não recebe replay —
/// o próximo contato publica de novo. Sem alocadores no hot path: structs
/// triviais copiáveis por valor.

#include "eng/ecs/Ecs.hpp"

namespace eng::scene {

/// Contato físico resolvido (publicado pela física a cada passo).
/// `nx/ny` = normal do contato apontando de `other` para `self` (mundo).
struct HitEvent {
    eng::ecs::Entity self{0xFFFFFFFFu, 0xFFFFFFFFu};
    eng::ecs::Entity other{0xFFFFFFFFu, 0xFFFFFFFFu};
    float nx = 0.f;
    float ny = 0.f;
};

/// Trigger: sobreposição ENTROU (entered) ou SAIU (false) neste passo.
struct TriggerEvent {
    eng::ecs::Entity self{0xFFFFFFFFu, 0xFFFFFFFFu};
    eng::ecs::Entity other{0xFFFFFFFFu, 0xFFFFFFFFu};
    bool entered = true;
};

/// Visibilidade de sprite contra o retângulo da câmera ativa
/// (publicado pelo culling do render — P4.7.0 Bloco 6).
struct VisibilityEvent {
    eng::ecs::Entity entity{0xFFFFFFFFu, 0xFFFFFFFFu};
    bool visible = true;
};

/// Nomes de handler NI-Script reservados para os eventos acima
/// (constantes para o runtime — o parser aceita qualquer IDENT;
/// a SEMÂNTICA destes nomes é fixa a partir do P4.7.0).
inline constexpr const char* kEventHit = "on_hit";
inline constexpr const char* kEventTriggerEnter = "on_enter";
inline constexpr const char* kEventTriggerExit = "on_exit";
inline constexpr const char* kEventVisible = "on_visible";
inline constexpr const char* kEventInvisible = "on_invisible";

} // namespace eng::scene
