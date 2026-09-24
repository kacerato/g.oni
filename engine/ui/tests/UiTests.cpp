/// Testes de eng::ui — hierarquia, layout,
/// hit-testing, eventos de botão, texto pontilhado, escala/resolução.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

#include "eng/ui/Ui.hpp"

namespace {

using namespace eng::ui;

int gClicks = 0;
void countClick(void* /*context*/, std::uint32_t /*widgetId*/)
{
    ++gClicks;
}

float gLastValue = -1.f;
void trackValue(void* /*context*/, std::uint32_t /*widgetId*/, float value)
{
    gLastValue = value;
}

}  // namespace

TEST_CASE("ui: hierarquia cria/anexa/remove em cascata", "[ui]")
{
    UiDocument doc;
    CHECK(doc.root() != nullptr);
    Widget* panel = doc.createWidget(WidgetKind::Panel);
    Widget* button = doc.createWidget(WidgetKind::Button, panel);
    Widget* label = doc.createWidget(WidgetKind::Label, button);
    CHECK(doc.widgetCount() == 4); // raiz + 3

    CHECK(panel->parent() == doc.root());
    CHECK(button->parent() == panel);
    CHECK(label->parent() == button);

    doc.destroyWidget(panel);
    CHECK(doc.widgetCount() == 1); // só a raiz
}

TEST_CASE("ui: layout com rects relativos e âncoras", "[ui]")
{
    UiDocument doc;
    doc.setDesignResolution(1000.f, 500.f);

    // Painel fixo no pai (raiz = 1000×500).
    Widget* panel = doc.createWidget(WidgetKind::Panel);
    panel->setRect({100.f, 50.f, 400.f, 200.f});

    // HUD ancorado ao canto inferior-direito: left=.7, top=.6, right=1,
    // bottom=1 → x=700, y=300, w=300, h=200.
    Widget* hud = doc.createWidget(WidgetKind::Panel);
    UiAnchors anchors;
    anchors.left = 0.7f;
    anchors.top = 0.6f;
    anchors.right = 1.f;
    anchors.bottom = 1.f;
    hud->setAnchors(anchors);
    hud->setRect({5.f, 5.f, 0.f, 0.f});

    // Filho do painel: rect RELATIVO ao painel.
    Widget* child = doc.createWidget(WidgetKind::Image, panel);
    child->setRect({10.f, 10.f, 50.f, 50.f});

    doc.layout();
    CHECK_THAT(panel->resolvedRect().x, Catch::Matchers::WithinAbs(100.f, 1e-4f));
    CHECK_THAT(panel->resolvedRect().w, Catch::Matchers::WithinAbs(400.f, 1e-4f));
    CHECK_THAT(hud->resolvedRect().x, Catch::Matchers::WithinAbs(705.f, 1e-3f));
    CHECK_THAT(hud->resolvedRect().y, Catch::Matchers::WithinAbs(305.f, 1e-3f));
    CHECK_THAT(hud->resolvedRect().w, Catch::Matchers::WithinAbs(300.f, 1e-3f));
    CHECK_THAT(hud->resolvedRect().h, Catch::Matchers::WithinAbs(200.f, 1e-3f));
    // Filho soma o offset do pai.
    CHECK_THAT(child->resolvedRect().x, Catch::Matchers::WithinAbs(110.f, 1e-4f));
    CHECK_THAT(child->resolvedRect().y, Catch::Matchers::WithinAbs(60.f, 1e-4f));
}

TEST_CASE("ui: hit-test top-most com janelas sobrepostas", "[ui]")
{
    UiDocument doc;
    doc.setDesignResolution(400.f, 300.f);
    doc.setScreenSize(400.f, 300.f);

    Widget* back = doc.createWidget(WidgetKind::Panel);
    back->setRect({0.f, 0.f, 400.f, 300.f});
    Widget* front = doc.createWidget(WidgetKind::Button, back);
    front->setRect({100.f, 100.f, 100.f, 60.f});
    doc.layout();

    // Dentro dos dois → o filho (top-most).
    Widget* hit = doc.hitTest(150.f, 130.f);
    REQUIRE(hit == front);
    // Só no fundo.
    CHECK(doc.hitTest(10.f, 10.f) == back);
    // Fora de tudo.
    CHECK(doc.hitTest(-5.f, -5.f) == nullptr);

    // Widget invisível não recebe hit.
    front->setVisible(false);
    CHECK(doc.hitTest(150.f, 130.f) == back);
}

TEST_CASE("ui: botão pressed/click via toques canônicos", "[ui]")
{
    UiDocument doc;
    doc.setDesignResolution(400.f, 300.f);
    doc.setScreenSize(400.f, 300.f);
    Widget* button = doc.createWidget(WidgetKind::Button);
    button->setRect({50.f, 50.f, 120.f, 70.f});
    button->setText("PLAY");
    doc.layout();

    gClicks = 0;
    button->setOnClick(&countClick, nullptr);

    // Down dentro → pressed.
    Widget* hitDown =
        doc.onTouch(100.f, 80.f, eng::input::TouchPhase::Down);
    REQUIRE(hitDown == button);

    // Draw list reflete o estado pressed (cor clareada).
    auto quadsPressed = doc.buildDrawList();
    CHECK(quadsPressed.size() >= 2); // fundo + texto

    // Up dentro → click.
    Widget* hitUp = doc.onTouch(100.f, 80.f, eng::input::TouchPhase::Up);
    CHECK(hitUp == button);
    CHECK(gClicks == 1);

    // Down, Up FORA → sem click (cancel).
    doc.onTouch(100.f, 80.f, eng::input::TouchPhase::Down);
    doc.onTouch(390.f, 290.f, eng::input::TouchPhase::Up);
    CHECK(gClicks == 1);

    // Cancel não dispara click.
    doc.onTouch(100.f, 80.f, eng::input::TouchPhase::Down);
    doc.onTouch(100.f, 80.f, eng::input::TouchPhase::Cancelled);
    CHECK(gClicks == 1);
}

TEST_CASE("ui: slider com arraste e valueChanged", "[ui]")
{
    UiDocument doc;
    doc.setDesignResolution(400.f, 300.f);
    doc.setScreenSize(400.f, 300.f);
    Widget* slider = doc.createWidget(WidgetKind::Slider);
    slider->setRect({50.f, 100.f, 300.f, 20.f});
    doc.layout();

    gLastValue = -1.f;
    slider->setOnValueChange(&trackValue, nullptr);

    doc.onTouch(50.f, 110.f, eng::input::TouchPhase::Down); // início
    CHECK_THAT(slider->value(), Catch::Matchers::WithinAbs(0.f, 1e-4f));

    doc.onTouch(200.f, 110.f, eng::input::TouchPhase::Move); // metade
    CHECK_THAT(slider->value(), Catch::Matchers::WithinAbs(0.5f, 1e-3f));
    CHECK_THAT(gLastValue, Catch::Matchers::WithinAbs(0.5f, 1e-3f));

    // Clamp além do fim do trilho (arraste DIRETO — posição além do hit).
    doc.dragSlider(slider, 400.f);
    CHECK_THAT(slider->value(), Catch::Matchers::WithinAbs(1.f, 1e-4f));
    doc.dragSlider(slider, 10.f);
    CHECK_THAT(slider->value(), Catch::Matchers::WithinAbs(0.f, 1e-4f));
}

TEST_CASE("ui: progress bar preenchimento na draw-list", "[ui]")
{
    UiDocument doc;
    doc.setDesignResolution(400.f, 300.f);
    doc.setScreenSize(400.f, 300.f);
    Widget* bar = doc.createWidget(WidgetKind::ProgressBar);
    bar->setRect({10.f, 10.f, 200.f, 12.f});
    bar->setValue(0.5f);
    doc.layout();

    auto quads = doc.buildDrawList();
    REQUIRE(quads.size() == 2); // trilho + preenchimento
    CHECK(quads[1].rect.w > 99.f); // metade de 200
    CHECK(quads[1].rect.w < 101.f);
    bar->setValue(1.f);
    doc.layout();
    quads = doc.buildDrawList();
    CHECK_THAT(quads[1].rect.w, Catch::Matchers::WithinAbs(200.f, 1e-3f));
}

TEST_CASE("ui: fonte 5×7 gera quads por pixel aceso", "[ui]")
{
    // 'I' tem 13 pixels acesos no glifo clássico.
    auto quadsI = textQuads("I", 0.f, 0.f, 7.f, UiColor{1, 1, 1, 1});
    int onPixels = 0;
    const FontGlyph* glyph = fontGlyph('I');
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            onPixels += glyph->pixels[row][col] ? 1 : 0;
        }
    }
    CHECK(quadsI.size() == static_cast<std::size_t>(onPixels));

    // Espaço não gera quads; avança o cursor.
    CHECK(textQuads(" ", 0.f, 0.f, 7.f, {}).empty());

    // Texto de 3 chars: largura total = 3*6-1 pixels (espaçamento).
    auto quads3 = textQuads("ABC", 0.f, 0.f, 7.f, {});
    float maxX = 0.f;
    for (const auto& quad : quads3) {
        maxX = std::max(maxX, quad.rect.x + quad.rect.w);
    }
    CHECK_THAT(maxX, Catch::Matchers::WithinAbs(17.f, 0.2f)); // 5+1+5+1+5

    // Glifo fora da faixa vira espaço (defensivo).
    CHECK(fontGlyph(static_cast<char>(0x7F))->character == ' ');
}

TEST_CASE("ui: resolução-independente (design→tela→design)", "[ui]")
{
    UiDocument doc;
    doc.setDesignResolution(1280.f, 720.f);

    // Botão no centro do design.
    Widget* button = doc.createWidget(WidgetKind::Button);
    button->setRect({590.f, 330.f, 100.f, 60.f});
    doc.layout();

    // Tela FÍSICA 2× menor: o hit-test converge para o MESMO widget.
    doc.setScreenSize(640.f, 360.f);
    // Ponto central da tela (320,180) → design (640,360) = dentro do botão.
    CHECK(doc.hitTest(320.f, 180.f) == button);
    // Ponto fora (canto) → nada.
    CHECK(doc.hitTest(10.f, 10.f) == nullptr);

    // Escala de DPI adicional (fonte grande): 0.5 → tela efetiva 320×180.
    doc.setScale(0.5f);
    // Centro da tela (320,180) → design (1280? ) — com escala 0.5 a tela
    // efetiva é 320×180 e o design continua 1280×720: centro→centro.
    CHECK(doc.hitTest(160.f, 90.f) == button);
}
