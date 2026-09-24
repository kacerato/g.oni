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
        auto created = eng::editor::EditorHost::create("gles", ws.c_str());
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
