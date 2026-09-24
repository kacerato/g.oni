#include "eng/input/Input.hpp"

/// eng::input — implementação. Estado puro, sem plataforma.

#include <algorithm>
#include <cstring>

namespace eng::input {

namespace {

using eng::core::Error;
using eng::core::Result;
using eng::core::StatusCode;
using eng::core::makeUnexpected;

[[nodiscard]] Error inputError(StatusCode code, std::string message)
{
    return Error{code, "input: " + std::move(message)};
}

}  // namespace

// =============================================================================
// TouchState
// =============================================================================

void TouchState::onTouch(std::uint32_t id, TouchPhase phase, float x, float y,
                         float pressure, std::uint64_t frame)
{
    switch (phase) {
    case TouchPhase::Down:
    case TouchPhase::Move: {
        TouchPoint* point = nullptr;
        for (auto& candidate : points_) {
            if (candidate.id == id) {
                point = &candidate;
                break;
            }
        }
        if (point == nullptr) {
            if (phase == TouchPhase::Move) {
                return; // Move de dedo desconhecido: ignorado (defensivo)
            }
            TouchPoint fresh;
            fresh.id = id;
            fresh.position = {x, y};
            fresh.delta = {0.f, 0.f};
            fresh.pressure = pressure;
            fresh.phase = TouchPhase::Down;
            fresh.frameStamp = frame;
            points_.push_back(fresh);
            return;
        }
        // Down duplicado = Move (defensivo; sistemas reenviam).
        // Delta ACUMULA entre updates (bug C-7): beginFrame zera no início
        // do update e os eventos da janela somam o deslocamento.
        point->delta.x += x - point->position.x;
        point->delta.y += y - point->position.y;
        point->position = {x, y};
        point->pressure = pressure;
        point->phase = phase;
        point->frameStamp = frame; // bug C-8: carimbo da fase
        break;
    }
    case TouchPhase::Up:
    case TouchPhase::Cancelled: {
        for (auto& candidate : points_) {
            if (candidate.id == id) {
                candidate.delta.x += x - candidate.position.x;
                candidate.delta.y += y - candidate.position.y;
                candidate.position = {x, y};
                candidate.phase = phase;
                candidate.frameStamp = frame;
                break;
            }
        }
        break;
    }
    }
}

void TouchState::beginFrame()
{
    // Deltas zeram (consumidos no update anterior).
    for (auto& point : points_) {
        point.delta = {0.f, 0.f};
    }
}

void TouchState::purgeEnded()
{
    const auto ended = [](const TouchPoint& point) {
        return point.phase == TouchPhase::Up ||
               point.phase == TouchPhase::Cancelled;
    };
    points_.erase(std::remove_if(points_.begin(), points_.end(), ended),
                  points_.end());
}

const TouchPoint* TouchState::find(std::uint32_t id) const noexcept
{
    for (const auto& point : points_) {
        if (point.id == id) {
            return &point;
        }
    }
    return nullptr;
}

std::vector<TouchPoint> TouchState::active() const
{
    return points_;
}

bool TouchState::isDown(std::uint32_t id) const noexcept
{
    const TouchPoint* point = find(id);
    return point != nullptr && point->phase != TouchPhase::Up &&
           point->phase != TouchPhase::Cancelled;
}

// =============================================================================
// Key canônico
// =============================================================================

namespace {

struct KeyName {
    Key key;
    std::string_view name;
};

constexpr KeyName kKeyNames[] = {
    {Key::A, "A"}, {Key::B, "B"}, {Key::C, "C"}, {Key::D, "D"},
    {Key::E, "E"}, {Key::F, "F"}, {Key::G, "G"}, {Key::H, "H"},
    {Key::I, "I"}, {Key::J, "J"}, {Key::K, "K"}, {Key::L, "L"},
    {Key::M, "M"}, {Key::N, "N"}, {Key::O, "O"}, {Key::P, "P"},
    {Key::Q, "Q"}, {Key::R, "R"}, {Key::S, "S"}, {Key::T, "T"},
    {Key::U, "U"}, {Key::V, "V"}, {Key::W, "W"}, {Key::X, "X"},
    {Key::Y, "Y"}, {Key::Z, "Z"},
    {Key::Num0, "Num0"}, {Key::Num1, "Num1"}, {Key::Num2, "Num2"},
    {Key::Num3, "Num3"}, {Key::Num4, "Num4"}, {Key::Num5, "Num5"},
    {Key::Num6, "Num6"}, {Key::Num7, "Num7"}, {Key::Num8, "Num8"},
    {Key::Num9, "Num9"},
    {Key::Space, "Space"}, {Key::Enter, "Enter"},
    {Key::Backspace, "Backspace"}, {Key::Back, "Back"},
    {Key::Menu, "Menu"},
    {Key::Up, "Up"}, {Key::Down, "Down"}, {Key::Left, "Left"},
    {Key::Right, "Right"},
    {Key::VolumeUp, "VolumeUp"}, {Key::VolumeDown, "VolumeDown"},
    {Key::Plus, "Plus"}, {Key::Minus, "Minus"},
};

}  // namespace

Key keyFromName(std::string_view name)
{
    for (const auto& entry : kKeyNames) {
        if (entry.name == name) {
            return entry.key;
        }
    }
    return Key::None;
}

std::string_view keyName(Key key)
{
    for (const auto& entry : kKeyNames) {
        if (entry.key == key) {
            return entry.name;
        }
    }
    return "None";
}

// =============================================================================
// ActionBindings
// =============================================================================

void ActionBindings::bind(std::string_view action, const ActionSource& source)
{
    map_[std::string(action)].push_back(source);
}

void ActionBindings::clear()
{
    map_.clear();
}

std::vector<std::string> ActionBindings::actions() const
{
    std::vector<std::string> names;
    names.reserve(map_.size());
    for (const auto& [name, sources] : map_) {
        (void)sources;
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

const std::vector<ActionSource>* ActionBindings::sourcesOf(
    std::string_view action) const
{
    const auto it = map_.find(std::string(action));
    return it == map_.end() ? nullptr : &it->second;
}

Result<ActionBindings> ActionBindings::fromJson(
    const eng::serial::JsonValue& value)
{
    using eng::serial::JsonValue;
    if (!value.isArray()) {
        return makeUnexpected(inputError(StatusCode::ParseError,
                                         "bindings: raiz deve ser array"));
    }
    ActionBindings bindings;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const JsonValue entry = value.at(i);
        if (!entry.isObject()) {
            return makeUnexpected(inputError(StatusCode::ParseError,
                                             "binding deve ser objeto"));
        }
        const auto name = entry.find("name");
        if (!name.has_value() || !name->isString() ||
            name->asString().empty()) {
            return makeUnexpected(inputError(StatusCode::ParseError,
                                             "binding sem 'name'"));
        }
        const auto sources = entry.find("sources");
        if (!sources.has_value() || !sources->isArray()) {
            return makeUnexpected(inputError(
                StatusCode::ParseError, "binding '" + name->asString() +
                                            "' sem 'sources'"));
        }
        for (std::size_t j = 0; j < sources->size(); ++j) {
            const JsonValue source = sources->at(j);
            if (!source.isObject()) {
                continue;
            }
            if (const auto key = source.find("key");
                key.has_value() && key->isString()) {
                const Key parsed = keyFromName(key->asString());
                if (parsed == Key::None) {
                    return makeUnexpected(inputError(
                        StatusCode::ParseError,
                        "tecla desconhecida: '" + key->asString() + "'"));
                }
                ActionSource s;
                s.kind = ActionSource::Kind::Key;
                s.key = parsed;
                bindings.bind(name->asString(), s);
                continue;
            }
            if (const auto zone = source.find("touchZone");
                zone.has_value() && zone->isArray() && zone->size() == 4) {
                ActionSource s;
                s.kind = ActionSource::Kind::TouchZone;
                s.zoneX0 = static_cast<float>(zone->at(0).asF64());
                s.zoneY0 = static_cast<float>(zone->at(1).asF64());
                s.zoneX1 = static_cast<float>(zone->at(2).asF64());
                s.zoneY1 = static_cast<float>(zone->at(3).asF64());
                bindings.bind(name->asString(), s);
                continue;
            }
            if (const auto button = source.find("gamepadButton");
                button.has_value() && button->isNumber()) {
                ActionSource s;
                s.kind = ActionSource::Kind::GamepadButton;
                s.gamepadButton = static_cast<std::uint8_t>(
                    button->asU64());
                bindings.bind(name->asString(), s);
            }
        }
    }
    return bindings;
}

eng::serial::JsonValue ActionBindings::toJson() const
{
    using eng::serial::JsonValue;
    JsonValue actions = JsonValue::array();
    for (const auto& [name, sources] : map_) {
        JsonValue entry = JsonValue::object();
        entry.set("name", JsonValue::string(name));
        JsonValue encoded = JsonValue::array();
        for (const auto& source : sources) {
            JsonValue s = JsonValue::object();
            switch (source.kind) {
            case ActionSource::Kind::Key:
                s.set("key", JsonValue::string(keyName(source.key)));
                break;
            case ActionSource::Kind::TouchZone: {
                JsonValue zone = JsonValue::array();
                zone.append(JsonValue::real(source.zoneX0));
                zone.append(JsonValue::real(source.zoneY0));
                zone.append(JsonValue::real(source.zoneX1));
                zone.append(JsonValue::real(source.zoneY1));
                s.set("touchZone", std::move(zone));
                break;
            }
            case ActionSource::Kind::GamepadButton:
                s.set("gamepadButton",
                      JsonValue::integer(source.gamepadButton));
                break;
            }
            encoded.append(std::move(s));
        }
        entry.set("sources", std::move(encoded));
        actions.append(std::move(entry));
    }
    return actions;
}

// =============================================================================
// InputSystem
// =============================================================================

void InputSystem::queueEvent(const InputEvent& event)
{
    queue_.push_back(event);
}

void InputSystem::setScreenSize(float width, float height) noexcept
{
    screenW_ = width > 0.f ? width : 1.f;
    screenH_ = height > 0.f ? height : 1.f;
}

void InputSystem::applyEvent(const InputEvent& event)
{
    switch (event.device) {
    case DeviceKind::Touch:
        touch_.onTouch(event.pointerId, event.touchPhase, event.x, event.y,
                       event.pressure, frame_);
        break;
    case DeviceKind::Keyboard: {
        const auto code = static_cast<std::uint16_t>(event.key);
        if (event.keyDown) {
            if (!keysDown_[code]) {
                keysPressed_[code] = true;
            }
            keysDown_[code] = true;
        } else {
            if (keysDown_[code]) {
                keysReleased_[code] = true;
            }
            keysDown_[code] = false;
        }
        break;
    }
    case DeviceKind::Mouse:
    case DeviceKind::Gamepad:
        // Pipeline declarado: coleta quando a plataforma tiver.
        break;
    }
}

void InputSystem::update()
{
    ++frame_;
    touch_.beginFrame();
    keysPressed_.clear();
    keysReleased_.clear();
    actionStates_.clear();

    for (const auto& event : queue_) {
        applyEvent(event);
    }
    queue_.clear();

    // Ações: combina fontes (§6.3 — uma ação " pressed" se QUALQUER fonte
    // começou; "down" se alguma mantém; "released" se todas soltaram).
    for (const auto& actionName : bindings_.actions()) {
        const auto* sources = bindings_.sourcesOf(actionName);
        if (sources == nullptr) {
            continue;
        }
        ActionState state;
        bool anySource = false;
        for (const auto& source : *sources) {
            switch (source.kind) {
            case ActionSource::Kind::Key: {
                const auto code = static_cast<std::uint16_t>(source.key);
                anySource = true;
                if (keysDown_.count(code) != 0 && keysDown_.at(code)) {
                    state.down = true;
                }
                if (keysPressed_.count(code) != 0) {
                    state.pressed = true;
                }
                if (keysReleased_.count(code) != 0) {
                    state.released = true;
                }
                break;
            }
            case ActionSource::Kind::TouchZone: {
                anySource = true;
                for (const auto& point : touch_.active()) {
                    // Zonas são FRAÇÕES da tela — normaliza pixels.
                    const float nx = screenW_ > 0.f
                                         ? point.position.x / screenW_
                                         : 0.f;
                    const float ny = screenH_ > 0.f
                                         ? point.position.y / screenH_
                                         : 0.f;
                    const bool inside =
                        nx >= source.zoneX0 && ny >= source.zoneY0 &&
                        nx <= source.zoneX1 && ny <= source.zoneY1;
                    if (!inside) {
                        continue;
                    }
                    // Bug C-9 da auditoria final: `pressed` dispara UMA vez
                    // (frameStamp do Down == frame corrente), igual ao
                    // teclado; um dedo parado sem eventos mantém apenas
                    // `down`. Up visível nesta janela necessariamente
                    // ocorreu nela (purgeEnded no fim do update).
                    const bool thisFrame = point.frameStamp == frame_;
                    if (point.phase == TouchPhase::Down) {
                        state.down = true;
                        if (thisFrame) {
                            state.pressed = true;
                        }
                    } else if (point.phase == TouchPhase::Move) {
                        state.down = true;
                    } else {
                        state.released = true;
                    }
                }
                break;
            }
            case ActionSource::Kind::GamepadButton:
                anySource = true; // coleta futura
                break;
            }
        }
        if (anySource) {
            actionStates_[actionName] = state;
        }
    }

    // Toques encerrados NESTA janela saem do estado (ficaram visíveis
    // durante o update para a semântica released).
    touch_.purgeEnded();
}

void InputSystem::reset()
{
    queue_.clear();
    touch_ = TouchState{};
    keysDown_.clear();
    keysPressed_.clear();
    keysReleased_.clear();
    actionStates_.clear();
    frame_ = 0;
}

bool InputSystem::keyDown(Key key) const noexcept
{
    const auto it = keysDown_.find(static_cast<std::uint16_t>(key));
    return it != keysDown_.end() && it->second;
}

bool InputSystem::keyPressed(Key key) const noexcept
{
    return keysPressed_.count(static_cast<std::uint16_t>(key)) != 0;
}

bool InputSystem::keyReleased(Key key) const noexcept
{
    return keysReleased_.count(static_cast<std::uint16_t>(key)) != 0;
}

void InputSystem::setBindings(ActionBindings bindings)
{
    bindings_ = std::move(bindings);
}

ActionState InputSystem::action(std::string_view name) const
{
    const auto it = actionStates_.find(std::string(name));
    return it != actionStates_.end() ? it->second : ActionState{};
}

}  // namespace eng::input
