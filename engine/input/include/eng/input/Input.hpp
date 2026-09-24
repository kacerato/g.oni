#pragma once

/// eng::input — estado de entrada canônico + ações de gameplay
///.
///
/// Camadas (missão §6.1): Android Input → Platform Input → eng::input →
/// Game. Os códigos de dispositivo NUNCA chegam ao gameplay: a fronteira
/// JNI converte MotionEvent/KeyCode para os valores canônicos DAQUI
/// (Key, TouchPhase); gameplay consulta AÇÕES ("jump") ou estado puro
/// (touch/teclas canônicas).
///
/// Dispositivos: Touch é o foco; Keyboard está completo no modelo
/// (eventos/estado); Mouse/Gamepad são DeviceKind declarados com o mesmo
/// pipeline de eventos — a coleta entra quando as plataformas a tiverem.
///
/// Thread: single-threaded (fila + update no tick do jogo — ADR-034). O
/// enfileiramento é projetado para ser chamado pelo mesmo fio do runtime.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "eng/core/Result.hpp"
#include "eng/math/Vec2.hpp"
#include "eng/serial/JsonValue.hpp"

namespace eng::input {

// =============================================================================
// Dispositivos
// =============================================================================

enum class DeviceKind : std::uint8_t {
    Touch,
    Keyboard,
    Mouse,
    Gamepad,
};

/// Fases do toque.
enum class TouchPhase : std::uint8_t {
    Down,     ///< dedo pousou
    Move,     ///< dedo moveu
    Up,       ///< dedo levantou
    Cancelled ///< sistema cancelou (ex.: roubo do foco)
};

/// Um ponto de toque VIVO.
struct TouchPoint {
    std::uint32_t id{0};       ///< pointer ID estável (multitouch)
    eng::math::Vec2 position{0.f, 0.f};  ///< pixels, origem topo-esquerda
    eng::math::Vec2 delta{0.f, 0.f};     ///< desde o último update
    float pressure{1.f};       ///< quando disponível (0..1; default 1)
    TouchPhase phase{TouchPhase::Down};
    std::uint64_t frameStamp{0}; ///< update em que a fase ocorreu
};

/// Estado corrente dos toques (por pointer id).
class TouchState final {
public:
    /// Aplica um evento de toque (chamado pelo InputSystem::update). O
    /// `frame` carimba TouchPoint::frameStamp — update em que a fase
    /// ocorreu (semântica de janela: `pressed`/`released` de zona disparam
    /// UMA vez, no update do evento — bug C-7/C-8/C-9 da auditoria final).
    void onTouch(std::uint32_t id, TouchPhase phase, float x, float y,
                 float pressure, std::uint64_t frame);

    /// Início de update: computa deltas (toques encerrados na janela
    /// ANTERIOR já saíram no fim do update anterior).
    void beginFrame();

    /// Fim do update: remove toques encerrados NESTA janela (visíveis
    /// durante o update p/ semântica released; fora dele, não existem).
    void purgeEnded();

    [[nodiscard]] const TouchPoint* find(std::uint32_t id) const noexcept;
    [[nodiscard]] std::vector<TouchPoint> active() const;
    [[nodiscard]] bool isDown(std::uint32_t id) const noexcept;
    [[nodiscard]] std::size_t count() const noexcept { return points_.size(); }

private:
    std::vector<TouchPoint> points_;
};

// =============================================================================
// Teclado canônico
// =============================================================================

enum class Key : std::uint16_t {
    None = 0,
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    Space, Enter, Backspace, Back, Menu,
    Up, Down, Left, Right,
    VolumeUp, VolumeDown,
    Plus, Minus,
};

/// Evento canônico (fila → update → estado).
struct InputEvent {
    DeviceKind device{DeviceKind::Touch};
    // Touch
    std::uint32_t pointerId{0};
    TouchPhase touchPhase{TouchPhase::Down};
    float x{0.f};
    float y{0.f};
    float pressure{1.f};
    // Keyboard
    Key key{Key::None};
    bool keyDown{false};
};

// =============================================================================
// Ações
// =============================================================================

/// Fonte de uma ação: tecla, botão de toque em zona da tela, eixo futuro.
struct ActionSource {
    enum class Kind : std::uint8_t { Key, TouchZone, GamepadButton };
    Kind kind{Kind::Key};
    Key key{Key::None};
    /// TouchZone: zona da tela em frações [0..1] (resolução-independente).
    float zoneX0{0.f}, zoneY0{0.f}, zoneX1{1.f}, zoneY1{1.f};
    std::uint8_t gamepadButton{0};
};

/// Estado de uma ação numa janela de update.
struct ActionState {
    bool down{false};      ///< mantido
    bool pressed{false};   ///< começou neste update
    bool released{false}; ///< terminou neste update
};

/// Mapa nome → fontes (ex.: "jump" ← Space, J, zona inferior-esquerda).
class ActionBindings final {
public:
    void bind(std::string_view action, const ActionSource& source);
    void clear();
    [[nodiscard]] std::vector<std::string> actions() const;

    /// Config por asset JSON: {"actions":[{"name":"jump",
    ///   "sources":[{"key":"Space"},{"touchZone":[0,0,.5,.5]}]}]}
    [[nodiscard]] static eng::core::Result<ActionBindings> fromJson(
        const eng::serial::JsonValue& value);
    [[nodiscard]] eng::serial::JsonValue toJson() const;

    [[nodiscard]] const std::vector<ActionSource>* sourcesOf(
        std::string_view action) const;

private:
    std::unordered_map<std::string, std::vector<ActionSource>> map_;
};

/// Parse "Space"/"A"/"Num3" → Key (usado no fromJson e no JNI).
[[nodiscard]] Key keyFromName(std::string_view name);
[[nodiscard]] std::string_view keyName(Key key);

// =============================================================================
// InputSystem — fila canônica + estado + ações
// =============================================================================

class InputSystem final {
public:
    /// Enfileira evento (conversão da plataforma já feita — TU JNI).
    void queueEvent(const InputEvent& event);

    /// Tamanho da tela em PIXELS (zonas de toque são FRAÇÕES — §6.6
    /// resolução-independente). O host configura em surfaceChanged.
    void setScreenSize(float width, float height) noexcept;

    /// Um passo do jogo: aplica a fila no estado e nas ações
    /// (pressed/released têm a janela de EXATAMENTE este update).
    void update();

    /// Limpa TUDO (pausa/primeiro frame).
    void reset();

    [[nodiscard]] const TouchState& touch() const noexcept { return touch_; }
    [[nodiscard]] bool keyDown(Key key) const noexcept;
    [[nodiscard]] bool keyPressed(Key key) const noexcept;
    [[nodiscard]] bool keyReleased(Key key) const noexcept;

    // --- ações (gameplay usa isto; §6.3) -------------------------------------

    void setBindings(ActionBindings bindings);
    [[nodiscard]] const ActionBindings& bindings() const noexcept
    {
        return bindings_;
    }
    [[nodiscard]] ActionState action(std::string_view name) const;

    [[nodiscard]] std::size_t queuedEvents() const noexcept
    {
        return queue_.size();
    }

private:
    void applyEvent(const InputEvent& event);

    std::vector<InputEvent> queue_;
    TouchState touch_;
    std::unordered_map<std::uint16_t, bool> keysDown_;
    std::unordered_map<std::uint16_t, bool> keysPressed_;
    std::unordered_map<std::uint16_t, bool> keysReleased_;
    ActionBindings bindings_;
    /// Estado da última janela por ação (cache por update).
    std::unordered_map<std::string, ActionState> actionStates_;
    float screenW_{1.f};
    float screenH_{1.f};
    std::uint64_t frame_{0};
};

}  // namespace eng::input
