#include "eng/editor/EditorDocument.hpp"

/// EditorDocument — estado + comandos (FASE 8; separação editor×runtime
/// ADR-044: clone por serialização, edição rejeitada em Play).

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <unordered_map>
#include <utility>

#include "eng/animation/Animation.hpp"
#include "eng/audio/Wav.hpp"
#include "eng/log/Macros.hpp"
#include "eng/math/Mat4.hpp"
#include "eng/math/Quat.hpp"
#include "eng/particles/Particles.hpp"
#include "eng/physics/Physics.hpp"
#include "eng/project/ProjectPaths.hpp"
#include "eng/editor/AnimationAssets.hpp"
#include "eng/editor/AudioSource.hpp"
#include "eng/editor/NiScriptComponent.hpp"
#include "eng/editor/SceneClone.hpp"
#include "eng/editor/ProjectZip.hpp"
#include "eng/editor/SpriteData.hpp"
#include "eng/image/Image.hpp"
#include "eng/render/Light2D.hpp"
#include "eng/editor/NiRuntime.hpp"
#include "eng/scene/Name.hpp"
#include "eng/scene/SceneIdentity.hpp"
#include "eng/scene/SceneSerializer.hpp"
#include "eng/serial/Json.hpp"
#include "eng/editor/TextureCache.hpp"

namespace eng::editor {

namespace {

using eng::core::Error;
using eng::core::Result;
using eng::core::StatusCode;
using eng::core::makeUnexpected;

// Histórico/snap (constantes do documento)
constexpr std::size_t kHistoryMax = 40;            ///< passos guardados
constexpr float kHistoryCoalesceSec = 1.2f;        ///< janela de 1 gesto
constexpr float kSnapTranslateStep = 0.5f;         ///< grade (mundo)
constexpr float kSnapRotateDeg = 15.f;             ///< ângulo do snap

ENG_LOG_CATEGORY("editor");

[[nodiscard]] Error documentError(StatusCode code, std::string message)
{
    return Error{code, "EditorDocument: " + std::move(message)};
}

// --- marcadores de persistência ------

/// Nome do projeto default criado numa instalação limpa.
constexpr std::string_view kDefaultProjectName{"MeuJogo"};
/// Registro do último projeto usado (raiz do workspace — oculto).
constexpr std::string_view kLastProjectFile{".goni_last_project"};
/// Registro da ÚLTIMA CENA do projeto (raiz do PROJETO — P4.2/B-A:
/// openProject restaura; viaja DENTRO do zip, então import → open já
/// devolve a cena de onde o autor parou).
constexpr std::string_view kLastSceneFile{".goni_last_scene"};

/// Trim cru de marcador de uma linha (mesma política de .goni_last_project).
[[nodiscard]] std::string trimMarkerLine(std::string text) noexcept
{
    while (!text.empty() &&
           (text.front() == '\n' || text.front() == '\r' ||
            text.front() == ' ')) {
        text.erase(text.begin());
    }
    while (!text.empty() &&
           (text.back() == '\n' || text.back() == '\r' ||
            text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

/// Euler (graus) ↔ Quat — MESMA convenção de Quat::fromEulerAngles
/// (R = RotY(yaw)·RotX(pitch)·RotZ(roll); ADR do math). Round-trip
/// testado em EditorTests.
[[nodiscard]] eng::math::Quat quatFromDegrees(
    const eng::math::Vec3& degrees) noexcept
{
    constexpr float kDegToRad = 3.14159265358979323846f / 180.f;
    return eng::math::Quat::fromEulerAngles(
        degrees.x * kDegToRad, degrees.y * kDegToRad, degrees.z * kDegToRad);
}

[[nodiscard]] eng::math::Vec3 degreesFromQuat(
    const eng::math::Quat& rotation) noexcept
{
    constexpr float kRadToDeg = 180.f / 3.14159265358979323846f;
    const eng::math::Mat4 m = rotation.toMatrix();
    // Extração YXZ da matriz de rotação pura (quaternion):
    //   pitch = asin(-M[1][2]); yaw = atan2(M[0][2], M[2][2]);
    //   roll = atan2(M[1][0], M[1][1]) — at(col,row) do column-major.
    const float sinPitch =
        std::clamp(-m.at(2, 1), -1.f, 1.f);
    const float pitch = std::asin(sinPitch);
    float yaw = 0.f;
    float roll = 0.f;
    if (std::abs(std::cos(pitch)) > 1e-4f) {
        yaw = std::atan2(m.at(2, 0), m.at(2, 2));
        roll = std::atan2(m.at(0, 1), m.at(1, 1));
    } else {
        // Gimbal lock: yaw degenerado — convenção yaw=0 (documentada).
        roll = std::atan2(-m.at(1, 0), m.at(0, 0));
    }
    return eng::math::Vec3{pitch * kRadToDeg, yaw * kRadToDeg,
                           roll * kRadToDeg};
}

/// Path RELATIVO seguro: não absoluto e SEM componente ".." —
/// anti-traversal que funciona tanto com workspace relativo (testes)
/// quanto absoluto (Android filesDir — o root é ESCOLHA do host; o que
/// não pode é escapar DE DENTRO do projeto).
[[nodiscard]] bool isSafeRelativePath(std::string_view path) noexcept
{
    if (path.empty() || path.find('\0') != std::string_view::npos) {
        return false;
    }
    if (path.front() == '/') {
        return false;
    }
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const std::size_t slash = path.find('/', begin);
        const std::string_view component =
            path.substr(begin, slash == std::string_view::npos
                                   ? std::string_view::npos
                                   : slash - begin);
        if (component == ".." || component.empty()) {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        begin = slash + 1;
    }
    return true;
}

/// Controles padrão do jogo: zonas de toque (frações da tela, origem no
/// canto superior esquerdo) + teclado. A UI desenha as mesmas zonas.
[[nodiscard]] eng::input::ActionBindings defaultGameBindings()
{
    using eng::input::ActionSource;
    using eng::input::Key;
    eng::input::ActionBindings b;
    auto zone = [](float x0, float y0, float x1, float y1) {
        ActionSource s;
        s.kind = ActionSource::Kind::TouchZone;
        s.zoneX0 = x0;
        s.zoneY0 = y0;
        s.zoneX1 = x1;
        s.zoneY1 = y1;
        return s;
    };
    auto key = [](Key k) {
        ActionSource s;
        s.kind = ActionSource::Kind::Key;
        s.key = k;
        return s;
    };
    b.bind("left", zone(0.f, 0.6f, 0.2f, 1.f));
    b.bind("left", key(Key::Left));
    b.bind("left", key(Key::A));
    b.bind("right", zone(0.2f, 0.6f, 0.4f, 1.f));
    b.bind("right", key(Key::Right));
    b.bind("right", key(Key::D));
    b.bind("jump", zone(0.75f, 0.6f, 1.f, 1.f));
    b.bind("jump", key(Key::Space));
    b.bind("jump", key(Key::Up));
    // Toque em qualquer ponto da tela (jogos de um botão).
    b.bind("tap", zone(0.f, 0.f, 1.f, 1.f));
    b.bind("tap", key(Key::Space));
    b.bind("tap", key(Key::Enter));
    b.bind("up", key(Key::Up));
    b.bind("up", key(Key::W));
    b.bind("down", key(Key::Down));
    b.bind("down", key(Key::S));
    return b;
}

[[nodiscard]] eng::fs::Path scenesRootOf(
    const eng::project::ProjectFile& project)
{
    const auto& roots = project.config.sceneRoots;
    if (!roots.empty()) {
        return project.paths().resolve(roots.front());
    }
    return project.paths().projectDir() / eng::fs::Path{"scenes"};
}

}  // namespace

// =============================================================================
// Criação
// =============================================================================

Result<std::unique_ptr<EditorDocument>> EditorDocument::create(
    eng::fs::FileSystem& fs, const eng::fs::Path& workspaceRoot)
{
    ensureEditorComponentsRegistered(); // catálogo de gameplay
    auto document = std::unique_ptr<EditorDocument>(new EditorDocument{});
    document->fs_ = &fs;
    document->workspaceRoot_ = workspaceRoot;
    // Contrato do header: "cena vazia PRONTA PARA EDIÇÃO" — o optional é
    // emitido aqui (bug C-3 da auditoria final: acessores como
    // sceneInFocus() faziam &*scene_ vazio → UB latente no estado
    // pré-projeto, mascarado pelo ensureProjectOnFirstRun da Activity).
    document->scene_.emplace();  // Scene não é movível — ADR-025
    document->niRuntime_ = std::make_unique<NiRuntime>(); // FASE 11
    // Bus de PREVIEW isolado do master — a voice do preview
    // nunca se mistura com as vozes de jogo (stop por handle + bus próprio).
    document->previewBus_ = document->audioMixer_.createBus("preview", 1.f);
    return document;
}

EditorDocument::~EditorDocument() = default;

// =============================================================================
// Projeto
// =============================================================================

Result<void> EditorDocument::newProject(std::string_view name)
{
    if (name.empty()) {
        return makeUnexpected(
            documentError(StatusCode::InvalidArgument, "nome vazio"));
    }
    if (name.find('/') != std::string_view::npos || name == "." ||
        name == "..") {
        return makeUnexpected(documentError(
            StatusCode::InvalidArgument,
            "nome de projeto é um nome de pasta (sem '/' ou '..')"));
    }
    const eng::fs::Path root = workspaceRoot_ / eng::fs::Path{std::string(name)};
    auto exists = fs_->exists(root);
    if (exists.isError()) {
        return makeUnexpected(exists.error());
    }
    if (exists.value()) {
        return makeUnexpected(documentError(StatusCode::AlreadyExists,
                                            "projeto '" + std::string(name) +
                                                "' já existe"));
    }

    // Estrutura: assets/<categorias> + scenes + project.goni.json.
    const eng::fs::Path assets = root / eng::fs::Path{"assets"};
    auto made = fs_->mkdirs(assets);
    if (made.isError()) {
        return makeUnexpected(made.error());
    }
    for (const auto& category : AssetBrowser::categories()) {
        auto cat = fs_->mkdirs(assets / eng::fs::Path{category});
        if (cat.isError()) {
            return makeUnexpected(cat.error());
        }
    }
    auto scenes = fs_->mkdirs(root / eng::fs::Path{"scenes"});
    if (scenes.isError()) {
        return makeUnexpected(scenes.error());
    }

    eng::project::ProjectFile file;
    file.config.projectId = eng::project::ProjectId::generate();
    file.config.name = std::string(name);
    file.config.engineVersion = eng::core::Version{0, 1, 0};
    file.config.assetRegistryPath = eng::fs::Path{"assets/asset_registry.json"};
    file.config.sceneRoots = {eng::fs::Path{"scenes"}};
    // Projetos novos já nascem com a tabela de camadas
    // de colisão nomeada ("default" bit 1).
    file.config.collisionLayers = eng::project::defaultCollisionLayers();
    file.filePath = root / eng::fs::Path{"project.goni.json"};
    auto written = file.writeTo(*fs_);
    if (written.isError()) {
        return makeUnexpected(written.error());
    }

    project_ = std::move(file);
    projectDirty_ = false;
    assets_ = std::make_unique<AssetBrowser>(
        *fs_, project_->paths().assetsRoot(),
        project_->paths().resolve(project_->config.assetRegistryPath));
    materialCache_.clear();  // projeto novo: materiais novos
    auto loaded = assets_->loadRegistry();
    if (loaded.isError()) {
        return makeUnexpected(loaded.error());
    }

    auto fresh = newScene();
    if (fresh.isError()) {
        return makeUnexpected(fresh.error());
    }
    // Diagnóstico (P3 §0): operação/projeto/caminho para o logcat.
    ENG_INFO("project-op: criar | projeto='{}' | caminho='{}'", name,
             (workspaceRoot_ / eng::fs::Path{std::string(name)}).str());
    (void)rememberLastUsedProject(name);  // best-effort (logado dentro)
    return {};
}

Result<void> EditorDocument::openProject(const eng::fs::Path& projectRoot)
{
    if (!isSafeRelativePath(projectRoot.str())) {
        return makeUnexpected(documentError(
            StatusCode::InvalidArgument,
            "projectRoot deve ser relativo ao workspace (sem ..)"));
    }
    // Simétrico a newProject: o caminho é resolvido CONTRA o workspace
    // (nunca CWD — no Android o CWD não é o workspace). Bug pego pelo
    // teste de segunda execução (dirs persistem entre runs).
    const eng::fs::Path root = workspaceRoot_ / projectRoot;
    const eng::fs::Path file = root / eng::fs::Path{"project.goni.json"};
    auto opened = eng::project::ProjectFile::readFrom(*fs_, file);
    if (opened.isError()) {
        return makeUnexpected(opened.error());
    }

    project_ = std::move(opened.value());
    projectDirty_ = false;
    materialCache_.clear();  // projeto aberto: materiais do anterior não valem
    assets_ = std::make_unique<AssetBrowser>(
        *fs_, project_->paths().assetsRoot(),
        project_->paths().resolve(project_->config.assetRegistryPath));
    auto loaded = assets_->loadRegistry();
    if (loaded.isError()) {
        return makeUnexpected(loaded.error());
    }

    // O projeto
    // abria e a cena ficava VAZIA (newScene) — o marker de última cena
    // não existia e a Activity nem loadScene chamava. O restore é AQUI,
    // no documento, testável no Linux: o marker (.goni_last_scene, na
    // raiz do projeto) aponta a cena; falha de load é ERRO EXPLÍCITO —
    // nunca silêncio, nunca "projeto novo" vazio (regra da missão).
    std::string lastScene;
    auto markerText = fs_->readAllText(
        project_->paths().projectDir() / eng::fs::Path{kLastSceneFile});
    if (!markerText.isError()) {
        lastScene = trimMarkerLine(std::move(markerText.value()));
    }
    auto fresh = newScene();
    if (fresh.isError()) {
        return makeUnexpected(fresh.error());
    }
    if (!lastScene.empty()) {
        auto restored = loadScene(lastScene);
        if (restored.isError()) {
            ENG_ERROR("project-op: restaurar última cena '{}' falhou: {}",
                      lastScene, restored.error().message);
            return makeUnexpected(documentError(
                restored.error().code,
                "cena anterior não carregou ('" + lastScene + "'): " +
                    restored.error().message));
        }
        ENG_INFO("project-op: última cena restaurada '{}' ({} entidades)",
                 lastScene, scene_->nodeCount());
    } else {
        ENG_INFO("project-op: sem última cena registrada — cena nova vazia");
    }
    // Diagnóstico (P3 §0): operação/projeto/caminho/estado para o logcat.
    ENG_INFO(
        "project-op: abrir | projeto='{}' | caminho='{}' | doc.hasProject={}",
        project_->config.name, file.str(), project_.has_value());
    // Registra o NOME DA PASTA (não config.name — settings renomeia o
    // config mas não a pasta; o restore precisa do nome que EXISTE no
    // disco para listar/abrir).
    (void)rememberLastUsedProject(projectRoot.filename().str());
    return {};
}

// =============================================================================
// Startup (bug Android "AlreadyExists" — P3 §0)
// =============================================================================

Result<std::vector<std::string>> EditorDocument::listProjects() const
{
    auto entries = fs_->list(workspaceRoot_, false);
    if (entries.isError()) {
        // Workspace ausente = instalação limpa SEM projetos (não é erro
        // de I/O — o host cria o diretório no primeiro uso).
        if (entries.error().code == StatusCode::NotFound) {
            return std::vector<std::string>{};
        }
        return makeUnexpected(entries.error());
    }
    std::vector<std::string> names{};
    for (const auto& entry : entries.value()) {
        if (!entry.isDirectory) {
            continue;
        }
        const std::string name{entry.path.filename().str()};
        // Ocultos (staging SAF ".import_tmp", marcadores internos) não
        // são projetos — mesmo filtro do seletor de projetos da Activity.
        if (name.empty() || name.front() == '.') {
            continue;
        }
        auto marker = fs_->exists(
            entry.path / eng::fs::Path{"project.goni.json"});
        if (marker.isError() || !marker.value()) {
            continue;  // diretório comum (lixo/não-projeto): ignora
        }
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());  // determinístico
    return names;
}

std::string EditorDocument::lastUsedProject() const
{
    auto text =
        fs_->readAllText(workspaceRoot_ / eng::fs::Path{kLastProjectFile});
    if (text.isError()) {
        return {};
    }
    // Sem newline/espaço — o registro é uma linha crua; trim defensivo.
    std::string_view name{text.value()};
    while (!name.empty() &&
           (name.front() == '\n' || name.front() == '\r' ||
            name.front() == ' ')) {
        name.remove_prefix(1);
    }
    while (!name.empty() &&
           (name.back() == '\n' || name.back() == '\r' ||
            name.back() == ' ')) {
        name.remove_suffix(1);
    }
    return std::string{name};
}

Result<void> EditorDocument::rememberLastUsedProject(std::string_view name)
{
    // Best-effort por DESIGN: falhar em lembrar não pode derrubar a
    // operação de projeto (o fallback é abrir o default/primeiro).
    const auto made = fs_->mkdirs(workspaceRoot_);
    if (made.isError()) {
        ENG_WARN("startup: não criou raiz do workspace p/ registro: {}",
                 made.error().message);
        return makeUnexpected(made.error());
    }
    auto written = fs_->writeAllText(
        workspaceRoot_ / eng::fs::Path{kLastProjectFile}, name);
    if (written.isError()) {
        ENG_WARN("startup: falha ao registrar último projeto: {}",
                 written.error().message);
        return makeUnexpected(written.error());
    }
    return {};
}

Result<std::string> EditorDocument::ensureStartupProject()
{
    // Caso 1: projeto JÁ em memória (reentrada na mesma sessão) — no-op.
    if (hasProject()) {
        ENG_INFO("startup: projeto já em memória ('{}') — no-op",
                 project_->config.name);
        return project_->config.name;
    }
    auto listed = listProjects();
    if (listed.isError()) {
        ENG_ERROR("startup: falha ao listar workspace: {}",
                  listed.error().message);
        return makeUnexpected(listed.error());
    }
    const auto& projects = listed.value();

    // Caso 2: workspace vazio (instalação limpa) — cria o default.
    if (projects.empty()) {
        ENG_INFO("startup: workspace sem projetos — criando default '{}'",
                 kDefaultProjectName);
        auto created = newProject(kDefaultProjectName);
        if (created.isError()) {
            ENG_ERROR("startup: criação do default falhou: {}",
                      created.error().message);
            return makeUnexpected(created.error());
        }
        return std::string{kDefaultProjectName};
    }

    // Caso 3: projetos existem — ABRE (nunca cria sobre existente).
    // Preferência: último usado → default → primeiro (alfabético).
    std::string last = lastUsedProject();
    if (std::find(projects.begin(), projects.end(), last) == projects.end()) {
        last.clear();  // registro ausente/stale: não vale
    }
    std::string chosen{last};
    if (chosen.empty() &&
        std::find(projects.begin(), projects.end(),
                  std::string{kDefaultProjectName}) != projects.end()) {
        chosen = std::string{kDefaultProjectName};
    }
    if (chosen.empty()) {
        chosen = projects.front();
    }
    ENG_INFO(
        "startup: {} projeto(s) no workspace — abrindo '{}' (último usado: "
        "'{}')",
        projects.size(), chosen, last.empty() ? "-" : last);
    auto opened = openProject(eng::fs::Path{chosen});
    if (opened.isError()) {
        ENG_ERROR("startup: falha ao abrir '{}': {}", chosen,
                  opened.error().message);
        return makeUnexpected(opened.error());
    }
    return chosen;
}

Result<void> EditorDocument::saveProject()
{
    if (!hasProject()) {
        return makeUnexpected(
            documentError(StatusCode::InvalidState, "sem projeto aberto"));
    }
    if (mode_ == Mode::Play) {
        return makeUnexpected(documentError(StatusCode::InvalidState,
                                             "projeto é somente-leitura em Play"));
    }
    auto written = project_->writeTo(*fs_);
    if (written.isError()) {
        return makeUnexpected(written.error());
    }
    // "Salvar projeto" é salvamento COMPLETO — a cena ATUAL
    // vai junto (device round 1: o autor salvava, recarregava e a cena
    // sumia — só o project.goni.json era escrito). Cena nunca salva →
    // default "main.json" (sem diálogo extra; mesmo default do menu Cena).
    const std::string scenePath = currentScenePath_.empty()
                                      ? std::string{"main.json"}
                                      : currentScenePath_;
    auto savedScene = saveScene(scenePath);
    if (savedScene.isError()) {
        ENG_ERROR("project-op: salvar cena '{}' junto do projeto falhou: {}",
                  scenePath, savedScene.error().message);
        return makeUnexpected(savedScene.error());
    }
    ENG_INFO("project-op: salvar | projeto='{}' | cena='{}' ({} entidades)",
             project_->config.name, scenePath, scene_->nodeCount());
    projectDirty_ = false;
    return {};
}

std::string EditorDocument::projectName() const
{
    return hasProject() ? project_->config.name : std::string{};
}

Result<void> EditorDocument::setProjectName(std::string_view name)
{
    if (!hasProject()) {
        return makeUnexpected(
            documentError(StatusCode::InvalidState, "sem projeto aberto"));
    }
    if (name.empty()) {
        return makeUnexpected(
            documentError(StatusCode::InvalidArgument, "nome vazio"));
    }
    if (mode_ == Mode::Play) {
        return makeUnexpected(documentError(StatusCode::InvalidState,
                                             "projeto é somente-leitura em Play"));
    }
    project_->config.name = std::string(name);
    projectDirty_ = true;
    return {};
}

// --- P4.6: camadas de colisão nomeadas (project settings) -------

std::vector<EditorDocument::CollisionLayerInfo>
EditorDocument::collisionLayers() const
{
    std::vector<CollisionLayerInfo> out;
    if (!hasProject()) {
        return out;
    }
    out.reserve(project_->config.collisionLayers.size());
    for (const eng::project::CollisionLayerName& layer :
         project_->config.collisionLayers) {
        out.push_back(CollisionLayerInfo{layer.name, layer.bit});
    }
    return out;
}

Result<void> EditorDocument::setCollisionLayerName(std::uint32_t bit,
                                                   std::string_view name)
{
    if (!hasProject()) {
        return makeUnexpected(
            documentError(StatusCode::InvalidState, "sem projeto aberto"));
    }
    if (mode_ == Mode::Play) {
        return makeUnexpected(documentError(StatusCode::InvalidState,
                                             "projeto é somente-leitura em Play"));
    }
    if (name.empty()) {
        return makeUnexpected(
            documentError(StatusCode::InvalidArgument, "nome vazio"));
    }
    bool found = false;
    for (eng::project::CollisionLayerName& layer :
         project_->config.collisionLayers) {
        if (layer.bit == bit) {
            found = true;
            continue;
        }
        if (layer.name == name) {
            return makeUnexpected(documentError(
                StatusCode::AlreadyExists,
                "camada de colisão '" + std::string(name) + "' já existe"));
        }
    }
    if (!found) {
        return makeUnexpected(documentError(
            StatusCode::NotFound, "bit não tem camada nomeada na tabela"));
    }
    for (eng::project::CollisionLayerName& layer :
         project_->config.collisionLayers) {
        if (layer.bit == bit) {
            layer.name = std::string(name);
        }
    }
    projectDirty_ = true;
    return {};
}

Result<std::uint32_t> EditorDocument::addCollisionLayer(
    std::string_view name)
{
    if (!hasProject()) {
        return makeUnexpected(
            documentError(StatusCode::InvalidState, "sem projeto aberto"));
    }
    if (mode_ == Mode::Play) {
        return makeUnexpected(documentError(StatusCode::InvalidState,
                                             "projeto é somente-leitura em Play"));
    }
    if (name.empty()) {
        return makeUnexpected(
            documentError(StatusCode::InvalidArgument, "nome vazio"));
    }
    for (const eng::project::CollisionLayerName& layer :
         project_->config.collisionLayers) {
        if (layer.name == name) {
            return makeUnexpected(documentError(
                StatusCode::AlreadyExists,
                "camada de colisão '" + std::string(name) + "' já existe"));
        }
    }
    // Menor bit livre (1..2^31) — 31 camadas nomeáveis no u32.
    std::uint32_t freeBit = 0u;
    for (std::uint32_t candidate = 1u; candidate != 0u;
         candidate <<= 1u) {
        bool used = false;
        for (const eng::project::CollisionLayerName& layer :
             project_->config.collisionLayers) {
            if (layer.bit == candidate) {
                used = true;
                break;
            }
        }
        if (!used) {
            freeBit = candidate;
            break;
        }
    }
    if (freeBit == 0u) {
        return makeUnexpected(documentError(
            StatusCode::InvalidState, "sem bits livres (32 camadas)"));
    }
    project_->config.collisionLayers.push_back(
        eng::project::CollisionLayerName{std::string(name), freeBit});
    projectDirty_ = true;
    return freeBit;
}

Result<void> EditorDocument::setGameConfig(
    const eng::project::GameConfig& game)
{
    if (!hasProject()) {
        return makeUnexpected(
            documentError(StatusCode::InvalidState, "sem projeto aberto"));
    }
    if (mode_ == Mode::Play) {
        return makeUnexpected(documentError(StatusCode::InvalidState,
                                             "projeto é somente-leitura em Play"));
    }
    for (const float c : {game.backgroundR, game.backgroundG, game.backgroundB}) {
        if (!std::isfinite(c) || c < 0.f || c > 1.f) {
            return makeUnexpected(documentError(
                StatusCode::InvalidArgument, "cor de fundo fora de 0..1"));
        }
    }
    if (game.orientation != "portrait" && game.orientation != "landscape" &&
        game.orientation != "auto") {
        return makeUnexpected(documentError(StatusCode::InvalidArgument,
                                            "orientação inválida"));
    }
    if (game.controls != "platformer" && game.controls != "tap" &&
        game.controls != "none") {
        return makeUnexpected(documentError(StatusCode::InvalidArgument,
                                            "controles inválidos"));
    }
    project_->config.game = game;
    projectDirty_ = true;
    return {};
}

Result<void> EditorDocument::setGridConfig(
    const eng::project::GridConfig& grid)
{
    if (!hasProject()) {
        return makeUnexpected(
            documentError(StatusCode::InvalidState, "sem projeto aberto"));
    }
    if (mode_ == Mode::Play) {
        return makeUnexpected(documentError(StatusCode::InvalidState,
                                             "projeto é somente-leitura em Play"));
    }
    if (!std::isfinite(grid.cell) || grid.cell <= 0.f ||
        grid.cell > 4096.f) {
        return makeUnexpected(documentError(
            StatusCode::InvalidArgument, "grid.cell inválido (0..4096]"));
    }
    if (grid.majorEvery < 2 || grid.majorEvery > 1024) {
        return makeUnexpected(documentError(
            StatusCode::InvalidArgument, "grid.majorEvery inválido (2..1024)"));
    }
    const float colors[] = {grid.minorR, grid.minorG, grid.minorB,
                            grid.majorR, grid.majorG, grid.majorB};
    for (const float c : colors) {
        if (!std::isfinite(c) || c < 0.f || c > 1.f) {
            return makeUnexpected(documentError(
                StatusCode::InvalidArgument, "cor da grade fora de [0,1]"));
        }
    }
    project_->config.grid = grid;
    projectDirty_ = true;
    return {};
}

eng::fs::Path EditorDocument::projectRoot() const
{
    return hasProject() ? project_->paths().projectDir()
                      : eng::fs::Path{};
}

// =============================================================================
// Import de assets com validação + zip do projeto
// =============================================================================

Result<std::string> EditorDocument::importAsset(std::string_view tempRelPath,
                                                std::string_view category,
                                                std::string_view name)
{
    if (assets_ == nullptr) {
        return makeUnexpected(
            documentError(StatusCode::InvalidState, "sem projeto aberto"));
    }
    std::string finalName;
    auto imported = assets_->import(tempRelPath, category, name, &finalName);
    if (imported.isError()) {
        return makeUnexpected(imported.error());
    }
    // VALIDAÇÃO DE CONTEÚDO NO IMPORT — antes vivia no JNI
    // e SÓ para texturas: áudio aceitava qualquer bytes e o erro estourava
    // DEPOIS, no preview ("wav: não é RIFF/WAVE"), sem orientar. Agora o
    // contrato vive no documento (testável no Linux; o JNI só delega).
    // Falha → remove o arquivo (não deixa lixo catalogado no projeto).
    if (category == "textures") {
        auto bytes = assets_->read(category, finalName);
        if (bytes.isError()) {
            return makeUnexpected(bytes.error());
        }
        auto decoded = eng::image::decode(std::span{bytes.value()});
        if (decoded.isError()) {
            (void)assets_->remove(category, finalName);
            return makeUnexpected(decoded.error());
        }
    } else if (category == "audio") {
        auto bytes = assets_->read(category, finalName);
        if (bytes.isError()) {
            return makeUnexpected(bytes.error());
        }
        auto parsed = eng::audio::Wav::parse(std::span{bytes.value()});
        if (parsed.isError()) {
            (void)assets_->remove(category, finalName);
            return makeUnexpected(documentError(
                StatusCode::ParseError,
                "áudio recusado no import — apenas WAV PCM suportado por "
                "agora (OGG/MP3 é fase futura): " +
                    parsed.error().message));
        }
    }
    return finalName;
}

Result<void> EditorDocument::exportProjectZip(std::string_view zipRelPath)
{
    if (!hasProject()) {
        return makeUnexpected(
            documentError(StatusCode::InvalidState, "sem projeto aberto"));
    }
    if (!isSafeRelativePath(zipRelPath)) {
        return makeUnexpected(documentError(
            StatusCode::InvalidArgument,
            "path do zip deve ser relativo ao workspace (sem ..)"));
    }
    // O wrapper do zip é o NOME DA PASTA real no disco —
    // settings renomeia config.name sem renomear a pasta; usar config
    // apontava export para pasta inexistente ("Pasta do projeto não
    // encontrada") e import derivava lixo da última entrada.
    const std::string folder =
        project_->paths().projectDir().filename().str();
    if (folder.empty()) {
        return makeUnexpected(documentError(StatusCode::Internal,
                                           "pasta do projeto sem nome"));
    }
    return buildProjectZip(*fs_, project_->paths().projectDir(),
                           eng::fs::Path{std::string(zipRelPath)}, folder);
}

Result<std::string> EditorDocument::importProjectZip(
    std::string_view zipRelPath, std::string_view preferredName)
{
    if (!isSafeRelativePath(zipRelPath)) {
        return makeUnexpected(documentError(
            StatusCode::InvalidArgument,
            "path do zip deve ser relativo ao workspace (sem ..)"));
    }
    // NÃO abre o projeto aqui: o chamador decide (openProject explícito —
    // erro de open volta para a UI como toast, nunca cena vazia silenciosa).
    return extractProjectZip(*fs_, eng::fs::Path{std::string(zipRelPath)},
                             workspaceRoot_, preferredName);
}

// =============================================================================
// Cena
// =============================================================================

Result<void> EditorDocument::newScene()
{
    if (mode_ == Mode::Play) {
        return makeUnexpected(documentError(
            StatusCode::InvalidState, "cena é somente-leitura em Play"));
    }
    clearHistory();  // nova cena = novo documento de undo
    scene_.emplace(); // constrói in place (Scene não é movível — ADR-025)
    selection_.reset();
    sceneDirty_ = false;
    // Cena nova = config de ticks de fábrica (timestep 1/60).
    physicsAccumulator_.setFixedDt(1.f / 60.f);
    // Cena nova = nada a restaurar no próximo open — o path
    // corrente e o marker morrem JUNTOS (best-effort no marker).
    currentScenePath_.clear();
    if (hasProject()) {
        (void)fs_->remove(project_->paths().projectDir() /
                          eng::fs::Path{kLastSceneFile});
    }
    return {};
}

Result<void> EditorDocument::saveScene(std::string_view scenePath)
{
    if (!hasProject()) {
        return makeUnexpected(
            documentError(StatusCode::InvalidState, "sem projeto aberto"));
    }
    if (mode_ == Mode::Play) {
        return makeUnexpected(documentError(
            StatusCode::InvalidState, "cena é somente-leitura em Play"));
    }
    if (!isSafeRelativePath(scenePath)) {
        return makeUnexpected(documentError(
            StatusCode::InvalidArgument,
            "path de cena deve ser RELATIVO ao projeto (§8.1, sem ..)"));
    }
    const eng::fs::Path path{std::string(scenePath)};
    auto text = eng::scene::SceneSerializer::save(*scene_);
    if (text.isError()) {
        return makeUnexpected(text.error());
    }
    // Timestep da física viaja na CENA (chave aditiva do
    // documento — o serializer ignora chaves desconhecidas, arquivos antigos
    // carregam com 1/60). Parse do próprio output: falhar aqui é bug grave
    // (serializer emitiu JSON inválido) — erro explícito, sem silêncio.
    {
        auto root = eng::serial::parseJson(text.value());
        if (root.isError()) {
            return makeUnexpected(root.error());
        }
        root.value().set(
            "physicsFixedDt",
            eng::serial::JsonValue::real(
                static_cast<double>(physicsAccumulator_.fixedDt())));
        // P4.7.0 B5: varredura do kinematic persistida (default ON).
        root.value().set("physicsKinematicSweep",
                         eng::serial::JsonValue::boolean(kinematicSweep_));
        // P4.7.0 B6: logic LOD (default OFF — opt-in do autor).
        root.value().set("logicLodEnabled",
                         eng::serial::JsonValue::boolean(logicLodEnabled_));
        text = eng::serial::dumpJson(root.value());
    }
    const eng::fs::Path full = scenesRootOf(*project_) / path;
    auto made = fs_->mkdirs(full.parent());
    if (made.isError()) {
        return makeUnexpected(made.error());
    }
    auto written = fs_->writeAllText(full, text.value());
    if (written.isError()) {
        return makeUnexpected(written.error());
    }
    // Path corrente + marker de última cena (o openProject
    // restaura de cá). Marker na raiz do PROJETO: viaja no zip (import →
    // open devolve a cena de onde o autor parou).
    currentScenePath_ = std::string(scenePath);
    auto marker = fs_->writeAllText(
        project_->paths().projectDir() / eng::fs::Path{kLastSceneFile},
        currentScenePath_);
    if (marker.isError()) {
        ENG_WARN("scene-op: falha ao registrar última cena: {}",
                 marker.error().message);
    }
    sceneDirty_ = false;
    return {};
}

namespace {

// --- P4.6 (Blocos 1/2): migração ADITIVA de cenas pré-P4.6 -----------------
//
// O decode refletido é ESTRITO com campo ausente (ADR-031 — chaves
// desconhecidas são ignoradas, mas campos novos REFLETIDOS obrigatórios
// quebrariam o load de cenas antigas). A migração injeta os defaults
// ANTES do SceneSerializer::load, preservando a semântica pré-P4.6:
//
//   eng::physics::RigidBody sem "bodyType"
//       → mass <= 0 ? "Static" : "DynamicLite"  (massa 0 ERA o estático)
//
// Blocos seguintes estendem migrateComponentDataP46 (Light2D/SpriteData —
// Bloco 2). Idempotente: cenas novas já trazem os campos (encode escreve
// tudo) → nenhum campo injetado → nenhum re-dump (fast path).

/// Migra `data` (cópia mutável do componente `type`). true = alterado.
[[nodiscard]] bool migrateComponentDataP46(const std::string& type,
                                           eng::serial::JsonValue& data)
{
    using eng::serial::JsonValue;
    if (type == "eng::physics::RigidBody" &&
        !data.find("bodyType").has_value()) {
        bool staticBody = false;
        if (const auto mass = data.find("mass");
            mass.has_value() && mass->isNumber()) {
            staticBody = mass->asF64() <= 0.0;
        }
        data.set("bodyType", JsonValue::string(
                                 staticBody ? "Static" : "DynamicLite"));
        return true;
    }
    return false;
}

// Migration aditiva da câmera: campos novos
// (rotationDeg/followName/deadzone*/smoothingTime/limits*/limit*) INJETADOS
// com os defaults pré-P4.7 quando ausentes. O decodeStruct é ESTRITO
// (campo refletido ausente = ParseError) — cenas salvas por versões
// anteriores NÃO carregariam sem este pre-pass. Idempotente por construção
// (só injeta quando a chave NÃO existe; cenas novas saem do save completo).
[[nodiscard]] bool migrateComponentDataP47(const std::string& type,
                                           eng::serial::JsonValue& data)
{
    using eng::serial::JsonValue;
    if (type == "eng::editor::NiScriptComponent") {
        // Opt-out do logic LOD (additive — ausente =
        // false: o script PARTICIPA do LOD quando o setting liga).
        bool changed = false;
        if (!data.find("lodOptOut").has_value()) {
            data.set("lodOptOut", JsonValue::boolean(false));
            changed = true;
        }
        // Script ligado a arquivo (ausente = fonte embutida, como antes).
        if (!data.find("scriptAsset").has_value()) {
            data.set("scriptAsset", JsonValue::string(""));
            changed = true;
        }
        return changed;
    }
    if (type != "eng::tick::CameraData") {
        return false;
    }
    bool changed = false;
    const auto injectNumber = [&](const char* key, double value) {
        if (!data.find(key).has_value()) {
            data.set(key, JsonValue::real(value));
            changed = true;
        }
    };
    const auto injectBool = [&](const char* key, bool value) {
        if (!data.find(key).has_value()) {
            data.set(key, JsonValue::boolean(value));
            changed = true;
        }
    };
    injectNumber("rotationDeg", 0.0);
    if (!data.find("followName").has_value()) {
        data.set("followName", JsonValue::string(""));
        changed = true;
    }
    injectNumber("deadzoneW", 0.0);
    injectNumber("deadzoneH", 0.0);
    injectNumber("smoothingTime", 0.0);
    injectBool("limitsEnabled", false);
    injectNumber("limitMinX", 0.0);
    injectNumber("limitMinY", 0.0);
    injectNumber("limitMaxX", 0.0);
    injectNumber("limitMaxY", 0.0);
    injectNumber("viewHeight", 0.0);
    return changed;
}

/// Percorre entities[].components[] e injeta os campos novos ausentes.
/// true = algo foi injetado (o chamador re-dumpa o JSON para o load).
[[nodiscard]] bool migrateSceneJsonAdditiveP46(eng::serial::JsonValue& root)
{
    using eng::serial::JsonValue;
    if (!root.isObject()) {
        return false;
    }
    const auto entities = root.find("entities");
    if (!entities.has_value() || !entities->isArray()) {
        return false;
    }
    bool changed = false;
    JsonValue newEntities = JsonValue::array();
    for (std::size_t i = 0; i < entities->size(); ++i) {
        JsonValue entity = entities->at(i);
        if (entity.isObject()) {
            if (const auto components = entity.find("components");
                components.has_value() && components->isArray()) {
                bool entityChanged = false;
                JsonValue newComponents = JsonValue::array();
                for (std::size_t c = 0; c < components->size(); ++c) {
                    JsonValue component = components->at(c);
                    if (component.isObject()) {
                        const auto type = component.find("type");
                        auto data = component.find("data");
                        if (type.has_value() && type->isString() &&
                            data.has_value() && data->isObject() &&
                            migrateComponentDataP46(type->asString(), *data)) {
                            component.set("data", std::move(*data));
                            entityChanged = true;
                        }
                        // P4.7.0 B4: câmera — campos novos com defaults.
                        if (migrateComponentDataP47(type->asString(), *data)) {
                            component.set("data", std::move(*data));
                            entityChanged = true;
                        }
                    }
                    newComponents.append(std::move(component));
                }
                if (entityChanged) {
                    entity.set("components", std::move(newComponents));
                    changed = true;
                }
            }
        }
        newEntities.append(std::move(entity));
    }
    if (changed) {
        root.set("entities", std::move(newEntities));
    }
    return changed;
}

}  // namespace

Result<void> EditorDocument::loadScene(std::string_view scenePath)
{
    if (!hasProject()) {
        return makeUnexpected(
            documentError(StatusCode::InvalidState, "sem projeto aberto"));
    }
    if (mode_ == Mode::Play) {
        return makeUnexpected(documentError(
            StatusCode::InvalidState, "cena é somente-leitura em Play"));
    }
    if (!isSafeRelativePath(scenePath)) {
        return makeUnexpected(documentError(StatusCode::InvalidArgument,
                                            "path relativo obrigatório (sem ..)"));
    }
    const eng::fs::Path path{std::string(scenePath)};
    const eng::fs::Path full = scenesRootOf(*project_) / path;
    auto text = fs_->readAllText(full);
    if (text.isError()) {
        return makeUnexpected(text.error());
    }
    auto fresh = newScene();
    if (fresh.isError()) {
        return makeUnexpected(fresh.error());
    }
    // Extrai o timestep da física ANTES do load (chave
    // aditiva do documento; arquivo antigo/ausente = default 1/60). Valor
    // inválido presente no arquivo é IGNORADO (config default) — o load da
    // cena nunca falha por config de ticks.
    {
        auto parsed = eng::serial::parseJson(text.value());
        if (parsed.ok() && parsed.value().isObject()) {
            const auto field = parsed.value().find("physicsFixedDt");
            if (field.has_value() && field->isNumber()) {
                const double v = field->asF64();
                if (std::isfinite(v) && v > 0.0 && v <= 0.25) {
                    physicsAccumulator_.setFixedDt(static_cast<float>(v));
                }
            }
            // P4.7.0 B5: varredura do kinematic (arquivo antigo/ausente =
            // ON — o script ingênuo COLIDE por padrão).
            kinematicSweep_ = true;
            if (const auto sweep = parsed.value().find(
                    "physicsKinematicSweep");
                sweep.has_value() && sweep->isBool()) {
                kinematicSweep_ = sweep->asBool();
            }
            // P4.7.0 B6: logic LOD (arquivo antigo/ausente = OFF — a
            // semântica pré-P4.7 de scripts é preservada; opt-in).
            logicLodEnabled_ = false;
            if (const auto lod = parsed.value().find("logicLodEnabled");
                lod.has_value() && lod->isBool()) {
                logicLodEnabled_ = lod->asBool();
            }
            // MIGRAÇÃO ADITIVA — campos refletidos
            // novos não existem em cenas pré-P4.6 e o decode é ESTRITO
            // com campo ausente. Injeta defaults ANTES do load
            // (mesmo padrão do physicsFixedDt); cenas novas já trazem os
            // campos (encode escreve tudo) e a migração é idempotente.
            if (migrateSceneJsonAdditiveP46(parsed.value())) {
                text = eng::serial::dumpJson(parsed.value());
            }
        }
    }
    auto loaded = eng::scene::SceneSerializer::load(*scene_, text.value());
    if (loaded.isError()) {
        return makeUnexpected(loaded.error());
    }
    // Mesma política do saveScene — path corrente + marker.
    currentScenePath_ = std::string(scenePath);
    auto marker = fs_->writeAllText(
        project_->paths().projectDir() / eng::fs::Path{kLastSceneFile},
        currentScenePath_);
    if (marker.isError()) {
        ENG_WARN("scene-op: falha ao registrar última cena: {}",
                 marker.error().message);
    }
    sceneDirty_ = false;
    return {};
}

// =============================================================================
// Entidades
// =============================================================================

Result<void> EditorDocument::requireEditMode() const
{
    if (mode_ == Mode::Play) {
        return makeUnexpected(
            documentError(StatusCode::InvalidState,
                          "edição rejeitada: editor em PLAY (§8.7)"));
    }
    return {};
}

Result<eng::ecs::Entity> EditorDocument::createEntity(
    std::string_view name, eng::ecs::Entity parent)
{
    auto guard = requireEditMode();
    if (guard.isError()) {
        return makeUnexpected(guard.error());
    }
    pushHistory("create");
    const eng::ecs::Entity entity = scene_->createNode();
    auto placed = scene_->world().emplace<eng::scene::Name>(
        entity, eng::scene::Name{name.empty() ? "Entity" : std::string(name)});
    if (placed == nullptr) {
        return makeUnexpected(
            documentError(StatusCode::Internal, "Name não emplantou"));
    }
    if (parent != eng::scene::kNoEntity) {
        auto attached = scene_->attach(entity, parent);
        if (!attached) {
            return makeUnexpected(documentError(
                StatusCode::InvalidArgument,
                "attach rejeitou o pai (inválido/ciclo)"));
        }
    }
    sceneDirty_ = true;
    return entity;
}

Result<void> EditorDocument::deleteEntity(eng::ecs::Entity entity)
{
    auto guard = requireEditMode();
    if (guard.isError()) {
        return makeUnexpected(guard.error());
    }
    if (!scene_->isNode(entity)) {
        return makeUnexpected(documentError(StatusCode::NotFound,
                                           "entidade obsoleta"));
    }
    pushHistory("delete");
    if (!scene_->destroyNode(entity)) {
        return makeUnexpected(
            documentError(StatusCode::Internal, "destroyNode falhou"));
    }
    if (selection_.has_value() && *selection_ == entity) {
        selection_.reset();
        ++selectionRevision_;  // seleção morreu com a entidade
    }
    sceneDirty_ = true;
    return {};
}

Result<void> EditorDocument::renameEntity(eng::ecs::Entity entity,
                                          std::string_view name)
{
    auto guard = requireEditMode();
    if (guard.isError()) {
        return makeUnexpected(guard.error());
    }
    if (name.empty()) {
        return makeUnexpected(
            documentError(StatusCode::InvalidArgument, "nome vazio"));
    }
    pushHistory("rename");
    auto* current = scene_->world().get<eng::scene::Name>(entity);
    if (current == nullptr) {
        current = scene_->world().emplace<eng::scene::Name>(
            entity, eng::scene::Name{std::string(name)});
        if (current == nullptr) {
            return makeUnexpected(documentError(StatusCode::NotFound,
                                                "entidade obsoleta"));
        }
    } else {
        current->value = std::string(name);
    }
    sceneDirty_ = true;
    return {};
}

Result<eng::ecs::Entity> EditorDocument::duplicateEntity(
    eng::ecs::Entity entity)
{
    auto guard = requireEditMode();
    if (guard.isError()) {
        return makeUnexpected(guard.error());
    }
    if (!scene_->isNode(entity)) {
        return makeUnexpected(documentError(StatusCode::NotFound,
                                           "entidade obsoleta"));
    }

    const std::string originalName = nameOf(*scene_, entity);
    std::string cloneName = originalName + ".alt";
    // Nome com sufixo repetido: numera para manter o rótulo legível.
    if (originalName.size() >= 4 &&
        originalName.compare(originalName.size() - 4, 4, ".alt") == 0) {
        cloneName = originalName + "2";
    }

    pushHistory("create");
    const eng::ecs::Entity clone =
        cloneSubtree(*scene_, entity, cloneName, /*activate=*/false);
    sceneDirty_ = true;
    ++selectionRevision_;  // clone entrou na cena → hierarquia/inspector
    return clone;
}

Result<void> EditorDocument::reparentEntity(eng::ecs::Entity entity,
                                            eng::ecs::Entity parent)
{
    auto guard = requireEditMode();
    if (guard.isError()) {
        return makeUnexpected(guard.error());
    }
    if (parent != eng::scene::kNoEntity && !scene_->isNode(parent)) {
        return makeUnexpected(documentError(StatusCode::InvalidArgument,
                                           "novo pai obsoleto"));
    }
    pushHistory("reparent");
    if (parent == eng::scene::kNoEntity) {
        // Raiz = detach (Scene::attach exige pai válido; já-raiz é no-op).
        if (!scene_->detach(entity) &&
            scene_->parentOf(entity) != eng::scene::kNoEntity) {
            return makeUnexpected(
                documentError(StatusCode::InvalidArgument,
                              "entidade obsoleta"));
        }
    } else if (!scene_->attach(entity, parent)) {
        return makeUnexpected(documentError(
            StatusCode::InvalidArgument,
            "attach rejeitou (inválido/self/ciclo — ADR-025)"));
    }
    sceneDirty_ = true;
    return {};
}

Result<TransformDesc> EditorDocument::transform(
    eng::ecs::Entity entity) const
{
    const auto* local = scene_->localTransform(entity);
    if (local == nullptr) {
        return makeUnexpected(documentError(StatusCode::NotFound,
                                           "entidade obsoleta"));
    }
    TransformDesc desc;
    desc.position = local->position;
    desc.rotationDegrees = degreesFromQuat(local->rotation);
    desc.scale = local->scale;
    return desc;
}

Result<void> EditorDocument::setTransform(eng::ecs::Entity entity,
                                          const TransformDesc& desc)
{
    auto guard = requireEditMode();
    if (guard.isError()) {
        return makeUnexpected(guard.error());
    }
    auto* local = scene_->localTransform(entity);
    if (local == nullptr) {
        return makeUnexpected(documentError(StatusCode::NotFound,
                                           "entidade obsoleta"));
    }
    if (!gizmo_.dragging()) {
        // Drag do gizmo captura no BEGIN (1 gesto = 1 undo); fora de
        // drag (Inspector TRS, applyTransform) cada apply = 1 passo.
        pushHistory("transform");
    }
    local->position = desc.position;
    local->rotation = quatFromDegrees(desc.rotationDegrees);
    local->scale = desc.scale;
    sceneDirty_ = true;
    ++selectionRevision_;  // Inspector → viewport: mudou transform
    return {};
}

// =============================================================================
// Seleção
// =============================================================================

Result<void> EditorDocument::select(eng::ecs::Entity entity)
{
    if (entity != eng::scene::kNoEntity && !scene_->isNode(entity)) {
        return makeUnexpected(documentError(StatusCode::NotFound,
                                           "entidade obsoleta"));
    }
    if (entity == eng::scene::kNoEntity) {
        selection_.reset();
        ++selectionRevision_;
        return {};
    }
    // Mudança de seleção MATA o
    // drag em voo. Nenhum estado de drag sobrevive — o gizmo é
    // reconstruído do (seleção, ferramenta, câmera) a cada frame e o
    // beginDrag é a ÚNICA forma de armá-lo.
    gizmoDragEnd();
    selection_ = entity;
    ++selectionRevision_;  // hierarquia selecionou → UI sincroniza
    return {};
}

void EditorDocument::deselect() noexcept
{
    gizmoDragEnd();  // re-armo — drag não sobrevive
    selection_.reset();
    ++selectionRevision_;
}

bool EditorDocument::isSelected(eng::ecs::Entity entity) const noexcept
{
    return selection_.has_value() && *selection_ == entity;
}

// =============================================================================
// Ferramentas + gizmo + sprite
// =============================================================================

GizmoBounds EditorDocument::selectionBounds(TextureCache* textures) const
{
    GizmoBounds bounds;
    if (!selection_.has_value() || mode_ != Mode::Edit) {
        return bounds;  // sem seleção/em Play: inválido (sem gizmo)
    }
    const eng::scene::Scene* scene = sceneInFocus();
    if (scene == nullptr || !scene->isNode(*selection_)) {
        return bounds;  // stale: seleção morreu (regressão P1.1)
    }
    auto quads = viewport_.buildQuads(*scene, selection_);
    for (const EntityQuad& quad : quads) {
        if (quad.entity != *selection_) {
            continue;
        }
        // Tamanho DESENHADO = escala × (região em px / ppu) para
        // sprites texturizados — o MESMO número do renderer/hit-test.
        float worldHalfW = quad.sizeX * 0.5f;
        float worldHalfH = quad.sizeY * 0.5f;
        if (!quad.textureAsset.empty() && textures != nullptr &&
            assets_ != nullptr) {
            const auto info =
                textures->imageInfo(*assets_, quad.textureAsset);
            if (info.valid) {
                const float regionPx =
                    static_cast<float>(info.width) * (quad.u1 - quad.u0);
                const float regionPy =
                    static_cast<float>(info.height) * (quad.v1 - quad.v0);
                const float ppu = quad.spritePpu > 0.f ? quad.spritePpu : 1.f;
                worldHalfW = quad.sizeX * regionPx / ppu * 0.5f;
                worldHalfH = quad.sizeY * regionPy / ppu * 0.5f;
            }
        }
        // Pivot desloca o centro visual do sprite (o renderer desenha o
        // quad com o offset do pivot — o bounds tem de casar).
        const float pivotOffX = (quad.pivotX - 0.5f) * worldHalfW * 2.f;
        const float pivotOffY = (quad.pivotY - 0.5f) * worldHalfH * 2.f;
        const float cosR = std::cos(quad.rotation);
        const float sinR = std::sin(quad.rotation);
        bounds.worldX = quad.worldX + pivotOffX * cosR - pivotOffY * sinR;
        bounds.worldY = quad.worldY + pivotOffX * sinR + pivotOffY * cosR;
        // Origem do NÓ separada do centro visual — o MOVE
        // opera sobre a ORIGEM (o que o Transform guarda); rotate/scale
        // continuam no centro visual (pivot).
        bounds.originX = quad.worldX;
        bounds.originY = quad.worldY;
        bounds.halfW = std::max(worldHalfW, pxToWorldMin());
        bounds.halfH = std::max(worldHalfH, pxToWorldMin());
        bounds.rotation = quad.rotation;
        bounds.valid = true;
        return bounds;
    }
    return bounds;
}

/// Meio-tamanho mínimo visível em mundo (handle sempre tocável).
float EditorDocument::pxToWorldMin() const noexcept
{
    const float zoom = viewport_.effectiveCamera().zoom;
    return zoom > 0.f ? Viewport::kMinQuadPixels * 0.5f / zoom : 0.5f;
}

GizmoHandle EditorDocument::gizmoDragBegin(float screenX, float screenY,
                                            TextureCache* textures)
{
    if (mode_ != Mode::Edit) {
        return GizmoHandle::None;  // edição é rejeitada em Play
    }
    const GizmoBounds bounds = selectionBounds(textures);
    if (!bounds.valid) {
        return GizmoHandle::None;
    }
    const GizmoHandle handle =
        gizmo_.hitTest(viewport_, tool_, bounds, screenX, screenY);
    if (handle == GizmoHandle::None) {
        return GizmoHandle::None;
    }
    // Transform INICIAL da EDIÇÃO (só em Edit — o gizmo não toca o clone).
    auto transform = this->transform(*selection_);
    if (transform.isError()) {
        return GizmoHandle::None;  // seleção stale no meio da operação
    }
    GizmoTransform start{};
    // O gizmo opera em MUNDO — a origem do nó vem dos
    // bounds ATUAIS (não do transform local, que só coincide na raiz).
    start.posX = bounds.originX;
    start.posY = bounds.originY;
    start.rotationDeg = transform.value().rotationDegrees.z;
    start.scaleX = transform.value().scale.x;
    start.scaleY = transform.value().scale.y;
    pushHistory("transform");       // 1 gesto de gizmo = 1 undo
    gizmoUndoArmed_ = true;         // limpeza de no-op no end
    revisionAtDragBegin_ = selectionRevision_;
    gizmo_.beginDrag(handle, start, viewport_, bounds, screenX, screenY);

    // Contexto do drag (vivo até gizmoDragEnd): mesmas texturas do begin
    // (R1 — bounds consistentes entre begin E dragTo) + inversa 2x2 do
    // pai (R2 — delta de mundo → espaço local do filho).
    dragTextures_ = textures;
    dragParentInv_ = parentInverse2D(*selection_);
    return handle;
}

Result<void> EditorDocument::gizmoDragTo(float screenX, float screenY)
{
    if (!gizmo_.dragging() || mode_ != Mode::Edit) {
        return {};
    }
    if (!selection_.has_value()) {
        gizmoDragEnd();
        return makeUnexpected(
            documentError(StatusCode::NotFound, "seleção perdida no drag"));
    }
    // R1 (bug §5): MESMA fonte de bounds do beginDrag — o cache de
    // texturas do drag, capturado no begin. Antes: nullptr aqui e
    // texturizado no begin → com pivot != (0.5,0.5) o centro de
    // referência do rotate/scale MUDAVA no meio do drag.
    const GizmoBounds bounds = selectionBounds(dragTextures_);
    if (!bounds.valid) {
        gizmoDragEnd();
        return makeUnexpected(
            documentError(StatusCode::NotFound, "entidade obsoleta"));
    }
    GizmoTransform target = gizmo_.dragTo(viewport_, bounds, screenX, screenY);
    // Translação → grade de 0.5
    // unidades; rotação → múltiplos de 15°. Aplica ao ALVO EM MUNDO
    // (raiz: posição local == mundo; filho: snap é aproximação — a
    // inversa do pai preserva o resto do gesto).
    if (snapTranslate_) {
        target.posX = std::round(target.posX / kSnapTranslateStep) *
                      kSnapTranslateStep;
        target.posY = std::round(target.posY / kSnapTranslateStep) *
                      kSnapTranslateStep;
    }
    if (snapRotate_) {
        target.rotationDeg = std::round(target.rotationDeg / kSnapRotateDeg) *
                             kSnapRotateDeg;
    }

    // Aplica ao ECS REAL. R2 (bug §5): o alvo do MOVE está em MUNDO —
    // converte o delta pela INVERSA do pai (raiz: identidade). Somar o
    // delta de mundo direto na posição LOCAL movia filhos de pais
    // rotacionados no EIXO ERRADO da tela.
    auto current = transform(*selection_);
    if (current.isError()) {
        gizmoDragEnd();
        return makeUnexpected(current.error());
    }
    TransformDesc desc = current.value();
    const float worldDeltaX = target.posX - bounds.originX;
    const float worldDeltaY = target.posY - bounds.originY;
    desc.position.x += dragParentInv_[0] * worldDeltaX +
                       dragParentInv_[1] * worldDeltaY;
    desc.position.y += dragParentInv_[2] * worldDeltaX +
                       dragParentInv_[3] * worldDeltaY;
    desc.rotationDegrees.z = target.rotationDeg;
    desc.scale.x = target.scaleX;
    desc.scale.y = target.scaleY;
    auto applied = setTransform(*selection_, desc);
    if (applied.isError()) {
        gizmoDragEnd();
        return applied;
    }
    ++selectionRevision_;  // Inspector atualiza ao vivo
    return {};
}

void EditorDocument::gizmoDragEnd() noexcept
{
    gizmo_.endDrag();
    dragTextures_ = nullptr;     // contexto do drag morre com o drag
    dragParentInv_ = {1.f, 0.f, 0.f, 1.f};
    if (gizmoUndoArmed_) {
        gizmoUndoArmed_ = false;
        // Toque no handle sem arrastar (ou drag falhou): a entrada era
        // no-op — sai do histórico (undo não vira "passo fantasma").
        if (selectionRevision_ == revisionAtDragBegin_ && !undoStack_.empty()) {
            undoStack_.pop_back();
        }
    }
}

/// Inversa 2x2 da parte LINEAR do world matrix do PAI (identidade na
/// raiz). Converte deltas de MUNDO → espaço LOCAL do filho (bug §5 R2):
/// column-major → m00=at(0,0), m01=at(1,0), m10=at(0,1), m11=at(1,1);
/// inv = 1/det · [[m11,-m01],[-m10,m00]]. Det 0 (pai degenerado) →
/// identidade honesta (drag continua respondendo, sem NaN).
std::array<float, 4> EditorDocument::parentInverse2D(
    eng::ecs::Entity entity) const noexcept
{
    std::array<float, 4> identity{1.f, 0.f, 0.f, 1.f};
    if (!scene_.has_value()) {
        return identity;
    }
    const eng::ecs::Entity parent = scene_->parentOf(entity);
    if (parent == eng::scene::kNoEntity) {
        return identity;
    }
    const eng::math::Mat4 pw = scene_->computeWorldMatrix(parent);
    const float m00 = pw.at(0, 0), m01 = pw.at(1, 0);
    const float m10 = pw.at(0, 1), m11 = pw.at(1, 1);
    const float det = m00 * m11 - m01 * m10;
    if (std::abs(det) < 1e-9f) {
        return identity;
    }
    const float inv = 1.f / det;
    return {m11 * inv, -m01 * inv, -m10 * inv, m00 * inv};
}

GizmoDrawData EditorDocument::gizmoDraw(TextureCache* textures) const
{
    GizmoDrawData draw;
    if (mode_ != Mode::Edit || tool_ == EditorTool::Select) {
        return draw;
    }
    const GizmoBounds bounds = selectionBounds(textures);
    if (!bounds.valid) {
        return draw;
    }
    draw.quads = gizmo_.layoutQuads(viewport_, tool_, bounds);
    draw.triangles = gizmo_.layoutTriangles(viewport_, tool_, bounds);
    draw.segments = gizmo_.layoutSegments(viewport_, tool_, bounds);
    // Transição entre tools — POP de 120ms nos handles (a
    // hit-test NÃO muda: alvo de toque constante, só o visual escala).
    const float pop = gizmoHandlePop();
    if (pop < 1.f) {
        for (GizmoQuad& quad : draw.quads) {
            quad.halfW *= pop;
            quad.halfH *= pop;
        }
        for (GizmoTriangle& triangle : draw.triangles) {
            triangle.halfW *= pop;
            triangle.halfH *= pop;
        }
    }
    return draw;
}

Result<eng::ecs::Entity> EditorDocument::createSprite(std::string_view name)
{
    auto guard = requireEditMode();
    if (guard.isError()) {
        return makeUnexpected(guard.error());
    }
    // Numeração automática: Sprite, Sprite 2, Sprite 3… (varre a
    // hierarquia — nomes únicos mantêm a UI legível; P1.10).
    std::string base{name.empty() ? "Sprite" : std::string(name)};
    auto nodes = hierarchySnapshot();
    if (std::any_of(nodes.begin(), nodes.end(),
                    [&](const HierarchyNode& n) { return n.name == base; })) {
        for (int suffix = 2;; ++suffix) {
            const std::string candidate = base + " " + std::to_string(suffix);
            if (!std::any_of(nodes.begin(), nodes.end(),
                             [&](const HierarchyNode& n) {
                                 return n.name == candidate;
                             })) {
                base = candidate;
                break;
            }
        }
    }
    pushHistory("create");
    suppressHistory_ = true;   // createEntity aninhado não duplica entrada
    auto entity = createEntity(base, eng::scene::kNoEntity);
    suppressHistory_ = false;
    if (entity.isError()) {
        return makeUnexpected(entity.error());
    }
    // SpriteData default: ppu 48 (1 texel : 1 px no zoom padrão), sem
    // textura → o renderer desenha o PLACEHOLDER xadrez (P1.10 — não
    // confundir placeholder com sprite renderizado).
    if (scene_->world().emplace<eng::editor::SpriteData>(
            entity.value(), eng::editor::SpriteData{}) == nullptr) {
        (void)scene_->destroyNode(entity.value());
        return makeUnexpected(documentError(StatusCode::Internal,
                                            "SpriteData não emplantou"));
    }
    selection_ = entity.value();
    ++selectionRevision_;
    sceneDirty_ = true;
    return entity;
}

// =============================================================================
// Componentes
// =============================================================================

std::vector<Inspector::Field> EditorDocument::inspectorFields(
    eng::ecs::Entity entity, std::string_view component) const
{
    // Leitura ALLOWED em Play (inspeção do runtime — ferramenta de debug).
    const eng::scene::Scene* scene = sceneInFocus();
    if (scene == nullptr) {
        return {};
    }
    // Handles de EDIÇÃO chegam aqui (seleção pré-Play, JNI); o clone tem
    // os PRÓPRIOS — traduz na fronteira (bug do clone aleatório).
    auto fields = Inspector::fieldsOf(*scene, toFocus(entity), component);
    if (fields.isError()) {
        ENG_WARN("inspector: {} (entity {}.{})", fields.error().message,
                 entity.index, entity.generation);
        return {};
    }
    return fields.value();
}

Result<void> EditorDocument::setInspectorField(eng::ecs::Entity entity,
                                               std::string_view component,
                                               std::string_view fieldPath,
                                               std::string_view value)
{
    auto guard = requireEditMode();
    if (guard.isError()) {
        return makeUnexpected(guard.error());
    }
    // Transform é TRS com rotação em QUAT — escrever
    // "rotation.z=30" direto no campo produzia um quat inválido que
    // decomponha para ~178° (graus viravam componente de quat). TODA
    // escrita de Transform pela UI passa pela API TRS (graus ↔ quat na
    // MESMA convenção do gizmo/setTransform) — uma fonte de verdade.
    if (component == "eng::math::Transform") {
        return setTransformField(entity, fieldPath, value);
    }
    pushHistory("field");
    auto written =
        Inspector::setField(*scene_, entity, component, fieldPath, value);
    if (written.isError()) {
        return makeUnexpected(written.error());
    }
    sceneDirty_ = true;
    return {};
}

namespace {

/// Campo TRS de "eng::math::Transform" (posição/rotação/escala × x/y/z).
[[nodiscard]] bool parseTransformField(std::string_view fieldPath,
                                       std::string_view value, int& axis,
                                       float& out)
{
    // fieldPath vem como "position.x" | "rotation.y" | "scale.z".
    constexpr std::string_view kPrefixes[3] = {"position.", "rotation.",
                                              "scale."};
    int group = -1;
    for (int i = 0; i < 3; ++i) {
        if (fieldPath.substr(0, kPrefixes[i].size()) == kPrefixes[i]) {
            group = i;
            fieldPath.remove_prefix(kPrefixes[i].size());
            break;
        }
    }
    if (group < 0 || fieldPath.size() != 1) {
        return false;
    }
    const char c = fieldPath[0];
    if (c != 'x' && c != 'y' && c != 'z') {
        return false;
    }
    axis = (group << 2) | (c == 'x' ? 0 : (c == 'y' ? 1 : 2));
    // Parse float estrito SEM exceções (lib é -fno-exceptions — ADR-004):
    // strtof + fim-da-string; rejeita lixo/NaN/inf (contrato Inspector).
    const std::string text{value};
    const char* begin = text.c_str();
    char* end = nullptr;
    out = std::strtof(begin, &end);
    return end != begin && *end == '\0' && std::isfinite(out);
}

}  // namespace

Result<void> EditorDocument::setTransformField(eng::ecs::Entity entity,
                                                std::string_view fieldPath,
                                                std::string_view value)
{
    int axis = -1;
    float v = 0.f;
    if (parseTransformField(fieldPath, value, axis, v)) {
        auto current = transform(entity);
        if (current.isError()) {
            return makeUnexpected(current.error());
        }
        TransformDesc desc = current.value();
        switch (axis) {
        // position (0-2)
        case (0 << 2) | 0: desc.position.x = v; break;
        case (0 << 2) | 1: desc.position.y = v; break;
        case (0 << 2) | 2: desc.position.z = v; break;
        // rotation em GRAUS (3-5) — quatFromDegrees na escrita
        case (1 << 2) | 0: desc.rotationDegrees.x = v; break;
        case (1 << 2) | 1: desc.rotationDegrees.y = v; break;
        case (1 << 2) | 2: desc.rotationDegrees.z = v; break;
        // scale (6-8) — P1.5: impedir valores inválidos (0/neg/NaN)
        case (2 << 2) | 0:
            desc.scale.x = std::clamp(
                v, eng::editor::TransformGizmo::kScaleMin,
                eng::editor::TransformGizmo::kScaleMax);
            break;
        case (2 << 2) | 1:
            desc.scale.y = std::clamp(
                v, eng::editor::TransformGizmo::kScaleMin,
                eng::editor::TransformGizmo::kScaleMax);
            break;
        case (2 << 2) | 2:
            desc.scale.z = std::clamp(
                v, eng::editor::TransformGizmo::kScaleMin,
                eng::editor::TransformGizmo::kScaleMax);
            break;
        default: break;
        }
        return setTransform(entity, desc);  // dirty + revision bump
    }
    // Campo não-TRS (não existe hoje): cai no caminho genérico.
    auto written =
        Inspector::setField(*scene_, entity, "eng::math::Transform",
                            fieldPath, value);
    if (written.isError()) {
        return makeUnexpected(written.error());
    }
    sceneDirty_ = true;
    return {};
}

Result<void> EditorDocument::addComponent(eng::ecs::Entity entity,
                                          std::string_view component)
{
    auto guard = requireEditMode();
    if (guard.isError()) {
        return makeUnexpected(guard.error());
    }
    pushHistory("component");
    // Contrato validado DENTRO do Inspector (requires/
    // conflicts/single com erro preciso); efeitos colaterais NATIVOS da
    // luz/física/materiais vivem em onAttach (registro em
    // ComponentRegistration.cpp) — o caso especial da Light2D migrou
    // para o hook (mesma semântica do P4.6 Bloco 2).
    auto added = Inspector::addComponent(*scene_, entity, component,
                                         /*attachUser=*/this);
    if (added.isError()) {
        return makeUnexpected(added.error());
    }
    sceneDirty_ = true;
    ++selectionRevision_;  // Inspector reflete o componente novo
    return {};
}

bool EditorDocument::materialCountsAsLit(const std::string& asset) const
{
    if (asset.empty()) {
        return true; // material vazio = lit (default)
    }
    const auto it = materialCache_.find(asset);
    if (it == materialCache_.end()) {
        return true; // cache frio conta como lit — default do engine
    }
    return it->second.shader == std::string(eng::render::kShaderLit);
}

Result<void> EditorDocument::removeComponent(eng::ecs::Entity entity,
                                             std::string_view component)
{
    auto guard = requireEditMode();
    if (guard.isError()) {
        return makeUnexpected(guard.error());
    }
    pushHistory("remove");
    // Recusa quando outro componente presente EXIGE o
    // removido (erro com o nome do dependente); onDetach roda pós-remoção.
    auto removed = Inspector::removeComponent(*scene_, entity, component,
                                              /*detachUser=*/this);
    if (removed.isError()) {
        return makeUnexpected(removed.error());
    }
    sceneDirty_ = true;
    ++selectionRevision_;
    return {};
}

// =============================================================================
// Componentes authoráveis (P2 §2/§14)
// =============================================================================

namespace {

/// Hint de dependência por tipo (P2 §14 — o catálogo é ÚNICO; os hints
/// informam o AUTOR sem inventar componentes falsos). Vazios = sem
/// dependência. O formato é texto livre para a UI exibir como está.
[[nodiscard]] std::string dependencyHintFor(std::string_view component)
{
    // Hint de CONTRATO primeiro (fonte única — o mesmo
    // registro que valida o add); legacy depois (dicas não-expressíveis
    // como requires — assets ausentes, painel recomendado).
    const auto* contract = eng::editor::Inspector::contractOf(component);
    if (contract != nullptr) {
        if (!contract->required.empty()) {
            std::string hint = "exige ";
            for (std::size_t i = 0; i < contract->required.size(); ++i) {
                if (i != 0) {
                    hint += ", ";
                }
                hint += contract->required[i];
            }
            return hint;
        }
        if (!contract->conflicts.empty()) {
            std::string hint = "conflita com ";
            for (std::size_t i = 0; i < contract->conflicts.size(); ++i) {
                if (i != 0) {
                    hint += ", ";
                }
                hint += contract->conflicts[i];
            }
            return hint;
        }
    }
    if (component == "eng::physics::RigidBody") {
        return "Colisão requer Collider (corpo sem collider atravessa)";
    }
    if (component == "eng::animation::Animator") {
        return "Requer um clip em assets/animations (painel Animação)";
    }
    if (component == "eng::editor::AudioSource") {
        return "Requer um WAV em assets/audio (importe no Assets)";
    }
    if (component == "eng::editor::NiScriptComponent") {
        return "Prefira anexar pelo painel Scripts (editor embutido)";
    }
    return {};
}

}  // namespace

std::vector<EditorDocument::ComponentMeta>
EditorDocument::addableComponents(eng::ecs::Entity entity) const
{
    std::vector<ComponentMeta> out;
    if (!scene_.has_value() || !scene_->isNode(entity)) {
        return out;
    }
    for (const auto& name : Inspector::catalog()) {
        // Built-ins obrigatórios não são addáveis (todo nó já os tem).
        if (!Inspector::isRemovable(name)) {
            continue;
        }
        // Já presente → o Inspector lista os campos; nada a adicionar.
        if (!Inspector::componentsOf(*scene_, entity).empty()) {
            bool present = false;
            for (const auto& has : Inspector::componentsOf(*scene_, entity)) {
                if (has == name) {
                    present = true;
                    break;
                }
            }
            if (present) {
                continue;
            }
        }
        out.push_back(ComponentMeta{name, true, dependencyHintFor(name)});
    }
    return out;
}

Result<std::vector<std::string>>
EditorDocument::addComponentWithDependencies(eng::ecs::Entity entity,
                                             std::string_view component)
{
    auto guard = requireEditMode();
    if (guard.isError()) {
        return makeUnexpected(guard.error());
    }
    pushHistory("component");
    suppressHistory_ = true;  // o addComponent aninhado não duplica entrada
    auto added = addComponent(entity, component);
    suppressHistory_ = false;
    if (added.isError()) {
        return makeUnexpected(added.error());
    }
    std::vector<std::string> created{std::string(component)};

    // Auto-criação SEGURA (P2 §14 — aditiva, nunca destrutiva):
    // Animator num clip com FRAMES precisa de SpriteData para o autor
    // VER o flipbook; SpriteData default é placeholder inofensivo.
    if (component == "eng::animation::Animator") {
        // Animator default tem clip "idle" — sem banco em Edit, nada a
        // validar aqui; o preview/assign cuida do resto.
    }
    return created;
}

// =============================================================================
// Play/Stop + Tick architecture
// =============================================================================

namespace {

/// ScriptTick (evolução P0-5): NI-Script no agendador. Vive no EDITOR
/// porque depende de NiRuntime (camada de composição — mesmo padrão do
/// catálogo de componentes, ADR-043).
class ScriptTick final : public eng::tick::TickSystem {
public:
    explicit ScriptTick(NiRuntime& runtime) noexcept : runtime_(runtime) {}

    [[nodiscard]] const char* name() const override { return "ScriptTick"; }
    [[nodiscard]] eng::tick::Phase phase() const override
    {
        return eng::tick::Phase::Update;
    }
    [[nodiscard]] int order() const override { return 30; }

    void tick(eng::scene::Scene& /*scene*/, float dt) override
    {
        if (runtime_.empty()) {
            return;
        }
        runtime_.tick(dt);
    }

private:
    NiRuntime& runtime_;
};

/// AudioTick (P2 §12): AudioSources do CLONE no mixer REAL. Primeiro
/// frame de cada voz playOnStart dispara UMA vez (set); vozes loop vivem
/// até o stopAll do stop(). O backend (AAudio/null) pertence ao HOST —
/// aqui apenas o CAMINHO REAL de vozes/mixer.
class AudioTick final : public eng::tick::TickSystem {
public:
    AudioTick(EditorDocument& document) noexcept : document_(document) {}

    [[nodiscard]] const char* name() const override { return "AudioTick"; }
    [[nodiscard]] eng::tick::Phase phase() const override
    {
        return eng::tick::Phase::Update;
    }
    [[nodiscard]] int order() const override { return 40; }

    void tick(eng::scene::Scene& scene, float /*dt*/) override
    {
        // playOnStart dispara UMA VEZ por Play (o set acumula — sem
        // clear: o loop de vida é o do próprio Play/stop).
        scene.world().each<eng::editor::AudioSource>(
            [&](eng::ecs::Entity entity,
                const eng::editor::AudioSource& source) {
                const bool already =
                    std::find(started_.begin(), started_.end(),
                              entity.index) != started_.end();
                if (!source.playOnStart || already ||
                    source.soundAsset.empty()) {
                    return;
                }
                started_.push_back(entity.index);
                auto sound = document_.soundFor(source.soundAsset);
                if (sound.isError()) {
                    ENG_WARN("AudioTick: {}", sound.error().message);
                    return;
                }
                auto played = document_.audioMixer().playSound(
                    *sound.value(), eng::audio::AudioMixer::kMasterBus,
                    source.volume, source.loop);
                if (played.isError()) {
                    ENG_WARN("AudioTick: {}", played.error().message);
                }
            });
        // Manutenção da thread do jogo: recolhe vozes encerradas.
        document_.audioMixer().tick();
    }

private:
    AudioTick(const AudioTick&) = delete;
    AudioTick& operator=(const AudioTick&) = delete;

    EditorDocument& document_;
    std::vector<std::uint32_t> started_;  ///< índices já disparados
};

}  // namespace

Result<void> EditorDocument::play()
{
    if (mode_ == Mode::Play) {
        return makeUnexpected(
            documentError(StatusCode::InvalidState, "já em Play"));
    }
    if (runtimeInput_.bindings().actions().empty()) {
        runtimeInput_.setBindings(defaultGameBindings());
    }
    // Scripts ligados a arquivo rodam a versão atual do arquivo.
    if (assets_ != nullptr) {
        scene_->world().each<eng::editor::NiScriptComponent>(
            [&](eng::ecs::Entity, eng::editor::NiScriptComponent& script) {
                if (script.scriptAsset.empty()) {
                    return;
                }
                auto content = scriptRead(script.scriptAsset);
                if (content.ok()) {
                    script.source = std::move(content.value());
                }
            });
    }
    // Validação de contratos ANTES de entrar em Play —
    // componente inválido (ex.: Collider.radius negativo introduzido por
    // caminho externo ao Inspector) NÃO entra em jogo: erro preciso com
    // tipo + nó. Custo O(nós × componentes) uma vez por Play — editor.
    {
        const auto& entries = eng::scene::detail::componentEntries();
        std::optional<eng::core::Error> contractError;
        scene_->world().each<eng::scene::SceneIdentity>(
            [&](eng::ecs::Entity node, const eng::scene::SceneIdentity&) {
                if (contractError.has_value() || !scene_->isNode(node)) {
                    return;
                }
                for (const auto& [name, entry] : entries) {
                    if (entry.onValidate == nullptr
                        || !entry.has(scene_->world(), node)) {
                        continue;
                    }
                    auto validated = entry.onValidate(*scene_, node, entry);
                    if (validated.isError()) {
                        contractError = eng::core::Error{
                            StatusCode::InvalidArgument,
                            "não é possível entrar em Play: '" + name
                                + "' no nó " + std::to_string(node.index)
                                + " é inválido — "
                                + validated.error().message};
                        return;
                    }
                }
            });
        if (contractError.has_value()) {
            return makeUnexpected(*contractError);
        }
    }
    // Entrar em Play PARA o preview (isolamento de vozes — a
    // voice de preview não atravessa a fronteira Edit→Play).
    audioPreviewStop();
    // Entrar em Play mata o drag do gizmo (o
    // clone é outra cena — nenhum estado de edição vaza para o runtime).
    gizmoDragEnd();
    // Clone por serialização: o round-trip é teste da FASE 3; a edição
    // permanece intocada por construção (nenhum ponteiro compartilhado).
    auto snapshot = eng::scene::SceneSerializer::save(*scene_);
    if (snapshot.isError()) {
        return makeUnexpected(snapshot.error());
    }
    runtimeScene_.emplace();
    auto loaded =
        eng::scene::SceneSerializer::load(*runtimeScene_, snapshot.value());
    if (loaded.isError()) {
        runtimeScene_.reset();
        return makeUnexpected(loaded.error());
    }
    // Scripts do clone compilam/instanciam AGORA (ADR-044 — a
    // edição nunca é tocada); @init roda na criação, `up start` a seguir.
    niRuntime_->setActionQuery(
        [](std::string_view action, int phase, void* user) {
            auto* input = static_cast<eng::input::InputSystem*>(user);
            const eng::input::ActionState state = input->action(action);
            return phase == 0 ? state.down
                   : phase == 1 ? state.pressed
                                : state.released;
        },
        &runtimeInput_);
    // P4.7.0 B5: o setting da cena dirige a varredura do kinematic.
    niRuntime_->setKinematicSweep(kinematicSweep_);
    // P4.7.0 B6: logic LOD — filtro instalado SÓ com o setting ON
    // (default OFF: comportamento pré-P4.7 preservado).
    niRuntime_->setLodFilter(
        logicLodEnabled_ ? &EditorDocument::lodFilterThunk : nullptr,
        this);
    niRuntime_->setRandomSeed(
        scriptSeed_ != 0
            ? scriptSeed_
            : static_cast<std::uint64_t>(
                  std::chrono::steady_clock::now().time_since_epoch().count()));
    niRuntime_->setSoundPlayer(
        [](void* user, std::string_view asset, float volume) {
            auto* doc = static_cast<EditorDocument*>(user);
            auto sound = doc->soundFor(asset);
            if (sound.isError()) {
                return false;
            }
            return doc->audioMixer()
                .playSound(*sound.value(), eng::audio::AudioMixer::kMasterBus,
                           volume, false)
                .ok();
        },
        this);
    niRuntime_->start(*runtimeScene_);
    niRuntime_->fireStart();

    // BUG DO CLONE ALEATÓRIO (pego pelo teste §10 — flaky ~40%): o save
    // ordena entidades por SceneEntityId e o
    // load recria nessa ordem — UUID é aleatório, então os ÍNDICES do
    // clone NÃO correspondem aos da edição. Handles de edição usados
    // contra o clone endereçavam a entidade ERRADA (inspector/move em
    // Play liam outra entidade). Correção na fronteira CERTA: o mapa
    // edição→runtime vivo enquanto o clone existir; leitores do foco
    // traduzem por toFocus(). A seleção pré-Play é REMAPEADA (continua
    // selecionada no clone — o que o usuário esperava ao apertar Play).
    editToRuntime_.clear();
    {
        std::unordered_map<eng::scene::SceneEntityId, eng::ecs::Entity>
            runtimeById;
        runtimeScene_->world().each<eng::scene::SceneIdentity>(
            [&](eng::ecs::Entity runtimeEntity,
                const eng::scene::SceneIdentity& identity) {
                runtimeById[identity.id] = runtimeEntity;
            });
        scene_->world().each<eng::scene::SceneIdentity>(
            [&](eng::ecs::Entity editEntity,
                const eng::scene::SceneIdentity& identity) {
                const auto it = runtimeById.find(identity.id);
                if (it != runtimeById.end()) {
                    editToRuntime_[editEntity] = it->second;
                }
            });
    }
    if (selection_.has_value()) {
        // O handle da
        // EDIÇÃO é capturado ANTES do remapeamento (a cópia remapeada
        // morre com o clone no stop(); esta é restaurada).
        selectionBeforePlay_ = selection_;
        const auto mapped = editToRuntime_.find(*selection_);
        if (mapped != editToRuntime_.end()) {
            selection_ = mapped->second;
        }
    }
    paused_ = false;  // Play novo começa rodando (pause é estado do gesto)

    // Evolução P0-5: o frame do jogo é o TICK SCHEDULER —
    // sistemas ordenados por (fase, ordem, inserção). Mesma ordem de
    // execução de antes (física → animação → partículas → scripts →
    // áudio → câmera), agora DECLARADA, testável e extensível.
    // P2 §8: o banco de animação é preenchido com TODOS os clips do
    // projeto (assets reais — o AnimationTick REAL os executa).
    loadAnimationBank();
    scheduler_ = std::make_unique<eng::tick::TickScheduler>();
    (void)scheduler_->addSystem(std::make_unique<eng::tick::PhysicsTick>(
        physicsWorld_, physicsAccumulator_));
    (void)scheduler_->addSystem(
        std::make_unique<eng::tick::AnimationTick>(runtimeAnimations_));
    (void)scheduler_->addSystem(std::make_unique<eng::tick::ParticleTick>());
    (void)scheduler_->addSystem(
        std::make_unique<ScriptTick>(*niRuntime_));
    (void)scheduler_->addSystem(std::make_unique<AudioTick>(*this));
    auto cameraTick = std::make_unique<eng::tick::CameraTickSystem>();
    cameraTick_ = cameraTick.get();  // P4.7.0 B4: hint de vista por frame
    (void)scheduler_->addSystem(std::move(cameraTick));

    mode_ = Mode::Play;
    // Câmera de jogo resolvida SEM rodar o frame: `up update` (e qualquer
    // sistema com efeito) só roda em tick() explícito do host — contrato
    // FASE 11 (play() não avança o mundo). O CameraTick ainda não tem
    // cache (nenhum frame rodou): resolução direta.
    syncGameCamera(eng::tick::resolveActiveCamera(*runtimeScene_));
    {
        std::string ticks;
        for (const std::string& name : scheduler_->systemOrder()) {
            ticks += ticks.empty() ? name : ", " + name;
        }
        ENG_INFO("PLAY: runtime clone pronto ({} nós, {} scripts; ticks: {})",
                 runtimeScene_->nodeCount(), niRuntime_->size(), ticks);
    }
    return {};
}

void EditorDocument::stop() noexcept
{
    if (mode_ == Mode::Play) {
        mode_ = Mode::Edit;
        gizmoDragEnd();  // re-armo no retorno à edição
        cameraTick_ = nullptr;  // P4.7.0 B4: morre com o scheduler
        scheduler_.reset();  // ticks morrem com o clone
        gameCameraActive_ = false;
        viewport_.setGameCamera(nullptr);  // câmera do editor volta
        niRuntime_->shutdown(); // `up destroy` + descarte (bindings morrem
                                // JUNTOS com o clone — ADR-044)
        audioMixer_.stopAll();  // P2 §12: vozes do Play morrem com o clone
        runtimeScene_.reset();
        // O
        // Modo Jogo exige voltar COM a seleção intacta — o handle da
        // EDIÇÃO capturado no play() é restaurado (o remapeado ao clone
        // é órfão aqui). Handle morto na edição → reset honesto.
        if (selectionBeforePlay_.has_value() &&
            scene_->isNode(*selectionBeforePlay_)) {
            selection_ = *selectionBeforePlay_;
        } else {
            selection_.reset();
        }
        selectionBeforePlay_.reset();
        paused_ = false;
        ++selectionRevision_;  // UI percebe o retorno
        editToRuntime_.clear();
        ENG_INFO("STOP: runtime descartado — edição intacta");
    }
}

void EditorDocument::tick(float deltaSeconds) noexcept
{
    // Em Edit o runtime fica PARADO (gestos do editor não vazam — §6.4);
    // o PREVIEW de animação (P2 §8) avança com o frame do host — o
    // renderFrame chama tick() sempre, e o preview é o único consumidor
    // de tempo em Edit.
    if (mode_ != Mode::Play) {
        previewTick(deltaSeconds);
        return;
    }
    // Runtime CONGELADO — nenhum sistema
    // avança (física/scripts/animação/áudio); a câmera de jogo fica no
    // último estado e o host continua RENDERIZANDO (frame vivo, mundo
    // parado — sem tela morta).
    if (paused_) {
        return;
    }
    // Input com janela de um update por frame.
    runtimeInput_.update();

    // Evolução P0-5: frame completo pelo TickScheduler —
    // física (timestep fixo), animação, partículas, scripts, áudio e
    // câmera nas fases/ordens declaradas no play(). Determinismo: a ordem é
    // fixa e cada sistema vê o estado deixado pelos anteriores.
    // P4.7.0 B4: o CameraTick recebe o tamanho REAL da vista — o clamp
    // pós-zoom dos limites precisa das meia-extensões visíveis.
    if (cameraTick_ != nullptr) {
        cameraTick_->setViewSize(viewport_.screenWidth(),
                                 viewport_.screenHeight());
    }
    {
        // Retângulo visível (câmera de jogo) para view_left()/… dos scripts.
        const float left = viewport_.screenToWorldX(0.f);
        const float right = viewport_.screenToWorldX(viewport_.screenWidth());
        const float top = viewport_.screenToWorldY(0.f);
        const float bottom = viewport_.screenToWorldY(viewport_.screenHeight());
        niRuntime_->setView(std::min(left, right), std::max(left, right),
                            std::min(top, bottom), std::max(top, bottom));
    }
    scheduler_->runFrame(*runtimeScene_, deltaSeconds);

    // P2 §8: FRAMES do clip do Animator aplicados ao SpriteData do clone
    // (o AnimationTick avançou o cursor; a aplicação visual é da camada
    // que conhece SpriteData — editor). O MESMO código do preview.
    applyAnimatorFrames(*runtimeScene_);

    // Câmera de jogo: o CameraTick cacheou a ativa no frame; o
    // viewport passa a ver POR ELA (render/hit-test/arraste seguem).
    const auto* cameraSystem = static_cast<const eng::tick::CameraTickSystem*>(
        scheduler_->find("CameraTick"));
    syncGameCamera(
        cameraSystem != nullptr
            ? cameraSystem->activeCamera()
            : eng::tick::resolveActiveCamera(*runtimeScene_));

    // `restart()` num script: recomeça a fase a partir da cena editada.
    if (niRuntime_->consumeRestartRequest()) {
        stop();
        (void)play();
    }
}

// Thunk do logic LOD (ponteiro de função não captura —
// o user é o documento). false = pulo o `up update` deste frame.
bool EditorDocument::lodFilterThunk(void* user, eng::ecs::Entity self)
{
    return static_cast<EditorDocument*>(user)->lodFilter(self);
}

bool EditorDocument::lodFilter(eng::ecs::Entity self) const
{
    const eng::scene::Scene* scene = sceneInFocus();
    if (scene == nullptr) {
        return true; // sem cena: nunca pula (honesto)
    }
    // Opt-out: gameplay crítico (spawner/placar/IA global) roda sempre.
    if (const auto* script =
            scene->world().get<eng::editor::NiScriptComponent>(self);
        script != nullptr && script->lodOptOut) {
        return true;
    }
    // AABB da entidade (escala pelas normas das colunas — mesma matemática
    // dos quads) contra a VISTA (câmera de jogo ativa) + a MESMA margem
    // do culling de render (coerente: o que desenha roda).
    const eng::math::Mat4 world = scene->computeWorldMatrix(self);
    const float x = world.at(3, 0);
    const float y = world.at(3, 1);
    const float sx = std::sqrt(world.at(0, 0) * world.at(0, 0) +
                               world.at(0, 1) * world.at(0, 1) +
                               world.at(0, 2) * world.at(0, 2));
    const float sy = std::sqrt(world.at(1, 0) * world.at(1, 0) +
                               world.at(1, 1) * world.at(1, 1) +
                               world.at(1, 2) * world.at(1, 2));
    const float rot = std::atan2(world.at(0, 1), world.at(0, 0));
    const float cosR = std::abs(std::cos(rot));
    const float sinR = std::abs(std::sin(rot));
    const float halfX = (cosR * sx + sinR * sy) * 0.5f;
    const float halfY = (sinR * sx + cosR * sy) * 0.5f;
    auto rect = viewport_.worldViewRect();
    rect.margin = Viewport::kCullMarginWorld;
    return rect.overlaps(halfX, halfY, x, y);
}

void EditorDocument::syncGameCamera(
    const eng::tick::ActiveCamera& active) noexcept
{
    if (active.found()) {
        gameCamera_.posX = active.data.posX;
        gameCamera_.posY = active.data.posY;
        gameCamera_.zoom =
            active.data.zoom > 0.f ? active.data.zoom : 48.f;
        if (active.data.viewHeight > 0.f && viewport_.screenHeight() > 1.f) {
            gameCamera_.zoom = viewport_.screenHeight() / active.data.viewHeight;
        }
        // P4.7.0 B4: rotação da vista (graus autoráveis → radianos).
        gameCamera_.rotation =
            active.data.rotationDeg * (3.14159265358979323846f / 180.f);
        viewport_.setGameCamera(&gameCamera_);
        gameCameraActive_ = true;
    } else {
        viewport_.setGameCamera(nullptr);
        gameCameraActive_ = false;
    }
}

void EditorDocument::gameTouch(int canonicalPhase, std::uint32_t pointerId,
                               float x, float y, float pressure)
{
    eng::input::InputEvent event;
    event.device = eng::input::DeviceKind::Touch;
    event.pointerId = pointerId;
    switch (canonicalPhase) {
    case 0: event.touchPhase = eng::input::TouchPhase::Down; break;
    case 1: event.touchPhase = eng::input::TouchPhase::Move; break;
    case 2: event.touchPhase = eng::input::TouchPhase::Up; break;
    default: event.touchPhase = eng::input::TouchPhase::Cancelled; break;
    }
    event.x = x;
    event.y = y;
    event.pressure = pressure;
    runtimeInput_.queueEvent(event);
}

void EditorDocument::setGameViewportSize(float width, float height) noexcept
{
    runtimeInput_.setScreenSize(width, height);
}

// =============================================================================
// Viewport
// =============================================================================

std::optional<eng::ecs::Entity> EditorDocument::viewportPick(
    float screenX, float screenY, TextureCache* textures) const
{
    const eng::scene::Scene* scene = sceneInFocus();
    if (scene == nullptr) {
        return std::nullopt;
    }
    auto quads = viewport_.buildQuads(*scene, selection_);
    // O hit-test precisa do tamanho DESENHADO: resolve as dimensões em
    // pixels das texturas (decode sem GPU, cacheado pelo host). Sem cache
    // (testes), vale a escala local.
    if (textures != nullptr && assets_ != nullptr) {
        for (auto& quad : quads) {
            if (quad.textureAsset.empty() || quad.textureWidthPx > 0u) {
                continue;
            }
            const auto info =
                textures->imageInfo(*assets_, quad.textureAsset);
            if (info.valid) {
                quad.textureWidthPx =
                    static_cast<std::uint32_t>(info.width);
                quad.textureHeightPx =
                    static_cast<std::uint32_t>(info.height);
            }
        }
    }
    // Raio de toque escala com a densidade (14 px fixos erravam no C33).
    const float tapRadius = std::max(14.f * viewport_.uiScale(), 14.f);
    return viewport_.hitTest(quads, screenX, screenY, tapRadius);
}

std::optional<eng::ecs::Entity> EditorDocument::viewportTap(
    float screenX, float screenY, TextureCache* textures)
{
    // Um toque novo encerra qualquer drag residual do gizmo.
    gizmoDragEnd();
    auto hit = viewportPick(screenX, screenY, textures);
    if (hit.has_value()) {
        selection_ = *hit;
    } else {
        selection_.reset();
    }
    ++selectionRevision_;
    return hit;
}

void EditorDocument::viewportPan(float screenDx, float screenDy) noexcept
{
    viewport_.pan(screenDx, screenDy);
}

void EditorDocument::viewportZoom(float factor, float focusX,
                                  float focusY) noexcept
{
    viewport_.zoomAt(factor, focusX, focusY);
}

Result<void> EditorDocument::moveEntityScreen(eng::ecs::Entity entity,
                                              float screenDx, float screenDy)
{
    // Edit: move na EDIÇÃO (dirty). Play: move no CLONE (debug §8.7) —
    // a escrita é permitida em Play APENAS aqui (mutação de runtime).
    const eng::scene::Scene* scene = sceneInFocus();
    if (scene == nullptr) {
        return makeUnexpected(
            documentError(StatusCode::InvalidState, "sem cena"));
    }
    // Handle de edição → handle do clone (mesma tradução do inspector).
    const eng::ecs::Entity focusEntity = toFocus(entity);
    if (!scene->isNode(focusEntity)) {
        return makeUnexpected(documentError(StatusCode::NotFound,
                                           "entidade obsoleta"));
    }
    // Câmera EM FOCO: em Play sob câmera de jogo o arraste-debug
    // precisa do zoom que o usuário está VENDO, não o do editor.
    const float zoom = viewport_.effectiveCamera().zoom;
    const float worldDx = screenDx / zoom;
    const float worldDy = -screenDy / zoom;
    auto* local =
        const_cast<eng::scene::Scene*>(scene)->localTransform(focusEntity);
    if (local == nullptr) {
        return makeUnexpected(documentError(StatusCode::Internal,
                                           "sem Transform"));
    }
    // Mesmo fixo do gizmo — delta de MUNDO convertido
    // para o espaço LOCAL do pai (filho de pai girado/escalado segue o
    // eixo de TELA, não o eixo local do pai).
    if (mode_ == Mode::Edit) {
        pushHistory("move");
    }
    const std::array<float, 4> inv = parentInverse2D(focusEntity);
    local->position.x += inv[0] * worldDx + inv[1] * worldDy;
    local->position.y += inv[2] * worldDx + inv[3] * worldDy;
    if (mode_ == Mode::Edit) {
        sceneDirty_ = true;
        ++selectionRevision_;  // viewport → Inspector: drag move
    }
    return {};
}

// =============================================================================
// Tradução de handles (bug do clone aleatório — play/stop)
// =============================================================================

eng::ecs::Entity EditorDocument::toFocus(eng::ecs::Entity entity) const noexcept
{
    if (mode_ != Mode::Play || editToRuntime_.empty()) {
        return entity;
    }
    const auto it = editToRuntime_.find(entity);
    return it != editToRuntime_.end() ? it->second : entity;
}

// =============================================================================
// Hierarquia
// =============================================================================

std::vector<EditorDocument::HierarchyNode>
EditorDocument::hierarchySnapshot() const
{
    std::vector<HierarchyNode> nodes;
    const eng::scene::Scene* scene = sceneInFocus();
    if (scene == nullptr) {
        return nodes;
    }
    nodes.reserve(scene->nodeCount());

    const auto visit = [&](auto&& self, eng::ecs::Entity node,
                           int depth) -> void {
        nodes.push_back(HierarchyNode{node, nameOf(*scene, node), depth});
        scene->eachChild(node, [&](eng::ecs::Entity child) {
            self(self, child, depth + 1);
        });
    };

    std::vector<eng::ecs::Entity> roots;
    scene->world().each<eng::scene::Hierarchy>(
        [&](eng::ecs::Entity node, const eng::scene::Hierarchy& hierarchy) {
            if (hierarchy.parent == eng::scene::kNoEntity &&
                scene->isNode(node)) {
                roots.push_back(node);
            }
        });
    std::sort(roots.begin(), roots.end(),
              [](eng::ecs::Entity a, eng::ecs::Entity b) {
                  return a.index < b.index;
              });
    for (const eng::ecs::Entity root : roots) {
        visit(visit, root, 0);
    }
    return nodes;
}

eng::scene::Scene* EditorDocument::sceneInFocus() noexcept
{
    return mode_ == Mode::Play && runtimeScene_.has_value() ? &*runtimeScene_
                                                           : &*scene_;
}

const eng::scene::Scene* EditorDocument::sceneInFocus() const noexcept
{
    return mode_ == Mode::Play && runtimeScene_.has_value() ? &*runtimeScene_
                                                           : &*scene_;
}

std::string EditorDocument::nameOf(const eng::scene::Scene& scene,
                                  eng::ecs::Entity entity)
{
    const auto* name = scene.world().get<eng::scene::Name>(entity);
    if (name == nullptr || name->value.empty()) {
        return "Entity";
    }
    return name->value;
}

// =============================================================================
// Pack/unpack para JNI
// =============================================================================

std::uint64_t EditorDocument::packEntity(eng::ecs::Entity entity) noexcept
{
    // index+1 evita ambiguidade com 0 ("nenhuma") mesmo com geração 0.
    return (static_cast<std::uint64_t>(entity.index) + 1ull) << 32ull |
           static_cast<std::uint64_t>(entity.generation);
}

eng::ecs::Entity EditorDocument::unpackEntity(std::uint64_t packed) noexcept
{
    if (packed == 0ull) {
        return eng::scene::kNoEntity;
    }
    const std::uint32_t index =
        static_cast<std::uint32_t>((packed >> 32ull) - 1ull);
    const std::uint32_t generation = static_cast<std::uint32_t>(packed);
    return eng::ecs::Entity{index, generation};
}

// =============================================================================
// Scripts NI-Script (evolução P0-7, ADR-053)
// =============================================================================

namespace {

/// Nome de script válido: não-vazio, sem '/', sem '..', sem '\0'.
/// A extensão .nis é forçada por scriptCreate; aqui aceita-se o nome
/// COMO ESTÁ (read/write casam com o que listou).
[[nodiscard]] bool isValidScriptName(std::string_view name) noexcept
{
    if (name.empty() || name.size() > 128) {
        return false;
    }
    if (name.find("..") != std::string_view::npos ||
        name.find('/') != std::string_view::npos ||
        name.find('\\') != std::string_view::npos ||
        name.find('\0') != std::string_view::npos) {
        return false;
    }
    return true;
}

/// Garante extensão .nis (adiciona quando ausente).
[[nodiscard]] std::string withNisExtension(std::string_view name)
{
    std::string out(name);
    if (out.size() < 4 || out.compare(out.size() - 4, 4, ".nis") != 0) {
        out += ".nis";
    }
    return out;
}

/// Template canônico de script novo: compila LIMPO (o teste do editor
/// PROVA via scriptCompile), sintaxe idêntica aos casos da FASE 11.
[[nodiscard]] std::string defaultScriptTemplate()
{
    return "# Script NI-Script do G.ONI\n"
           "# Linguagem: docs/ni-script/ (indentacao por blocos, 'stop' fecha)\n"
           "\n"
           "add &BL\n"
           "\n"
           "var speed: float = 2.0\n"
           "var ticks: int = 0\n"
           "\n"
           "up start:\n"
           "    # roda uma vez ao entrar em PLAY\n"
           "stop\n"
           "\n"
           "up update:\n"
           "    # roda por frame — exemplo: move a entidade no eixo X\n"
           "    var me = self()\n"
           "    me.position.x = me.position.x + speed\n"
           "    ticks = ticks + 1\n"
           "stop\n"
           "\n"
           "up destroy:\n"
           "    # limpeza ao sair do PLAY\n"
           "stop\n";
}

}  // namespace

Result<std::vector<std::string>> EditorDocument::scriptList() const
{
    if (assets_ == nullptr) {
        return makeUnexpected(documentError(
            StatusCode::InvalidState, "nenhum projeto aberto"));
    }
    auto listed = assets_->list("scripts");
    if (listed.isError()) {
        return makeUnexpected(listed.error());
    }
    std::vector<std::string> names;
    names.reserve(listed.value().size());
    for (const auto& entry : listed.value()) {
        names.push_back(entry.name);
    }
    return names;
}

Result<std::string> EditorDocument::scriptRead(std::string_view name) const
{
    if (!isValidScriptName(name)) {
        return makeUnexpected(documentError(StatusCode::InvalidArgument,
                                            "nome de script invalido"));
    }
    if (assets_ == nullptr) {
        return makeUnexpected(documentError(StatusCode::InvalidState,
                                            "nenhum projeto aberto"));
    }
    auto bytes = assets_->read("scripts", name);
    if (bytes.isError()) {
        return makeUnexpected(bytes.error());
    }
    std::string text;
    text.reserve(bytes.value().size());
    for (const std::byte b : bytes.value()) {
        text.push_back(static_cast<char>(b));
    }
    return text;
}

Result<void> EditorDocument::scriptWrite(std::string_view name,
                                         std::string_view content)
{
    if (!isValidScriptName(name)) {
        return makeUnexpected(documentError(StatusCode::InvalidArgument,
                                            "nome de script invalido"));
    }
    if (assets_ == nullptr || !project_.has_value()) {
        return makeUnexpected(documentError(StatusCode::InvalidState,
                                            "nenhum projeto aberto"));
    }
    const eng::fs::Path path = project_->paths().assetsRoot() /
                               eng::fs::Path{"scripts"} /
                               eng::fs::Path{std::string(name)};
    if (path.isAbsolute()) {
        return makeUnexpected(documentError(StatusCode::InvalidArgument,
                                            "caminho absoluto proibido"));
    }
    auto existed = fs_->exists(path);
    if (existed.isError()) {
        return makeUnexpected(existed.error());
    }
    auto written = fs_->writeAllText(path, std::string(content));
    if (written.isError()) {
        return makeUnexpected(written.error());
    }
    if (!existed.value()) {
        // Arquivo NOVO: cataloga no registry (upsert + persist).
        auto registered = assets_->registerExisting("scripts", name);
        if (registered.isError()) {
            // O arquivo existe mas o meta não persistiu — erro real
            // (o browser não listaria o script).
            return makeUnexpected(registered.error());
        }
    }
    syncLinkedScripts(name, content);
    return {};
}

void EditorDocument::syncLinkedScripts(std::string_view name,
                                       std::string_view content)
{
    if (mode_ != Mode::Edit || !scene_.has_value()) {
        return;
    }
    bool touched = false;
    scene_->world().each<eng::editor::NiScriptComponent>(
        [&](eng::ecs::Entity, eng::editor::NiScriptComponent& script) {
            if (script.scriptAsset == name && script.source != content) {
                script.source = std::string(content);
                touched = true;
            }
        });
    if (touched) {
        sceneDirty_ = true;
        ++selectionRevision_;
    }
}

Result<void> EditorDocument::scriptCreate(std::string_view rawName)
{
    if (!isValidScriptName(rawName)) {
        return makeUnexpected(documentError(
            StatusCode::InvalidArgument,
            "nome de script invalido (vazio/caracteres proibidos)"));
    }
    const std::string name = withNisExtension(rawName);
    if (assets_ == nullptr || !project_.has_value()) {
        return makeUnexpected(documentError(StatusCode::InvalidState,
                                            "nenhum projeto aberto"));
    }
    auto listed = scriptList();
    if (listed.isError()) {
        return makeUnexpected(listed.error());
    }
    for (const auto& existing : listed.value()) {
        if (existing == name) {
            return makeUnexpected(documentError(
                StatusCode::AlreadyExists,
                "script '" + name + "' ja existe"));
        }
    }
    return scriptWrite(name, defaultScriptTemplate());
}

Result<void> EditorDocument::scriptDelete(std::string_view name)
{
    if (!isValidScriptName(name)) {
        return makeUnexpected(documentError(StatusCode::InvalidArgument,
                                            "nome de script invalido"));
    }
    if (assets_ == nullptr) {
        return makeUnexpected(documentError(StatusCode::InvalidState,
                                            "nenhum projeto aberto"));
    }
    // AssetBrowser::remove apaga arquivo + meta e persiste.
    return assets_->remove("scripts", name);
}

Result<EditorDocument::ScriptCheck> EditorDocument::scriptCompile(
    std::string_view source) const
{
    // MESMA tabela visível ao runtime de Play (NiRuntime::start) — o que
    // valida aqui é o que o jogo vai compilar lá.
    eng::ni::NiNativeTable natives;
    NiRuntime::registerNatives(natives);

    ScriptCheck check;
    std::vector<eng::ni::NiDiag> diags;
    const eng::ni::CompileOptions options{&natives};
    auto program = eng::ni::compile(source, options, &diags);
    check.ok = static_cast<bool>(program);
    check.diags.reserve(diags.size());
    for (const auto& diag : diags) {
        check.diags.push_back(
            ScriptDiag{diag.line, diag.col, diag.message});
    }
    return check;
}

Result<void> EditorDocument::scriptAssign(eng::ecs::Entity entity,
                                           std::string_view name)
{
    auto guard = requireEditMode();
    if (guard.isError()) {
        return makeUnexpected(guard.error());
    }
    if (!scene_->isNode(entity)) {
        return makeUnexpected(documentError(StatusCode::NotFound,
                                            "entidade obsoleta"));
    }
    auto content = scriptRead(name);
    if (content.isError()) {
        return makeUnexpected(content.error());
    }
    pushHistory("attach");
    // Caminho pelo catálogo ÚNICO (mesma via do Inspector): componente
    // presente → escreve source; ausente → adiciona default e escreve.
    if (!scene_->world().has<eng::editor::NiScriptComponent>(entity)) {
        auto added = Inspector::addComponent(
            *scene_, entity, "eng::editor::NiScriptComponent");
        if (added.isError()) {
            return makeUnexpected(added.error());
        }
    }
    auto written = Inspector::setField(
        *scene_, entity, "eng::editor::NiScriptComponent", "source",
        content.value());
    if (written.isError()) {
        return makeUnexpected(written.error());
    }
    if (auto* script =
            scene_->world().get<eng::editor::NiScriptComponent>(entity)) {
        script->scriptAsset = std::string(name);
    }
    sceneDirty_ = true;
    ++selectionRevision_;
    ENG_INFO("script anexado: {} ({} bytes) → entidade {}", name,
             content.value().size(), entity.index);
    return {};
}

// =============================================================================
// Animação authorável (P2 §8)
// =============================================================================

namespace {

constexpr std::string_view kAnimCategory = "animations";

// --- materiais (P3 §3) -------------------------------------------------------
constexpr std::string_view kMaterialCategory = "materials";
constexpr std::string_view kMaterialExt = ".mat.json";

/// Nome de asset de material válido (mesma política de scripts/animações).
[[nodiscard]] bool isValidMaterialName(std::string_view rawName)
{
    if (rawName.empty() || rawName.size() > 96) {
        return false;
    }
    for (const char c : rawName) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                        c == ' ' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return rawName != "." && rawName != "..";
}

/// Força a extensão .mat.json (a UI envia o nome cru).
[[nodiscard]] std::string withMaterialExtension(std::string_view name)
{
    std::string out{name};
    if (out.size() < kMaterialExt.size() ||
        out.compare(out.size() - kMaterialExt.size(), kMaterialExt.size(),
                    kMaterialExt) != 0) {
        out += kMaterialExt;
    }
    return out;
}

/// Nome de asset de animação válido (mesma política de scripts: sem
/// path/nul; a extensão é forçada por animationCreate).
[[nodiscard]] bool isValidAnimName(std::string_view name) noexcept
{
    if (name.empty() || name.size() > 128) {
        return false;
    }
    return name.find("..") == std::string_view::npos &&
           name.find('/') == std::string_view::npos &&
           name.find('\\') == std::string_view::npos &&
           name.find('\0') == std::string_view::npos;
}

/// Garante a extensão .anim.json (adiciona quando ausente).
[[nodiscard]] std::string withAnimExtension(std::string_view name)
{
    constexpr std::string_view kExt = ".anim.json";
    std::string out{name};
    if (out.size() < kExt.size() ||
        out.compare(out.size() - kExt.size(), kExt.size(), kExt) != 0) {
        out += kExt;
    }
    return out;
}

}  // namespace

Result<std::vector<EditorDocument::AnimSummary>>
EditorDocument::animationList() const
{
    auto listed = assets_->list(kAnimCategory);
    if (listed.isError()) {
        return makeUnexpected(listed.error());
    }
    std::vector<AnimSummary> out;
    out.reserve(listed.value().size());
    for (const auto& entry : listed.value()) {
        AnimSummary summary;
        summary.name = entry.name;
        auto content = animationRead(entry.name);
        if (content.isError()) {
            summary.clip = entry.name;  // ilegível: lista honesta com erro
            continue;
        }
        std::vector<AnimDiag> diags;
        auto decoded = animationDecode(content.value(), &diags);
        if (decoded.isError()) {
            summary.clip = entry.name;
            continue;
        }
        summary.clip = decoded.value().clip.name;
        summary.duration = decoded.value().clip.duration();
        summary.frames = decoded.value().clip.frames.size();
        summary.keys = decoded.value().clip.position.size() +
                       decoded.value().clip.rotation.size() +
                       decoded.value().clip.scale.size();
        summary.loop = decoded.value().meta.loop;
        out.push_back(std::move(summary));
    }
    return out;
}

Result<std::string> EditorDocument::animationRead(
    std::string_view name) const
{
    if (!isValidAnimName(name)) {
        return makeUnexpected(documentError(StatusCode::InvalidArgument,
                                            "nome de animação inválido"));
    }
    if (assets_ == nullptr) {
        return makeUnexpected(documentError(StatusCode::InvalidState,
                                            "nenhum projeto aberto"));
    }
    auto bytes = assets_->read(kAnimCategory, name);
    if (bytes.isError()) {
        return makeUnexpected(bytes.error());
    }
    std::string text;
    text.reserve(bytes.value().size());
    for (const std::byte b : bytes.value()) {
        text.push_back(static_cast<char>(b));
    }
    return text;
}

Result<void> EditorDocument::animationWrite(std::string_view name,
                                            std::string_view json)
{
    if (!isValidAnimName(name)) {
        return makeUnexpected(documentError(StatusCode::InvalidArgument,
                                            "nome de animação inválido"));
    }
    if (assets_ == nullptr || !project_.has_value()) {
        return makeUnexpected(documentError(StatusCode::InvalidState,
                                            "nenhum projeto aberto"));
    }
    // Valida ANTES de gravar: lixo não entra no projeto.
    std::vector<AnimDiag> diags;
    auto decoded = animationDecode(json, &diags);
    if (decoded.isError()) {
        return makeUnexpected(documentError(
            StatusCode::InvalidArgument,
            "JSON rejeitado: " + decoded.error().message));
    }
    const eng::fs::Path path = project_->paths().assetsRoot() /
                               eng::fs::Path{std::string(kAnimCategory)} /
                               eng::fs::Path{std::string(name)};
    if (path.isAbsolute()) {
        return makeUnexpected(documentError(StatusCode::InvalidArgument,
                                            "caminho absoluto proibido"));
    }
    auto existed = fs_->exists(path);
    if (existed.isError()) {
        return makeUnexpected(existed.error());
    }
    auto written = fs_->writeAllText(path, std::string(json));
    if (written.isError()) {
        return makeUnexpected(written.error());
    }
    if (!existed.value()) {
        auto registered =
            assets_->registerExisting(kAnimCategory, name);
        if (registered.isError()) {
            return makeUnexpected(registered.error());
        }
    }
    return {};
}

Result<void> EditorDocument::animationCreate(std::string_view rawName)
{
    if (!isValidAnimName(rawName)) {
        return makeUnexpected(documentError(
            StatusCode::InvalidArgument,
            "nome de animação inválido (vazio/caracteres proibidos)"));
    }
    const std::string name = withAnimExtension(rawName);
    auto existing = assets_->list(kAnimCategory);
    if (existing.isError()) {
        return makeUnexpected(existing.error());
    }
    for (const auto& entry : existing.value()) {
        if (entry.name == name) {
            return makeUnexpected(
                documentError(StatusCode::AlreadyExists,
                              "animação '" + name + "' já existe"));
        }
    }
    // Template de FLIPBOOK puro: frames começam em t=0 (o authoring
    // adiciona texturas via animationAddFrame; TRS fica desligado —
    // defaults inteligentes do assign). frameHold = 1/fps (o slot).
    const std::string clipBase(rawName);
    const std::string json = "{\n"
                             "  \"name\": \"" + clipBase + "\",\n"
                             "  \"fps\": 8,\n"
                             "  \"loop\": true,\n"
                             "  \"frameHold\": 0.125,\n"
                             "  \"frames\": []\n"
                             "}\n";
    return animationWrite(name, json);
}

Result<void> EditorDocument::animationDelete(std::string_view name)
{
    if (!isValidAnimName(name)) {
        return makeUnexpected(documentError(StatusCode::InvalidArgument,
                                            "nome de animação inválido"));
    }
    if (assets_ == nullptr) {
        return makeUnexpected(documentError(StatusCode::InvalidState,
                                            "nenhum projeto aberto"));
    }
    return assets_->remove(kAnimCategory, name);
}

// =============================================================================
// Materiais (P3 §3) — assets/materials/<nome>.mat.json
// =============================================================================

Result<std::vector<EditorDocument::MaterialSummary>>
EditorDocument::materialList() const
{
    if (assets_ == nullptr) {
        return makeUnexpected(documentError(StatusCode::InvalidState,
                                            "nenhum projeto aberto"));
    }
    auto listed = assets_->list(kMaterialCategory);
    if (listed.isError()) {
        return makeUnexpected(listed.error());
    }
    std::vector<MaterialSummary> out;
    out.reserve(listed.value().size());
    for (const auto& entry : listed.value()) {
        auto content = materialRead(entry.name);
        if (content.isError()) {
            continue;  // ilegível: NÃO lista (a UI só oferece o válido)
        }
        auto decoded = eng::render::materialDecode(content.value());
        if (decoded.isError()) {
            continue;
        }
        MaterialSummary summary;
        summary.name = entry.name;
        summary.shader = decoded.value().material.shader;
        summary.tintR = decoded.value().material.tintR;
        summary.tintG = decoded.value().material.tintG;
        summary.tintB = decoded.value().material.tintB;
        summary.tintA = decoded.value().material.tintA;
        out.push_back(std::move(summary));
    }
    return out;
}

Result<std::string> EditorDocument::materialRead(
    std::string_view name) const
{
    if (!isValidMaterialName(name)) {
        return makeUnexpected(documentError(StatusCode::InvalidArgument,
                                            "nome de material inválido"));
    }
    if (assets_ == nullptr) {
        return makeUnexpected(documentError(StatusCode::InvalidState,
                                            "nenhum projeto aberto"));
    }
    auto bytes = assets_->read(kMaterialCategory, name);
    if (bytes.isError()) {
        return makeUnexpected(bytes.error());
    }
    std::string text;
    text.reserve(bytes.value().size());
    for (const std::byte b : bytes.value()) {
        text.push_back(static_cast<char>(b));
    }
    return text;
}

Result<void> EditorDocument::materialWrite(std::string_view name,
                                            std::string_view json)
{
    if (!isValidMaterialName(name)) {
        return makeUnexpected(documentError(StatusCode::InvalidArgument,
                                            "nome de material inválido"));
    }
    if (assets_ == nullptr || !project_.has_value()) {
        return makeUnexpected(documentError(StatusCode::InvalidState,
                                            "nenhum projeto aberto"));
    }
    // Valida ANTES de gravar: lixo não entra no projeto.
    auto decoded = eng::render::materialDecode(json);
    if (decoded.isError()) {
        return makeUnexpected(documentError(
            StatusCode::InvalidArgument,
            "JSON rejeitado: " + decoded.error().message));
    }
    const eng::fs::Path path =
        project_->paths().assetsRoot() /
        eng::fs::Path{std::string(kMaterialCategory)} /
        eng::fs::Path{std::string(name)};
    if (path.isAbsolute()) {
        return makeUnexpected(documentError(StatusCode::InvalidArgument,
                                            "caminho absoluto proibido"));
    }
    auto existed = fs_->exists(path);
    if (existed.isError()) {
        return makeUnexpected(existed.error());
    }
    auto written = fs_->writeAllText(path, std::string(json));
    if (written.isError()) {
        return makeUnexpected(written.error());
    }
    if (!existed.value()) {
        auto registered = assets_->registerExisting(kMaterialCategory, name);
        if (registered.isError()) {
            return makeUnexpected(registered.error());
        }
    }
    // Cache sai (shader/tint podem ter mudado — o próximo resolve relê).
    materialCache_.erase(std::string(name));
    return {};
}

Result<void> EditorDocument::materialCreate(std::string_view rawName)
{
    if (!isValidMaterialName(rawName)) {
        return makeUnexpected(documentError(
            StatusCode::InvalidArgument,
            "nome de material inválido (vazio/caracteres proibidos)"));
    }
    if (assets_ == nullptr) {
        return makeUnexpected(documentError(StatusCode::InvalidState,
                                            "nenhum projeto aberto"));
    }
    const std::string name = withMaterialExtension(rawName);
    auto existing = assets_->list(kMaterialCategory);
    if (existing.isError()) {
        return makeUnexpected(existing.error());
    }
    for (const auto& entry : existing.value()) {
        if (entry.name == name) {
            return makeUnexpected(documentError(
                StatusCode::AlreadyExists,
                "material '" + name + "' já existe"));
        }
    }
    eng::render::MaterialAsset asset;
    asset.name = rawName;
    asset.material.shader = eng::render::kShaderLit;
    auto encoded = eng::render::materialEncode(asset);
    if (encoded.isError()) {
        return makeUnexpected(encoded.error());
    }
    return materialWrite(name, encoded.value());
}

Result<void> EditorDocument::materialDelete(std::string_view name)
{
    if (!isValidMaterialName(name)) {
        return makeUnexpected(documentError(StatusCode::InvalidArgument,
                                            "nome de material inválido"));
    }
    if (assets_ == nullptr) {
        return makeUnexpected(documentError(StatusCode::InvalidState,
                                            "nenhum projeto aberto"));
    }
    materialCache_.erase(std::string(name));
    return assets_->remove(kMaterialCategory, name);
}

Result<std::vector<std::string>> EditorDocument::materialNames() const
{
    auto listed = materialList();
    if (listed.isError()) {
        return makeUnexpected(listed.error());
    }
    std::vector<std::string> names;
    names.reserve(listed.value().size());
    for (const auto& summary : listed.value()) {
        names.push_back(summary.name);
    }
    return names;
}

void EditorDocument::resolveMaterials(std::vector<EntityQuad>& quads) const
{
    if (assets_ == nullptr) {
        return;  // sem projeto: default já está nos quads ("lit" neutro)
    }
    for (EntityQuad& quad : quads) {
        if (!quad.isSprite || quad.materialAsset.empty()) {
            continue;  // default: materialShader="lit", tint intactos
        }
        const std::string name = withMaterialExtension(quad.materialAsset);
        auto cached = materialCache_.find(name);
        if (cached == materialCache_.end()) {
            auto content = materialRead(name);
            if (content.isError()) {
                ENG_WARN("material '{}' ilegível — usando default lit "
                         "neutro",
                         name);
                materialCache_[name] = eng::render::SpriteMaterial{};
            } else {
                auto decoded = eng::render::materialDecode(content.value());
                if (decoded.isError()) {
                    ENG_WARN("material '{}' inválido ({}) — usando default "
                             "lit neutro",
                             name, decoded.error().message);
                    materialCache_[name] = eng::render::SpriteMaterial{};
                } else {
                    materialCache_[name] = decoded.value().material;
                }
            }
            cached = materialCache_.find(name);
        }
        const eng::render::SpriteMaterial& material = cached->second;
        quad.materialShader = material.shader;
        quad.tintR *= material.tintR;
        quad.tintG *= material.tintG;
        quad.tintB *= material.tintB;
        quad.tintA *= material.tintA;
    }
}

Result<void> EditorDocument::animationAssign(eng::ecs::Entity entity,
                                              std::string_view name)
{
    auto guard = requireEditMode();
    if (guard.isError()) {
        return makeUnexpected(guard.error());
    }
    if (!scene_->isNode(entity)) {
        return makeUnexpected(documentError(StatusCode::NotFound,
                                           "entidade obsoleta"));
    }
    // O nome aceito aqui é o ARQUIVO; o CLIP é o nome interno (fonte:
    // o JSON — "o que o banco vai indexar").
    auto content = animationRead(withAnimExtension(name));
    if (content.isError()) {
        return makeUnexpected(content.error());
    }
    auto decoded = animationDecode(content.value());
    if (decoded.isError()) {
        return makeUnexpected(decoded.error());
    }
    const std::string clipName = decoded.value().clip.name;

    pushHistory("attach");
    // Componente Animator (adiciona default quando ausente — §14).
    if (!scene_->world().has<eng::animation::Animator>(entity)) {
        auto added = Inspector::addComponent(
            *scene_, entity, "eng::animation::Animator");
        if (added.isError()) {
            return makeUnexpected(added.error());
        }
    }
    // Auto-criação SEGURA (P2 §14): clip com FRAMES precisa de SpriteData
    // para o autor VER o flipbook; default é placeholder inofensivo.
    if (!decoded.value().clip.frames.empty() &&
        !scene_->world().has<eng::editor::SpriteData>(entity)) {
        auto sprite = Inspector::addComponent(
            *scene_, entity, "eng::editor::SpriteData");
        if (sprite.isError()) {
            return makeUnexpected(sprite.error());
        }
    }
    auto* animator = scene_->world().get<eng::animation::Animator>(entity);
    animator->clip = clipName;
    animator->time = 0.f;
    animator->loop = decoded.value().meta.loop;
    animator->playing = false;  // o Play inicia (autoplay é do runtime)
    animator->previousClip.clear();
    animator->blendRemaining = 0.f;
    // Defaults INTELIGENTES: track vazia → apply* DESLIGADO (clip de
    // flipbook puro não zera o Transform do autor; o TRS fica dele).
    animator->applyPosition = !decoded.value().clip.position.empty();
    animator->applyRotation = !decoded.value().clip.rotation.empty();
    animator->applyScale = !decoded.value().clip.scale.empty();
    animator->applySprite = !decoded.value().clip.frames.empty();
    sceneDirty_ = true;
    ++selectionRevision_;
    ENG_INFO("animação anexada: {} (clip '{}', {} frames) → entidade {}",
             name, clipName, decoded.value().clip.frames.size(),
             entity.index);
    return {};
}

Result<float> EditorDocument::animationAddFrame(
    std::string_view name, std::string_view textureAsset)
{
    if (!isValidAnimName(name) || textureAsset.empty()) {
        return makeUnexpected(documentError(StatusCode::InvalidArgument,
                                            "nome/textura inválidos"));
    }
    if (assets_ == nullptr) {
        return makeUnexpected(documentError(StatusCode::InvalidState,
                                            "nenhum projeto aberto"));
    }
    // A textura precisa EXISTIR no projeto (authoring real — sem
    // referências quebradas silenciosas).
    auto textures = assets_->list("textures");
    if (textures.isError()) {
        return makeUnexpected(textures.error());
    }
    bool found = false;
    for (const auto& entry : textures.value()) {
        if (entry.name == textureAsset) {
            found = true;
            break;
        }
    }
    if (!found) {
        return makeUnexpected(documentError(
            StatusCode::NotFound,
            "textura '" + std::string(textureAsset) +
                "' não está no projeto (importe-a primeiro)"));
    }

    const std::string fileName = withAnimExtension(name);
    auto content = animationRead(fileName);
    if (content.isError()) {
        return makeUnexpected(content.error());
    }
    auto decoded = animationDecode(content.value());
    if (decoded.isError()) {
        return makeUnexpected(decoded.error());
    }
    // Cadência de flipbook: primeiro frame em t=0; o seguinte uma cadeia
    // (1/fps) depois do último — o duration estende pelo frameHold.
    const float step =
        decoded.value().meta.fps > 0.f ? 1.f / decoded.value().meta.fps : 0.125f;
    const float when = decoded.value().clip.frames.empty()
                           ? 0.f
                           : decoded.value().clip.frames.back().time + step;

    // Reconstrói o JSON acrescentando o frame (codec canônico — o dump
    // é estável, round-trip testado).
    eng::animation::SpriteFrameKey key;
    key.time = when;
    key.textureAsset = std::string(textureAsset);
    decoded.value().clip.frames.push_back(std::move(key));
    auto encoded = animationEncode(decoded.value());
    if (encoded.isError()) {
        return makeUnexpected(encoded.error());
    }
    auto written = animationWrite(fileName, encoded.value());
    if (written.isError()) {
        return makeUnexpected(written.error());
    }
    return when;
}

Result<void> EditorDocument::animationSetMeta(std::string_view name,
                                               bool loop, float fps)
{
    if (!isValidAnimName(name)) {
        return makeUnexpected(documentError(StatusCode::InvalidArgument,
                                            "nome de animação inválido"));
    }
    if (!std::isfinite(fps) || fps <= 0.f || fps > 120.f) {
        return makeUnexpected(
            documentError(StatusCode::InvalidArgument,
                          "fps deve estar em (0, 120]"));
    }
    const std::string fileName = withAnimExtension(name);
    auto content = animationRead(fileName);
    if (content.isError()) {
        return makeUnexpected(content.error());
    }
    auto decoded = animationDecode(content.value());
    if (decoded.isError()) {
        return makeUnexpected(decoded.error());
    }
    decoded.value().meta.loop = loop;
    decoded.value().meta.fps = fps;
    auto encoded = animationEncode(decoded.value());
    if (encoded.isError()) {
        return makeUnexpected(encoded.error());
    }
    return animationWrite(fileName, encoded.value());
}

// --- P4.6: autoraria de keys TRS (timeline v1) --------------------

namespace {

/// Key TRS normalizado (rotation em GRAUS — convenção do autor).
struct TrsKey {
    float time;
    float x;
    float y;
    float z;
};

[[nodiscard]] bool validTrsTrackName(const std::string& track)
{
    return track == "position" || track == "rotation" ||
           track == "scale";
}

/// Lê a track pedida como keys normalizados. false = track inválida.
[[nodiscard]] bool readTrsTrack(
    const eng::animation::AnimationClip& clip, const std::string& track,
    std::vector<TrsKey>& out)
{
    if (track == "position") {
        for (const auto& k : clip.position) {
            out.push_back({k.time, k.value.x, k.value.y, k.value.z});
        }
        return true;
    }
    if (track == "rotation") {
        for (const auto& k : clip.rotation) {
            const eng::math::Vec3 d = degreesFromQuat(k.value);
            out.push_back({k.time, d.x, d.y, d.z});
        }
        return true;
    }
    if (track == "scale") {
        for (const auto& k : clip.scale) {
            out.push_back({k.time, k.value.x, k.value.y, k.value.z});
        }
        return true;
    }
    return false;
}

/// Escreve de volta (rotation: graus → quat) e ORDENA por tempo (estável —
/// a amostragem do AnimationSystem assume keys ordenados).
void writeTrsTrack(eng::animation::AnimationClip& clip,
                   const std::string& track, std::vector<TrsKey> keys)
{
    std::stable_sort(keys.begin(), keys.end(),
                     [](const TrsKey& a, const TrsKey& b) {
                         return a.time < b.time;
                     });
    if (track == "position") {
        clip.position.clear();
        for (const auto& k : keys) {
            clip.position.push_back(
                eng::animation::PositionKey{k.time,
                                            eng::math::Vec3{k.x, k.y, k.z}});
        }
    } else if (track == "rotation") {
        clip.rotation.clear();
        for (const auto& k : keys) {
            clip.rotation.push_back(
                eng::animation::RotationKey{
                    k.time, quatFromDegrees(
                                eng::math::Vec3{k.x, k.y, k.z})});
        }
    } else if (track == "scale") {
        clip.scale.clear();
        for (const auto& k : keys) {
            clip.scale.push_back(
                eng::animation::ScaleKey{k.time,
                                         eng::math::Vec3{k.x, k.y, k.z}});
        }
    }
}

/// Corpo comum: valida → lê asset → decodifica. `asset` sai decodificado.
[[nodiscard]] eng::core::Result<eng::editor::AnimationAsset>
loadAnimAssetForKeys(const EditorDocument& doc, std::string_view name,
                     const std::string& track)
{
    if (!isValidAnimName(name)) {
        return makeUnexpected(documentError(
            StatusCode::InvalidArgument, "nome de animação inválido"));
    }
    if (!validTrsTrackName(track)) {
        return makeUnexpected(documentError(
            StatusCode::InvalidArgument,
            "track inválida (use position|rotation|scale)"));
    }
    const std::string fileName = withAnimExtension(name);
    auto content = doc.animationRead(fileName);
    if (content.isError()) {
        return makeUnexpected(content.error());
    }
    return animationDecode(content.value());
}

}  // namespace

Result<void> EditorDocument::animationAddKey(std::string_view name,
                                             std::string_view track,
                                             float time, float x, float y,
                                             float z)
{
    if (!std::isfinite(time) || time < 0.f || !std::isfinite(x) ||
        !std::isfinite(y) || !std::isfinite(z)) {
        return makeUnexpected(documentError(
            StatusCode::InvalidArgument,
            "key TRS: tempo/valores devem ser finitos (tempo >= 0)"));
    }
    auto loaded = loadAnimAssetForKeys(*this, name, std::string(track));
    if (loaded.isError()) {
        return makeUnexpected(loaded.error());
    }
    std::vector<TrsKey> keys;
    const std::string trackStr{track};
    (void)readTrsTrack(loaded.value().clip, trackStr, keys);
    // Key no MESMO tempo (ε 1e-4) SUBSTITUI — gravar de novo = atualizar.
    bool replaced = false;
    for (TrsKey& k : keys) {
        if (std::abs(k.time - time) <= 1e-4f) {
            k = TrsKey{time, x, y, z};
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        keys.push_back(TrsKey{time, x, y, z});
    }
    writeTrsTrack(loaded.value().clip, trackStr, std::move(keys));
    auto encoded = animationEncode(loaded.value());
    if (encoded.isError()) {
        return makeUnexpected(encoded.error());
    }
    return animationWrite(withAnimExtension(name), encoded.value());
}

Result<std::string> EditorDocument::animationKeyList(
    std::string_view name, std::string_view track) const
{
    auto loaded = loadAnimAssetForKeys(*this, name, std::string(track));
    if (loaded.isError()) {
        return makeUnexpected(loaded.error());
    }
    std::vector<TrsKey> keys;
    const std::string trackStr{track};
    (void)readTrsTrack(loaded.value().clip, trackStr, keys);
    std::stable_sort(keys.begin(), keys.end(),
                     [](const TrsKey& a, const TrsKey& b) {
                         return a.time < b.time;
                     });
    std::string tsv;
    char line[128];
    for (std::size_t i = 0; i < keys.size(); ++i) {
        std::snprintf(line, sizeof(line), "%zu\t%.4g\t%.4g\t%.4g\t%.4g\n", i,
                      static_cast<double>(keys[i].time),
                      static_cast<double>(keys[i].x),
                      static_cast<double>(keys[i].y),
                      static_cast<double>(keys[i].z));
        tsv += line;
    }
    if (!tsv.empty()) {
        tsv.pop_back();
    }
    return tsv;
}

Result<void> EditorDocument::animationKeySet(std::string_view name,
                                             std::string_view track,
                                             std::size_t index, float time,
                                             float x, float y, float z)
{
    if (!std::isfinite(time) || time < 0.f || !std::isfinite(x) ||
        !std::isfinite(y) || !std::isfinite(z)) {
        return makeUnexpected(documentError(
            StatusCode::InvalidArgument,
            "key TRS: tempo/valores devem ser finitos (tempo >= 0)"));
    }
    auto loaded = loadAnimAssetForKeys(*this, name, std::string(track));
    if (loaded.isError()) {
        return makeUnexpected(loaded.error());
    }
    std::vector<TrsKey> keys;
    const std::string trackStr{track};
    (void)readTrsTrack(loaded.value().clip, trackStr, keys);
    if (index >= keys.size()) {
        return makeUnexpected(documentError(
            StatusCode::NotFound, "key " + std::to_string(index) +
                                      " não existe na track '" + trackStr +
                                      "'"));
    }
    keys[index] = TrsKey{time, x, y, z};
    writeTrsTrack(loaded.value().clip, trackStr, std::move(keys));
    auto encoded = animationEncode(loaded.value());
    if (encoded.isError()) {
        return makeUnexpected(encoded.error());
    }
    return animationWrite(withAnimExtension(name), encoded.value());
}

Result<void> EditorDocument::animationKeyDelete(std::string_view name,
                                                std::string_view track,
                                                std::size_t index)
{
    auto loaded = loadAnimAssetForKeys(*this, name, std::string(track));
    if (loaded.isError()) {
        return makeUnexpected(loaded.error());
    }
    std::vector<TrsKey> keys;
    const std::string trackStr{track};
    (void)readTrsTrack(loaded.value().clip, trackStr, keys);
    if (index >= keys.size()) {
        return makeUnexpected(documentError(
            StatusCode::NotFound, "key " + std::to_string(index) +
                                      " não existe na track '" + trackStr +
                                      "'"));
    }
    keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(index));
    writeTrsTrack(loaded.value().clip, trackStr, std::move(keys));
    auto encoded = animationEncode(loaded.value());
    if (encoded.isError()) {
        return makeUnexpected(encoded.error());
    }
    return animationWrite(withAnimExtension(name), encoded.value());
}

Result<void> EditorDocument::previewStart(eng::ecs::Entity entity,
                                          std::string_view clipName)
{
    auto guard = requireEditMode();
    if (guard.isError()) {
        return makeUnexpected(guard.error());
    }
    if (!scene_->isNode(entity)) {
        return makeUnexpected(documentError(StatusCode::NotFound,
                                           "entidade obsoleta"));
    }
    // Resolve o clip por ARQUIVO (nome do asset) ou nome INTERNO do clip.
    auto listed = animationList();
    if (listed.isError()) {
        return makeUnexpected(listed.error());
    }
    std::string fileName;
    for (const auto& summary : listed.value()) {
        if (summary.name == clipName || summary.clip == clipName) {
            fileName = summary.name;
            break;
        }
    }
    if (fileName.empty()) {
        return makeUnexpected(
            documentError(StatusCode::NotFound,
                          "animação '" + std::string(clipName) +
                              "' não encontrada"));
    }
    auto content = animationRead(fileName);
    if (content.isError()) {
        return makeUnexpected(content.error());
    }
    auto decoded = animationDecode(content.value());
    if (decoded.isError()) {
        return makeUnexpected(decoded.error());
    }
    auto original = transform(entity);
    if (original.isError()) {
        return makeUnexpected(original.error());
    }
    preview_ = PreviewState{entity, decoded.value().clip.name, 0.f,
                            decoded.value().meta.loop, original.value()};
    return {};
}

void EditorDocument::previewTick(float deltaSeconds) noexcept
{
    if (!preview_.has_value() || mode_ != Mode::Edit) {
        return;
    }
    PreviewState& state = *preview_;
    const eng::animation::AnimationClip* clip =
        runtimeAnimations_.find(state.clip);
    // O banco em EDIT carrega na hora do preview (fonte: assets).
    if (clip == nullptr) {
        if (!assets_) {
            return;
        }
        auto listed = assets_->list(kAnimCategory);
        if (listed.isError()) {
            return;
        }
        for (const auto& entry : listed.value()) {
            auto content = animationRead(entry.name);
            if (content.isError()) {
                continue;
            }
            auto decoded = animationDecode(content.value());
            if (decoded.isError()) {
                continue;
            }
            runtimeAnimations_.add(std::move(decoded.value().clip));
        }
        clip = runtimeAnimations_.find(state.clip);
    }
    if (clip == nullptr || clip->duration() <= 0.f) {
        return;
    }
    state.time += deltaSeconds;
    if (state.loop) {
        state.time = std::fmod(state.time, clip->duration());
    } else if (state.time >= clip->duration()) {
        state.time = clip->duration();
    }
    // MESMO sampler do runtime (AnimationSystem::sample — fonte única).
    const auto pose =
        eng::animation::AnimationSystem::sample(*clip, state.time);
    eng::editor::TransformDesc desc = state.original;
    desc.position = pose.position;
    desc.rotationDegrees = degreesFromQuat(pose.rotation);
    desc.scale = pose.scale;
    (void)setTransform(state.entity, desc);  // dirty (honesto: preview edita)
    // Frames no sprite da ENTIDADE em edição (mesma aplicação do Play).
    applyAnimatorFrames(*scene_);
}

void EditorDocument::previewStop() noexcept
{
    if (!preview_.has_value()) {
        return;
    }
    // Restaura o TRANSFORM original (preview não deixa sujeira).
    if (scene_.has_value() && scene_->isNode(preview_->entity)) {
        (void)setTransform(preview_->entity, preview_->original);
    }
    preview_.reset();
}

void EditorDocument::loadAnimationBank()
{
    // MERGE, sem clear: clips adicionados programaticamente (API C++/testes)
    // com nomes ÚNICOS sobrevivem; assets são a FONTE DE AUTORIA e
    // sobrescrevem clipes de mesmo nome (insert_or_assign do banco).
    if (assets_ == nullptr) {
        return;
    }
    auto listed = assets_->list(kAnimCategory);
    if (listed.isError()) {
        return;
    }
    for (const auto& entry : listed.value()) {
        auto content = animationRead(entry.name);
        if (content.isError()) {
            ENG_WARN("animation: {} ilegível ({})", entry.name,
                     content.error().message);
            continue;
        }
        auto decoded = animationDecode(content.value());
        if (decoded.isError()) {
            ENG_WARN("animation: {} inválida ({})", entry.name,
                     decoded.error().message);
            continue;
        }
        runtimeAnimations_.add(std::move(decoded.value().clip));
    }
}

void EditorDocument::applyAnimatorFrames(eng::scene::Scene& scene)
{
    scene.world().each<eng::animation::Animator>(
        [&](eng::ecs::Entity entity,
            const eng::animation::Animator& animator) {
            if (!animator.applySprite) {
                return;
            }
            const eng::animation::AnimationClip* clip =
                runtimeAnimations_.find(animator.clip);
            if (clip == nullptr) {
                return;
            }
            const auto* frame =
                eng::animation::sampleFrame(*clip, animator.time);
            if (frame == nullptr) {
                return;
            }
            auto* sprite = scene.world().get<eng::editor::SpriteData>(entity);
            if (sprite == nullptr) {
                return;
            }
            sprite->textureAsset = frame->textureAsset;
            sprite->u0 = frame->u0;
            sprite->v0 = frame->v0;
            sprite->u1 = frame->u1;
            sprite->v1 = frame->v1;
        });
}

// =============================================================================
// Áudio authorável (P2 §12)
// =============================================================================

Result<std::shared_ptr<const eng::audio::Sound>>
EditorDocument::soundFor(std::string_view assetName)
{
    const std::string key{assetName};
    const auto cached = soundCache_.find(key);
    if (cached != soundCache_.end()) {
        return cached->second;
    }
    if (assets_ == nullptr) {
        return makeUnexpected(documentError(StatusCode::InvalidState,
                                            "nenhum projeto aberto"));
    }
    auto bytes = assets_->read("audio", key);
    if (bytes.isError()) {
        return makeUnexpected(bytes.error());
    }
    auto wav = eng::audio::Wav::parse(
        std::span{bytes.value().data(), bytes.value().size()});
    if (wav.isError()) {
        return makeUnexpected(wav.error());
    }
    auto sound = eng::audio::Sound::fromWav(wav.value());
    if (sound.isError()) {
        return makeUnexpected(sound.error());
    }
    auto shared = std::make_shared<const eng::audio::Sound>(
        std::move(sound.value()));
    soundCache_[key] = shared;
    return shared;
}

Result<void> EditorDocument::audioPreview(std::string_view assetName)
{
    if (assets_ == nullptr) {
        return makeUnexpected(documentError(StatusCode::InvalidState,
                                            "nenhum projeto aberto"));
    }
    // 2º toque no MESMO asset = STOP (a voice vivia
    // "para sempre" — sem handle de stop, o som só morria reiniciando a
    // engine). Voice de outro asset: para a anterior e toca o novo.
    if (previewVoice_.isValid() && audioMixer_.isPlaying(previewVoice_) &&
        previewAsset_ == assetName) {
        audioPreviewStop();
        return {};
    }
    audioPreviewStop();  // mata voice anterior (outro asset/terminada)
    auto sound = soundFor(assetName);
    if (sound.isError()) {
        return makeUnexpected(sound.error());
    }
    // Bus de PREVIEW (isolado das vozes de jogo do master) — o stop é
    // por HANDLE (nunca stopAll, que mataria vozes do jogo).
    auto played = audioMixer_.playSound(*sound.value(), previewBus_,
                                        1.f, false);
    if (played.isError()) {
        return makeUnexpected(played.error());
    }
    previewVoice_ = played.value();
    previewAsset_ = std::string{assetName};
    audioMixer_.tick();
    return {};
}

void EditorDocument::audioPreviewStop() noexcept
{
    // Stop POR HANDLE — idempotente; handle obsoleto é no-op
    // seguro do mixer. Nenhum outro caminho mata a voice.
    if (previewVoice_.isValid()) {
        audioMixer_.stop(previewVoice_);
        previewVoice_ = eng::audio::VoiceHandle{};
    }
    previewAsset_.clear();
}

bool EditorDocument::audioPreviewPlaying() const noexcept
{
    return previewVoice_.isValid() && audioMixer_.isPlaying(previewVoice_);
}

// =============================================================================
// Ticks/Camadas
// =============================================================================

Result<std::vector<EditorDocument::LayerInfo>> EditorDocument::layerList()
    const
{
    std::vector<LayerInfo> layers;
    for (const eng::scene::LayerDefinition& layer :
         scene_->layers().definitions()) {
        LayerInfo info;
        info.name = layer.name;
        info.timeScale = layer.timeScale;
        info.update = layer.participation.update;
        info.physics = layer.participation.physics;
        info.render = layer.participation.render;
        layers.push_back(std::move(info));
    }
    return layers;
}

Result<void> EditorDocument::addLayer(std::string_view name)
{
    auto guard = requireEditMode();
    if (guard.isError()) {
        return makeUnexpected(guard.error());
    }
    if (name.empty()) {
        return makeUnexpected(documentError(StatusCode::InvalidArgument,
                                            "nome de camada vazio"));
    }
    return scene_->layers().addLayer(name);
}

Result<void> EditorDocument::setLayerTimeScale(std::string_view name,
                                               float timeScale)
{
    auto guard = requireEditMode();
    if (guard.isError()) {
        return makeUnexpected(guard.error());
    }
    if (!std::isfinite(timeScale) || timeScale < 0.f) {
        return makeUnexpected(documentError(
            StatusCode::InvalidArgument,
            "timeScale deve ser finito e >= 0 (0 = camada pausada)"));
    }
    auto applied = scene_->layers().setTimeScale(name, timeScale);
    if (applied.isError()) {
        return applied;
    }
    sceneDirty_ = true;
    return {};
}

Result<void> EditorDocument::setLayerParticipation(std::string_view name,
                                                   bool update, bool physics,
                                                   bool render)
{
    auto guard = requireEditMode();
    if (guard.isError()) {
        return makeUnexpected(guard.error());
    }
    eng::scene::LayerParticipation participation;
    participation.update = update;
    participation.physics = physics;
    participation.render = render;
    auto applied = scene_->layers().setParticipation(name, participation);
    if (applied.isError()) {
        return applied;
    }
    sceneDirty_ = true;
    return {};
}

Result<void> EditorDocument::setPhysicsFixedDt(float fixedDt)
{
    auto guard = requireEditMode();
    if (guard.isError()) {
        return makeUnexpected(guard.error());
    }
    if (!std::isfinite(fixedDt) || fixedDt <= 0.f || fixedDt > 0.25f) {
        return makeUnexpected(documentError(
            StatusCode::InvalidArgument,
            "timestep da física deve ser finito, > 0 e <= 0.25 s"));
    }
    physicsAccumulator_.setFixedDt(fixedDt);
    sceneDirty_ = true;
    return {};
}


// =============================================================================
// Undo/redo por SNAPSHOTS de cena + fit do viewport
// =============================================================================

// NOT const: SceneSerializer::save recebe Scene& (canonicaliza IDs —
// o MESMO caminho do saveScene; capturar é um efeito documentado).
bool EditorDocument::captureScene(std::string& out) noexcept
{
    if (!scene_.has_value()) {
        return false;
    }
    auto saved = eng::scene::SceneSerializer::save(*scene_);
    if (saved.isError()) {
        ENG_WARN("histórico: serialização falhou: {}", saved.error().message);
        return false;
    }
    out = std::move(saved.value());
    return true;
}

bool EditorDocument::pushHistory(std::string_view label) noexcept
{
    if (suppressHistory_ || historyGroupDepth_ > 0) {
        return false;  // op aninhada ou dentro de um grupo
    }
    if (mode_ != Mode::Edit || !scene_.has_value()) {
        return false;
    }
    std::string snapshot;
    if (!captureScene(snapshot)) {
        return false;  // sem snapshot → sem undo desta op (honesto)
    }
    const auto now = std::chrono::steady_clock::now();
    // Coalescência SÓ do gesto "move" (stream de scroll sem begin/end):
    // eventos dentro da janela renovam o instante (1 gesto = 1 undo).
    // Todas as outras ops capturam a PRÓPRIA entrada (editar A e B
    // seguidos = 2 undos — cada apply do autor é um passo).
    if (label == "move" && !undoStack_.empty() &&
        undoStack_.back().label == label) {
        const auto dt = std::chrono::duration<float>(
            now - undoStack_.back().time).count();
        if (dt < kHistoryCoalesceSec) {
            undoStack_.back().time = now;
            redoStack_.clear();
            return true;
        }
    }
    undoStack_.push_back(
        HistoryEntry{std::move(snapshot), std::string(label), now});
    while (undoStack_.size() > kHistoryMax) {
        undoStack_.pop_front();
    }
    redoStack_.clear();
    return true;
}

void EditorDocument::beginHistoryGroup(std::string_view label)
{
    if (historyGroupDepth_ == 0) {
        (void)pushHistory(label);
    }
    ++historyGroupDepth_;
}

void EditorDocument::endHistoryGroup() noexcept
{
    if (historyGroupDepth_ > 0) {
        --historyGroupDepth_;
    }
}

void EditorDocument::clearHistory() noexcept
{
    undoStack_.clear();
    redoStack_.clear();
}

Result<void> EditorDocument::undo()
{
    if (mode_ == Mode::Play) {
        return makeUnexpected(documentError(
            StatusCode::InvalidState, "undo indisponível em Play"));
    }
    if (undoStack_.empty()) {
        return makeUnexpected(documentError(StatusCode::InvalidState,
                                            "nada a desfazer"));
    }
    // 1. Estado ATUAL vai para o redo (best-effort).
    std::string currentState;
    const bool haveRedo = captureScene(currentState);
    // 2. Restaura o ANTES (valida o JSON ANTES de tocar na cena).
    HistoryEntry entry = std::move(undoStack_.back());
    undoStack_.pop_back();
    auto preparse = eng::serial::parseJson(entry.snapshot);
    if (preparse.isError()) {
        undoStack_.push_back(std::move(entry));  // devolve: nada mudou
        return makeUnexpected(documentError(
            StatusCode::Internal, "snapshot ilegível (bug)"));
    }
    scene_.emplace();  // cena limpa (não-movível — ADR-025); layers voltam
                       // pelo próprio snapshot (secção "layers")
    auto applied = eng::scene::SceneSerializer::load(*scene_, entry.snapshot);
    if (applied.isError()) {
        // Snapshot autogerado — caminho de bug; erro EXPLÍCITO (nunca
        // silêncio), cena vazia + histórico intacto para diagnóstico.
        selection_.reset();
        ++selectionRevision_;
        sceneDirty_ = true;
        gizmoDragEnd();
        previewStop();
        return makeUnexpected(documentError(
            StatusCode::Internal,
            "restaurar snapshot falhou: " + applied.error().message));
    }
    if (haveRedo) {
        redoStack_.push_back(HistoryEntry{
            std::move(currentState), entry.label,
            std::chrono::steady_clock::now()});
        while (redoStack_.size() > kHistoryMax) {
            redoStack_.pop_front();
        }
    }
    // IDs mudaram (cena recriada) — seleção morre com honestidade.
    gizmoDragEnd();
    previewStop();
    selection_.reset();
    ++selectionRevision_;
    sceneDirty_ = true;
    return {};
}

Result<void> EditorDocument::redo()
{
    if (mode_ == Mode::Play) {
        return makeUnexpected(documentError(
            StatusCode::InvalidState, "redo indisponível em Play"));
    }
    if (redoStack_.empty()) {
        return makeUnexpected(documentError(StatusCode::InvalidState,
                                            "nada a refazer"));
    }
    std::string currentState;
    const bool haveUndo = captureScene(currentState);
    HistoryEntry entry = std::move(redoStack_.back());
    redoStack_.pop_back();
    auto preparse = eng::serial::parseJson(entry.snapshot);
    if (preparse.isError()) {
        redoStack_.push_back(std::move(entry));
        return makeUnexpected(documentError(
            StatusCode::Internal, "snapshot ilegível (bug)"));
    }
    scene_.emplace();
    auto applied = eng::scene::SceneSerializer::load(*scene_, entry.snapshot);
    if (applied.isError()) {
        selection_.reset();
        ++selectionRevision_;
        sceneDirty_ = true;
        gizmoDragEnd();
        previewStop();
        return makeUnexpected(documentError(
            StatusCode::Internal,
            "restaurar snapshot falhou: " + applied.error().message));
    }
    if (haveUndo) {
        undoStack_.push_back(HistoryEntry{
            std::move(currentState), entry.label,
            std::chrono::steady_clock::now()});
        while (undoStack_.size() > kHistoryMax) {
            undoStack_.pop_front();
        }
    }
    gizmoDragEnd();
    previewStop();
    selection_.reset();
    ++selectionRevision_;
    sceneDirty_ = true;
    return {};
}

void EditorDocument::viewportFit(TextureCache* textures)
{
    if (mode_ != Mode::Edit || !scene_.has_value()) {
        return;
    }
    // AABB dos quads DESENHADOS (mesma fórmula do renderer/bounds —
    // textura/ppu quando resolvível; sem textura → quad da escala).
    float minX = 0.f, minY = 0.f, maxX = 0.f, maxY = 0.f;
    bool any = false;
    auto quads = viewport_.buildQuads(*scene_, selection_);
    for (const EntityQuad& quad : quads) {
        // Com seleção: só a selecionada; sem: a cena inteira.
        if (selection_.has_value() && quad.entity != *selection_) {
            continue;
        }
        float halfW = quad.sizeX * 0.5f;
        float halfH = quad.sizeY * 0.5f;
        if (!quad.textureAsset.empty() && textures != nullptr &&
            assets_ != nullptr) {
            const auto info = textures->imageInfo(*assets_, quad.textureAsset);
            if (info.valid) {
                const float regionPx =
                    static_cast<float>(info.width) * (quad.u1 - quad.u0);
                const float regionPy =
                    static_cast<float>(info.height) * (quad.v1 - quad.v0);
                const float ppu =
                    quad.spritePpu > 0.f ? quad.spritePpu : 1.f;
                halfW = quad.sizeX * regionPx / ppu * 0.5f;
                halfH = quad.sizeY * regionPy / ppu * 0.5f;
            }
        }
        // AABB conservador do quad RODADO (expande pelos eixos).
        const float c = std::abs(std::cos(quad.rotation));
        const float s = std::abs(std::sin(quad.rotation));
        const float ex = c * halfW + s * halfH;
        const float ey = s * halfW + c * halfH;
        if (!any) {
            minX = quad.worldX - ex; maxX = quad.worldX + ex;
            minY = quad.worldY - ey; maxY = quad.worldY + ey;
            any = true;
        } else {
            minX = std::min(minX, quad.worldX - ex);
            maxX = std::max(maxX, quad.worldX + ex);
            minY = std::min(minY, quad.worldY - ey);
            maxY = std::max(maxY, quad.worldY + ey);
        }
    }
    auto& cam = viewport_.camera();
    if (!any) {
        // Cena vazia: reset honesto (origem, zoom de fábrica).
        cam.posX = 0.f;
        cam.posY = 0.f;
        cam.zoom = 48.f;
        ++selectionRevision_;
        return;
    }
    // Margem de 25% para o conteúdo respirar (não colado nas arestas).
    const float halfW = std::max((maxX - minX) * 0.5f * 1.25f, 0.5f);
    const float halfH = std::max((maxY - minY) * 0.5f * 1.25f, 0.5f);
    const float screenW = viewport_.screenWidth();
    const float screenH = viewport_.screenHeight();
    const float zoom = std::min(screenW / (2.f * halfW),
                                screenH / (2.f * halfH));
    cam.posX = (minX + maxX) * 0.5f;
    cam.posY = (minY + maxY) * 0.5f;
    cam.zoom = std::clamp(zoom, Viewport::kMinZoom, Viewport::kMaxZoom);
    ++selectionRevision_;  // Inspector/viewport sincronizam
}

Result<std::vector<std::string>> EditorDocument::sceneList() const
{
    if (!project_.has_value()) {
        return makeUnexpected(
            documentError(StatusCode::InvalidState, "sem projeto aberto"));
    }
    auto listed = fs_->list(scenesRootOf(*project_), false);
    if (listed.isError()) {
        return makeUnexpected(listed.error());
    }
    std::vector<std::string> names;
    for (const auto& entry : listed.value()) {
        if (entry.isDirectory) {
            continue;
        }
        std::string name = entry.path.filename().str();
        if (name.size() > 5 && name.ends_with(".json")) {
            names.push_back(std::move(name));
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace eng::editor
