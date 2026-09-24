/// Capturas dos exemplos rodando (GLES real, surface headless).
///
/// Oculto por padrão: rode com
///   GONI_CAPTURE_DIR=/tmp/cap eng_editor_tests "[.capture]"
/// para gravar PPMs de cada exemplo em edição e em jogo.

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "eng/editor/EditorDocument.hpp"
#include "eng/editor/EditorHost.hpp"
#include "eng/editor/EditorProtocol.hpp"
#include "eng/editor/Inspector.hpp"
#include "eng/input/Input.hpp"
#include "eng/scene/Name.hpp"

namespace {

bool writePpm(const std::string& path, std::uint32_t w, std::uint32_t h,
              const std::vector<std::uint8_t>& rgba)
{
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    std::fprintf(file, "P6\n%u %u\n255\n", w, h);
    // GL lê de baixo para cima: grava invertido para a imagem ficar em pé.
    for (std::uint32_t row = 0; row < h; ++row) {
        const std::uint8_t* line = rgba.data() + (h - 1 - row) * w * 4u;
        for (std::uint32_t x = 0; x < w; ++x) {
            std::fwrite(line + x * 4u, 1, 3, file);
        }
    }
    std::fclose(file);
    return true;
}

struct Capture {
    std::unique_ptr<eng::editor::EditorHost> host;
    std::unique_ptr<eng::editor::EditorProtocol> proto;
    std::uint32_t w;
    std::uint32_t h;
    std::string dir;

    bool snap(const std::string& name)
    {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 4u);
        auto* renderer = host->viewportRenderer();
        if (renderer == nullptr ||
            !renderer->renderer()->readPixels(w, h, pixels.data()).ok()) {
            return false;
        }
        return writePpm(dir + "/" + name + ".ppm", w, h, pixels);
    }

    void frames(int n, bool tapEvery = false, int period = 24)
    {
        for (int i = 0; i < n; ++i) {
            if (tapEvery && i % period == 0) {
                key(true);
            }
            (void)host->renderFrame(1.f / 60.f);
            if (tapEvery && i % period == 0) {
                key(false);
            }
        }
    }

    double field(eng::ecs::Entity e, const char* comp, const char* path)
    {
        auto v = eng::editor::Inspector::getField(*host->document().sceneInFocus(),
                                                  e, comp, path);
        return v.ok() ? std::atof(v.value().c_str()) : 0.0;
    }

    /// Piloto do Voo: toca quando o pássaro fica abaixo do vão à frente.
    void autopilot(int n)
    {
        for (int i = 0; i < n; ++i) {
            const auto& scene = *host->document().sceneInFocus();
            eng::ecs::Entity bird = eng::scene::kNoEntity;
            double target = 0.3;
            double nearest = 1e9;
            scene.world().each<eng::scene::Name>(
                [&](eng::ecs::Entity e, const eng::scene::Name& name) {
                    if (scene.isTemplated(e)) {
                        return;
                    }
                    if (name.value == "Pássaro") {
                        bird = e;
                    }
                    if (name.value == "Cano") {
                        const double px = field(e, "eng::math::Transform", "position.x");
                        if (px > -2.0 && px < nearest) {
                            nearest = px;
                            target = field(e, "eng::math::Transform", "position.y");
                        }
                    }
                });
            const bool press =
                bird != eng::scene::kNoEntity &&
                field(bird, "eng::math::Transform", "position.y") < target - 0.4 &&
                field(bird, "eng::physics::RigidBody", "velocity.y") < 0.0;
            if (press) {
                key(true);
            }
            (void)host->renderFrame(1.f / 60.f);
            if (press) {
                key(false);
            }
        }
    }

    void key(bool down)
    {
        eng::input::InputEvent e;
        e.device = eng::input::DeviceKind::Keyboard;
        e.key = eng::input::Key::Space;
        e.keyDown = down;
        host->document().runtimeInput().queueEvent(e);
    }
};

}  // namespace

TEST_CASE("captura: exemplos em edição e em jogo", "[.capture]")
{
    const char* dir = std::getenv("GONI_CAPTURE_DIR");
    if (dir == nullptr) {
        SKIP("defina GONI_CAPTURE_DIR");
    }
    std::filesystem::create_directories(dir);
    struct Case {
        const char* id;
        std::uint32_t w;
        std::uint32_t h;
    };
    for (const Case c : {Case{"flappy", 360, 760}, Case{"platformer", 760, 360},
                         Case{"boxes", 360, 760}}) {
        const std::string ws = std::string(".editor-test-ws-capture-") + c.id;
        std::filesystem::remove_all(ws);
        const char* backend = std::getenv("GONI_CAPTURE_BACKEND");
        auto created = eng::editor::EditorHost::create(
            backend != nullptr ? backend : "gles", ws.c_str());
        REQUIRE(created.ok());
        Capture cap{std::unique_ptr<eng::editor::EditorHost>(created.value()),
                    nullptr, c.w, c.h, dir};
        int marker = 0;
        cap.host->surfaceCreated(&marker, eng::rhi::NativeWindowKind::Headless,
                                 c.w, c.h);
        if (cap.host->state() != eng::editor::HostSurfaceState::Available) {
            SKIP("OpenGL ES indisponível");
        }
        cap.proto = std::make_unique<eng::editor::EditorProtocol>(
            cap.host->document(), cap.host.get());
        auto& doc = cap.host->document();
        const std::string req = std::string(R"({"op":"project.new","name":"Cap)") +
                                c.id + R"(","template":")" + c.id + R"("})";
        INFO(cap.proto->call(req));
        REQUIRE(doc.hasProject());
        doc.setGameViewportSize(static_cast<float>(c.w), static_cast<float>(c.h));
        // O projeto exportado (.goni) serve de amostra para o export de APK.
        const std::string goni = std::string(c.id) + ".goni";
        INFO(cap.proto->call(R"({"op":"project.exportZip","path":")" + goni + R"("})"));
        std::filesystem::copy_file(ws + "/" + goni, std::string(dir) + "/" + goni,
                                   std::filesystem::copy_options::overwrite_existing);
        (void)cap.proto->call(R"({"op":"viewport.fit"})");
        cap.frames(2);
        CHECK(cap.snap(std::string(c.id) + "-edicao"));

        doc.setScriptSeed(3);
        REQUIRE(doc.play().ok());
        cap.frames(3);
        CHECK(cap.snap(std::string(c.id) + "-inicio"));
        if (std::string(c.id) == "flappy") {
            cap.key(true);
            cap.frames(1);
            cap.key(false);
            cap.autopilot(60 * 9);
        } else if (std::string(c.id) == "platformer") {
            cap.frames(90);
        } else {
            cap.frames(60 * 4, true, 30);
        }
        CHECK(cap.snap(std::string(c.id) + "-jogando"));
        doc.stop();
        cap.host->surfaceDestroyed();
    }
}

namespace {

/// Linhas (convenção GL: 0 = base) onde a coluna tem a cor pedida.
struct RowSpan {
    int first = -1;
    int last = -1;
};

RowSpan rowsWith(const std::vector<std::uint8_t>& px, std::uint32_t w, std::uint32_t h,
                 std::uint32_t x0, std::uint32_t x1,
                 bool (*match)(const std::uint8_t*))
{
    RowSpan span;
    for (std::uint32_t row = 0; row < h; ++row) {
        for (std::uint32_t x = x0; x < x1; ++x) {
            if (match(px.data() + (static_cast<std::size_t>(row) * w + x) * 4u)) {
                if (span.first < 0) {
                    span.first = static_cast<int>(row);
                }
                span.last = static_cast<int>(row);
                break;
            }
        }
    }
    return span;
}

bool isRed(const std::uint8_t* p) { return p[0] > 180 && p[1] < 80 && p[2] < 80; }
bool isBlue(const std::uint8_t* p) { return p[2] > 180 && p[0] < 80 && p[1] < 80; }
bool isWhite(const std::uint8_t* p) { return p[0] > 200 && p[1] > 200 && p[2] > 200; }

/// Largura (px brancos) de uma linha.
int whiteWidth(const std::vector<std::uint8_t>& px, std::uint32_t w, int row)
{
    int n = 0;
    for (std::uint32_t x = 0; x < w; ++x) {
        n += isWhite(px.data() + (static_cast<std::size_t>(row) * w + x) * 4u) ? 1 : 0;
    }
    return n;
}

}  // namespace

// O quadro precisa sair EM PÉ nos dois backends: um sprite acima do centro
// aparece em cima, e o "L" do texto tem a perna embaixo. No Vulkan o clip
// space tem Y invertido em relação ao GL — sem compensar, o aparelho
// mostrava viewport, jogo e texto de cabeça para baixo.
TEST_CASE("render: imagem em pé em GLES e Vulkan (edição e jogo)",
          "[editor][rhi_hardware][orientation]")
{
    for (const char* backend : {"gles", "vulkan"}) {
        INFO("backend " << backend);
        const std::string ws = std::string(".editor-test-ws-orient-") + backend;
        std::filesystem::remove_all(ws);
        auto created = eng::editor::EditorHost::create(backend, ws.c_str());
        if (!created.ok()) {
            WARN("sem " << backend);
            continue;
        }
        std::unique_ptr<eng::editor::EditorHost> host{created.value()};
        constexpr std::uint32_t w = 200;
        constexpr std::uint32_t h = 320;
        int marker = 0;
        host->surfaceCreated(&marker, eng::rhi::NativeWindowKind::Headless, w, h);
        if (host->state() != eng::editor::HostSurfaceState::Available) {
            WARN(backend << " indisponível neste ambiente");
            continue;
        }
        eng::editor::EditorProtocol proto(host->document(), host.get());
        auto& doc = host->document();
        (void)proto.call(R"({"op":"project.new","name":"Orient","template":"empty"})");
        REQUIRE(doc.hasProject());
        doc.setGameViewportSize(static_cast<float>(w), static_cast<float>(h));

        auto make = [&](const char* tmpl, const char* name, double y) {
            const std::string r = proto.call(std::string(R"({"op":"entity.create","template":")") +
                                             tmpl + R"(","name":")" + name + R"("})");
            const auto at = r.find("\"result\":");
            REQUIRE(at != std::string::npos);
            const std::string id = r.substr(at + 9, r.find_first_of(",}", at + 9) - at - 9);
            (void)proto.call(R"({"op":"transform.set","id":)" + id + R"(,"p":[0,)" +
                             std::to_string(y) + "," + "0]}");
            return id;
        };
        const std::string red = make("sprite", "Cima", 2.5);
        const std::string blue = make("sprite", "Baixo", -2.5);
        const std::string text = make("text", "Letra", 0.0);
        auto set = [&](const std::string& id, const char* comp, const char* path,
                       const char* value) {
            const std::string r = proto.call(R"({"op":"component.set","id":)" + id +
                                             R"(,"component":")" + comp + R"(","path":")" +
                                             path + R"(","value":")" + value + R"("})");
            INFO(r);
            CHECK(r.find("\"ok\":true") != std::string::npos);
        };
        set(red, "eng::editor::SpriteData", "tintR,tintG,tintB", "#FF0000");
        set(blue, "eng::editor::SpriteData", "tintR,tintG,tintB", "#0000FF");
        set(text, "eng::editor::TextData", "text", "L");
        set(text, "eng::editor::TextData", "size", "1.5");
        (void)proto.call(R"({"op":"entity.select","id":0})");

        for (const bool playing : {false, true}) {
            INFO((playing ? "jogo" : "edição"));
            if (playing) {
                REQUIRE(doc.play().ok());
            } else {
                (void)proto.call(R"({"op":"viewport.fit"})");
            }
            for (int i = 0; i < 3; ++i) {
                (void)host->renderFrame(1.f / 60.f);
            }
            std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4u);
            REQUIRE(host->viewportRenderer()->renderer()->readPixels(w, h, px.data()).ok());
            if (const char* dir = std::getenv("GONI_CAPTURE_DIR")) {
                (void)writePpm(std::string(dir) + "/orient-" + backend +
                                   (playing ? "-jogo" : "-edicao") + ".ppm",
                               w, h, px);
            }

            const RowSpan r = rowsWith(px, w, h, w / 2 - 4, w / 2 + 4, &isRed);
            const RowSpan b = rowsWith(px, w, h, w / 2 - 4, w / 2 + 4, &isBlue);
            REQUIRE(r.first >= 0);
            REQUIRE(b.first >= 0);
            CHECK(r.first > b.last);  // vermelho (y = +2,5) acima do azul

            // "L": perna horizontal embaixo = linha branca mais larga na base.
            const RowSpan l = rowsWith(px, w, h, 0, w, &isWhite);
            REQUIRE(l.first >= 0);
            CHECK(whiteWidth(px, w, l.first) > whiteWidth(px, w, l.last) * 2);
            if (playing) {
                doc.stop();
            }
        }
        host->surfaceDestroyed();
        std::filesystem::remove_all(ws);
    }
}
