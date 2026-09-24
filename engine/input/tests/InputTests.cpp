/// Testes de eng::input — estado puro, sem
/// plataforma: eventos sintéticos exercitam o MESMO pipeline que o TU JNI
/// alimenta no Android.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "eng/input/Input.hpp"

namespace {

using namespace eng::input;

InputEvent touchEvent(std::uint32_t id, TouchPhase phase, float x, float y,
                      float pressure = 1.f)
{
    InputEvent event;
    event.device = DeviceKind::Touch;
    event.pointerId = id;
    event.touchPhase = phase;
    event.x = x;
    event.y = y;
    event.pressure = pressure;
    return event;
}

InputEvent keyEvent(Key key, bool down)
{
    InputEvent event;
    event.device = DeviceKind::Keyboard;
    event.key = key;
    event.keyDown = down;
    return event;
}

}  // namespace

// =============================================================================
// Touch
// =============================================================================

TEST_CASE("input: touch down/move/up com identidade por pointer", "[input]")
{
    InputSystem input;
    input.queueEvent(touchEvent(7, TouchPhase::Down, 100.f, 200.f, 0.5f));
    input.update();
    CHECK(input.touch().count() == 1);
    const TouchPoint* point = input.touch().find(7);
    REQUIRE(point != nullptr);
    CHECK(point->position.x == 100.f);
    CHECK(point->position.y == 200.f);
    CHECK(point->pressure == 0.5f);
    CHECK(input.touch().isDown(7));

    input.queueEvent(touchEvent(7, TouchPhase::Move, 130.f, 240.f));
    input.update();
    point = input.touch().find(7);
    REQUIRE(point != nullptr);
    CHECK(point->position.x == 130.f);
    CHECK(point->position.y == 240.f);
    CHECK(point->phase == TouchPhase::Move);

    input.queueEvent(touchEvent(7, TouchPhase::Up, 130.f, 240.f));
    input.update();
    CHECK(input.touch().find(7) == nullptr); // encerrado sai no beginFrame
    CHECK_FALSE(input.touch().isDown(7));
}

TEST_CASE("input: multitouch com 3 dedos concorrentes", "[input]")
{
    InputSystem input;
    input.queueEvent(touchEvent(1, TouchPhase::Down, 10.f, 10.f));
    input.queueEvent(touchEvent(2, TouchPhase::Down, 20.f, 20.f));
    input.queueEvent(touchEvent(3, TouchPhase::Down, 30.f, 30.f));
    input.update();
    CHECK(input.touch().count() == 3);
    CHECK(input.touch().isDown(1));
    CHECK(input.touch().isDown(2));
    CHECK(input.touch().isDown(3));

    // Move independente de UM dedo não afeta os outros.
    input.queueEvent(touchEvent(2, TouchPhase::Move, 25.f, 25.f));
    input.update();
    CHECK(input.touch().count() == 3);
    CHECK(input.touch().find(1)->position.x == 10.f);
    CHECK(input.touch().find(2)->position.x == 25.f);

    input.queueEvent(touchEvent(1, TouchPhase::Up, 10.f, 10.f));
    input.update();
    CHECK(input.touch().count() == 2);
    CHECK(input.touch().isDown(3));
}

TEST_CASE("input: cancel descarta o toque; move órfão é ignorado", "[input]")
{
    InputSystem input;
    input.queueEvent(touchEvent(4, TouchPhase::Down, 5.f, 5.f));
    input.update();
    input.queueEvent(touchEvent(4, TouchPhase::Cancelled, 5.f, 5.f));
    input.update();
    CHECK(input.touch().find(4) == nullptr);

    input.queueEvent(touchEvent(99, TouchPhase::Move, 1.f, 1.f));
    input.update();
    CHECK(input.touch().find(99) == nullptr);
}

TEST_CASE("input: eventos na fila só aplicam no update (janela por frame)",
          "[input]")
{
    InputSystem input;
    input.queueEvent(touchEvent(1, TouchPhase::Down, 1.f, 1.f));
    // ANTES do update o estado não mudou.
    CHECK(input.touch().count() == 0);
    input.update();
    CHECK(input.touch().count() == 1);
    CHECK(input.queuedEvents() == 0);
}

// =============================================================================
// Teclado canônico
// =============================================================================

TEST_CASE("input: teclado pressed/released com janela de um update", "[input]")
{
    InputSystem input;
    input.queueEvent(keyEvent(Key::Space, true));
    input.update();
    CHECK(input.keyDown(Key::Space));
    CHECK(input.keyPressed(Key::Space));
    CHECK_FALSE(input.keyReleased(Key::Space));

    // Mantido no próximo update: pressed SOME (janela de 1 frame).
    input.update();
    CHECK(input.keyDown(Key::Space));
    CHECK_FALSE(input.keyPressed(Key::Space));

    input.queueEvent(keyEvent(Key::Space, false));
    input.update();
    CHECK_FALSE(input.keyDown(Key::Space));
    CHECK(input.keyReleased(Key::Space));
}

TEST_CASE("input: keyFromName round-trip do canônico", "[input]")
{
    CHECK(keyFromName("Space") == Key::Space);
    CHECK(keyFromName("A") == Key::A);
    CHECK(keyFromName("Num3") == Key::Num3);
    CHECK(keyFromName("NaoExiste") == Key::None);
    CHECK(keyName(Key::VolumeUp) == "VolumeUp");
}

// =============================================================================
// Ações
// =============================================================================

TEST_CASE("input: ação por tecla e por zona de toque (combina fontes)",
          "[input]")
{
    InputSystem input;
    ActionBindings bindings;
    ActionSource jump;
    jump.kind = ActionSource::Kind::Key;
    jump.key = Key::Space;
    bindings.bind("jump", jump);

    ActionSource jumpTouch;
    jumpTouch.kind = ActionSource::Kind::TouchZone;
    jumpTouch.zoneX0 = 0.f;
    jumpTouch.zoneY0 = 0.5f;
    jumpTouch.zoneX1 = 0.5f;
    jumpTouch.zoneY1 = 1.f;
    bindings.bind("jump", jumpTouch);
    input.setBindings(std::move(bindings));
    input.setScreenSize(200.f, 100.f); // zonas são frações desta tela

    // Tecla.
    input.queueEvent(keyEvent(Key::Space, true));
    input.update();
    CHECK(input.action("jump").down);
    CHECK(input.action("jump").pressed);
    input.queueEvent(keyEvent(Key::Space, false));
    input.update();
    CHECK(input.action("jump").released);
    CHECK_FALSE(input.action("jump").down);

    // Toque na zona (tela 200x100 → zona = x<100, y>50).
    input.queueEvent(touchEvent(1, TouchPhase::Down, 40.f, 80.f));
    input.update();
    CHECK(input.action("jump").down);
    CHECK(input.action("jump").pressed);

    // Toque FORA da zona não aciona.
    input.queueEvent(touchEvent(1, TouchPhase::Up, 40.f, 80.f));
    input.queueEvent(touchEvent(2, TouchPhase::Down, 150.f, 10.f));
    input.update();
    CHECK_FALSE(input.action("jump").down);
    CHECK(input.action("jump").released);

    // Ação sem fontes: estado zerado, sem crash.
    CHECK_FALSE(input.action("inexistente").down);
}

TEST_CASE("input: bindings de/para JSON (config por asset — §D6)", "[input]")
{
    // JSON construído via fábrica (o teste fica no wrapper puro — o parse
    // de TEXTO é do eng::serial::Json, já testado na FASE 3).
    using eng::serial::JsonValue;
    JsonValue root = JsonValue::array();
    JsonValue attack = JsonValue::object();
    attack.set("name", JsonValue::string("attack"));
    JsonValue sources = JsonValue::array();
    JsonValue keySrc = JsonValue::object();
    keySrc.set("key", JsonValue::string("X"));
    sources.append(std::move(keySrc));
    JsonValue zoneSrc = JsonValue::object();
    JsonValue zone = JsonValue::array();
    zone.append(JsonValue::real(0.5));
    zone.append(JsonValue::real(0.0));
    zone.append(JsonValue::real(1.0));
    zone.append(JsonValue::real(0.5));
    zoneSrc.set("touchZone", std::move(zone));
    sources.append(std::move(zoneSrc));
    attack.set("sources", std::move(sources));
    root.append(std::move(attack));

    auto bindings = ActionBindings::fromJson(root);
    REQUIRE(bindings.ok());
    const auto* sourcesOf = bindings.value().sourcesOf("attack");
    REQUIRE(sourcesOf != nullptr);
    REQUIRE(sourcesOf->size() == 2);
    CHECK((*sourcesOf)[0].key == Key::X);
    CHECK((*sourcesOf)[1].kind == ActionSource::Kind::TouchZone);
    CHECK((*sourcesOf)[1].zoneX0 == 0.5f);

    // Round-trip toJson → fromJson.
    auto reloaded = ActionBindings::fromJson(bindings.value().toJson());
    REQUIRE(reloaded.ok());
    CHECK(reloaded.value().actions() ==
          std::vector<std::string>{"attack"});

    // Erro preciso: tecla desconhecida.
    JsonValue bad = JsonValue::array();
    JsonValue badEntry = JsonValue::object();
    badEntry.set("name", JsonValue::string("x"));
    JsonValue badSources = JsonValue::array();
    JsonValue badKey = JsonValue::object();
    badKey.set("key", JsonValue::string("Wat"));
    badSources.append(std::move(badKey));
    badEntry.set("sources", std::move(badSources));
    bad.append(std::move(badEntry));
    auto failed = ActionBindings::fromJson(bad);
    CHECK(failed.isError());
}

TEST_CASE("input: reset limpa tudo (pause/primeiro frame)", "[input]")
{
    InputSystem input;
    input.queueEvent(touchEvent(1, TouchPhase::Down, 1.f, 1.f));
    input.queueEvent(keyEvent(Key::A, true));
    input.update();
    CHECK(input.touch().count() == 1);
    CHECK(input.keyDown(Key::A));
    input.reset();
    CHECK(input.touch().count() == 0);
    CHECK_FALSE(input.keyDown(Key::A));
    CHECK(input.queuedEvents() == 0);
}

// =============================================================================
// Correções da auditoria final FASES 4–10 (remediação C-7/C-8/C-9)
// =============================================================================

TEST_CASE("input: delta acumula entre updates e zera na janela (C-7)",
          "[input]")
{
    InputSystem input;
    input.setScreenSize(200.f, 100.f);

    input.queueEvent(touchEvent(1, TouchPhase::Down, 10.f, 10.f));
    input.update();
    // Down: delta nasce zero (não há posição anterior).
    CHECK(input.touch().active().at(0).delta.x == 0.f);

    // Dois moves na MESMA janela acumulam o deslocamento total.
    input.queueEvent(touchEvent(1, TouchPhase::Move, 30.f, 10.f));
    input.queueEvent(touchEvent(1, TouchPhase::Move, 45.f, 10.f));
    input.update();
    CHECK(input.touch().active().at(0).delta.x == Catch::Approx(35.f));

    // Próxima janela SEM eventos: delta zera (consumido).
    input.update();
    CHECK(input.touch().active().at(0).delta.x == 0.f);
    CHECK(input.touch().active().at(0).position.x == Catch::Approx(45.f));
}

TEST_CASE("input: frameStamp carimba o update da fase (C-8)", "[input]")
{
    InputSystem input;
    input.setScreenSize(200.f, 100.f);

    input.queueEvent(touchEvent(7, TouchPhase::Down, 10.f, 10.f));
    input.update(); // frame 1: Down carimbado
    CHECK(input.touch().active().at(0).frameStamp == 1u);

    input.update(); // frame 2: sem eventos, carimbo PRESERVA
    CHECK(input.touch().active().at(0).frameStamp == 1u);

    input.queueEvent(touchEvent(7, TouchPhase::Move, 20.f, 10.f));
    input.update(); // frame 3: Move re-carimba
    CHECK(input.touch().active().at(0).frameStamp == 3u);
}

TEST_CASE("input: pressed de zona dispara UMA vez com dedo parado (C-9)",
          "[input]")
{
    InputSystem input;
    ActionBindings bindings;
    ActionSource hold;
    hold.kind = ActionSource::Kind::TouchZone;
    hold.zoneX0 = 0.f;
    hold.zoneY0 = 0.f;
    hold.zoneX1 = 1.f;
    hold.zoneY1 = 1.f;
    bindings.bind("hold", hold);
    input.setBindings(std::move(bindings));
    input.setScreenSize(200.f, 100.f);

    // Down na zona: pressed UMA vez.
    input.queueEvent(touchEvent(1, TouchPhase::Down, 100.f, 50.f));
    input.update();
    CHECK(input.action("hold").down);
    CHECK(input.action("hold").pressed);

    // Dedo MANTIDO sem novos eventos: down continua, pressed NÃO repete
    // (antes: disparava a cada frame — inconsistente com o teclado).
    input.update();
    input.update();
    CHECK(input.action("hold").down);
    CHECK_FALSE(input.action("hold").pressed);

    // Move na zona: continua down (sem pressed).
    input.queueEvent(touchEvent(1, TouchPhase::Move, 110.f, 50.f));
    input.update();
    CHECK(input.action("hold").down);
    CHECK_FALSE(input.action("hold").pressed);

    // Up: released.
    input.queueEvent(touchEvent(1, TouchPhase::Up, 110.f, 50.f));
    input.update();
    CHECK(input.action("hold").released);
    CHECK_FALSE(input.action("hold").down);
}
