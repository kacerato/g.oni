#pragma once

/// eng::editor::EditorDocument — estado e comandos do editor
///.
///
/// Modelo de estado (missão §8.9) — SEM globals:
///   ProjectState  = ProjectFile + dirty
///   SceneState    = Scene em EDIÇÃO + dirty
///   SelectionState= entidade selecionada (ou nada)
///   ViewportState = Viewport (câmera 2D)
///   InspectorState= leitura pura via Inspector (sem estado próprio)
///   RUNTIME       = Scene clone existindo APENAS em Play
///
/// SEPARAÇÃO EDITOR × RUNTIME (missão §8.7, ADR-044): o estado do editor é
/// a cena editada; o runtime é um CLONE por serialização (round-trip já
/// testado na FASE 3) que nasce no play() e é DESCARTADO no stop(). Em
/// Play, comandos de edição são REJEITADOS (erro explícito) — o viewport
/// renderiza o clone; mutação de entidades em Play (arraste — ferramenta
/// de debug) altera o CLONE e nunca vaza para a edição.
///
/// Todos os comandos devolvem Result (missão: erros com contexto). I/O
/// sempre via eng::fs (paths RELATIVOS ao root — §8.1; absolutos são
/// rejeitados pelo próprio ProjectFile/Path).

#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "eng/core/Result.hpp"
#include "eng/animation/Animation.hpp"
#include "eng/audio/Audio.hpp"
#include "eng/ecs/Ecs.hpp"
#include "eng/editor/AssetBrowser.hpp"
#include "eng/render/SpriteMaterial.hpp"
#include "eng/editor/Gizmo.hpp"
#include "eng/editor/Inspector.hpp"
#include "eng/editor/Viewport.hpp"
#include "eng/fs/FileSystem.hpp"
#include "eng/fs/Path.hpp"
#include "eng/input/Input.hpp"
#include "eng/math/Vec3.hpp"
#include "eng/niscript/NiVm.hpp"
#include "eng/particles/Particles.hpp"
#include "eng/physics/Physics.hpp"
#include "eng/project/ProjectFile.hpp"
#include "eng/scene/Scene.hpp"
#include "eng/tick/Camera.hpp"
#include "eng/tick/Tick.hpp"

namespace eng::editor {

class NiRuntime;     // NiRuntime.hpp (frente — impl em NiRuntime.cpp)
class TextureCache;  // TextureCache.hpp (parâmetro de bounds/gizmo)

/// Registra os componentes de gameplay (physics/animation/particles) no
/// catálogo do serializer — efeito colateral da inicialização estática de
/// ComponentRegistration.cpp; a chamada garante que o TU entre no link
/// (libs estáticas descartam objetos não referenciados).
void ensureEditorComponentsRegistered() noexcept;

/// TRS do editor com rotação em GRAUS Euler XYZ (convenção do usuário; a
/// cena guarda Quat — conversão interna testada).
struct TransformDesc {
    eng::math::Vec3 position{};
    eng::math::Vec3 rotationDegrees{};
    eng::math::Vec3 scale{1.f, 1.f, 1.f};
};

class EditorDocument final {
public:
    /// Documento sem projeto (cena vazia pronta para edição). O fs é
    /// EMPRESTADO (o host é o dono — EditorHost no Android, teste no
    /// Linux): NativeFileSystem no app, MemoryFileSystem nos testes
    /// (missão §8.1: reuso de eng::project/fs, nada de paths absolutos
    /// DENTRO do projeto). O documento não sobrevive ao fs.
    [[nodiscard]] static eng::core::Result<std::unique_ptr<EditorDocument>>
    create(eng::fs::FileSystem& fs, const eng::fs::Path& workspaceRoot);

    ~EditorDocument();
    EditorDocument(const EditorDocument&) = delete;
    EditorDocument& operator=(const EditorDocument&) = delete;

    // --- projeto ------------------------------------------------------

    /// Cria <workspaceRoot>/<name> com project.goni.json, assets/<cats>/ e
    /// scenes/ e o ABRE. Erro se já existe.
    [[nodiscard]] eng::core::Result<void> newProject(std::string_view name);

    /// Abre projeto existente (project.goni.json parseado — ADR-032).
    [[nodiscard]] eng::core::Result<void> openProject(
        const eng::fs::Path& projectRoot);

    /// Persiste config + registry de assets.
    [[nodiscard]] eng::core::Result<void> saveProject();

    [[nodiscard]] bool hasProject() const noexcept { return project_.has_value(); }
    [[nodiscard]] std::string projectName() const;
    /// Project Settings: renomeia (config + disco + dirty).
    [[nodiscard]] eng::core::Result<void> setProjectName(std::string_view name);
    [[nodiscard]] eng::fs::Path projectRoot() const;

    // --- startup (bug Android "AlreadyExists" — bloco P3 §0) -----------------
    //
    // CAUSA RAIZ do bug: hasProject() reflete o estado EM MEMÓRIA, que é
    // sempre "sem projeto" num processo novo — usado como detector de
    // "primeira execução", levava o startup a CHAMAR newProject("MeuJogo")
    // quando o projeto JÁ EXISTIA no disco (AlreadyExists + editor sem
    // projeto). A política abaixo separa os casos corretamente.

    /// Projetos no workspace: diretórios com project.goni.json, ordem
    /// alfabética, sem ocultos (staging do SAF, marcadores internos).
    [[nodiscard]] eng::core::Result<std::vector<std::string>>
    listProjects() const;

    /// Último projeto usado neste workspace (arquivo oculto
    /// .goni_last_project na RAIZ do workspace — fora de qualquer
    /// projeto, portanto nunca exportado no zip). Vazio = sem registro
    /// (instalação limpa ou primeira execução).
    [[nodiscard]] std::string lastUsedProject() const;

    /// Registra `name` como último usado (chamado automaticamente em
    /// new/open bem-sucedidos; best-effort — falha NUNCA derruba a
    /// operação de projeto, o fallback é abrir o default/primeiro).
    [[nodiscard]] eng::core::Result<void> rememberLastUsedProject(
        std::string_view name);

    /// Política de projeto na inicialização (origem: Activity/EditorHost):
    ///   1. projeto em memória → no-op (devolve o nome atual);
    ///   2. workspace SEM projetos → cria o projeto default ("MeuJogo");
    ///   3. workspace COM projetos → ABRE o último usado; senão o default
    ///      "MeuJogo" (se existir); senão o primeiro em ordem alfabética.
    /// Devolve o nome do projeto criado/aberto. ERRO CONTROLADO —
    /// AlreadyExists é IMPOSSÍVEL neste caminho (só cria quando não há
    /// NENHUM projeto no workspace). Cada decisão é logada (operação,
    /// projeto, caminho, estado do documento — logcat [GONI]).
    [[nodiscard]] eng::core::Result<std::string> ensureStartupProject();

    // --- cena ----------------------------------------------------------

    /// Cena nova (a atual é descartada — sem perda silenciosa: chamador
    /// decide salvar antes; sceneDirty reflete).
    eng::core::Result<void> newScene();
    /// Salva em <scenesRoot>/<path>. Sujo→limpo.
    [[nodiscard]] eng::core::Result<void> saveScene(std::string_view scenePath);
    /// Carrega (LIMPA a cena atual e preenche — SceneSerializer::load em
    /// cena vazia; ADR-033).
    [[nodiscard]] eng::core::Result<void> loadScene(std::string_view scenePath);
    /// Cenas salvas do projeto aberto (nomes relativos a scenes/).
    [[nodiscard]] eng::core::Result<std::vector<std::string>> sceneList() const;

    // --- P4.5: undo/redo (command pattern por SNAPSHOTS de cena) ------------
    //
    // Estratégia: cada operação mutante captura o estado ANTES via
    // SceneSerializer::save (o MESMO codec do saveScene — round-trip
    // testado). Isto dá undo/redo CORRETO para create/delete/attach sem
    // remapeamento de IDs: restaurar recria a cena inteira (IDs novos são
    // internos — a seleção é limpa com honestidade). Gestos contínuos
    // (drag de gizmo / move por scroll) coalescem numa ÚNICA entrada por
    // janela de tempo + label (1 gesto = 1 undo).
    //
    // P4.5 restringe o escopo do prompt: move/rotate/scale/create/delete/
    // attach (+ rename/reparent/componentes/campos que são o mesmo gesto
    // de autoria). Assets no disco (scripts/materiais) NÃO participam.

    /// Há passos a desfazer?
    [[nodiscard]] bool canUndo() const noexcept { return !undoStack_.empty(); }
    /// Há passos a refazer?
    [[nodiscard]] bool canRedo() const noexcept { return !redoStack_.empty(); }
    /// Restaura o snapshot anterior (erro explícito em Play/vazio).
    /// Agrupa várias operações num único passo de desfazer (ex.: criar uma
    /// entidade a partir de um modelo = criar + componentes + campos).
    /// Aninhável; só o grupo mais externo grava o snapshot.
    void beginHistoryGroup(std::string_view label);
    void endHistoryGroup() noexcept;
    [[nodiscard]] eng::core::Result<void> undo();
    /// Restaura o estado anterior a um undo (erro explícito em Play/vazio).
    [[nodiscard]] eng::core::Result<void> redo();

    // --- P4.5: snap do gizmo (chips da tool sheet) ----------------------------

    /// Snap de TRANSLAÇÃO para a grade (0.5 unidades de mundo).
    void setSnapTranslate(bool on) noexcept { snapTranslate_ = on; }
    /// Snap de ROTAÇÃO para múltiplos de 15°.
    void setSnapRotate(bool on) noexcept { snapRotate_ = on; }
    [[nodiscard]] bool snapTranslate() const noexcept { return snapTranslate_; }
    [[nodiscard]] bool snapRotate() const noexcept { return snapRotate_; }

    // --- P4.5: fit do viewport (cluster de zoom) -------------------------------

    /// Enquadra a SELEÇÃO (bounds desenhados reais) ou, sem seleção, a cena
    /// inteira (AABB dos quads desenhados). Cena vazia → reset (origem,
    /// zoom default). Erro nunca (cena vazia é caso válido).
    void viewportFit(class TextureCache* textures);

    // --- Ticks/Camadas ------------
    // A engine JÁ tem LayerRegistry (timeScale + participação update/física/
    // render por camada) e TimestepAccumulator (física de passo fixo) — nada
    // disso era AUTORÁVEL. A sheet de Ticks toca este estado REAL.
    struct LayerInfo {
        std::string name;
        float timeScale = 1.f;
        bool update = true;
        bool physics = true;
        bool render = true;
    };
    /// Camadas em ordem da registry (GAME, SUBGAME, nomeadas — estável).
    [[nodiscard]] eng::core::Result<std::vector<LayerInfo>> layerList() const;
    /// Camada nomeada nova (participação total, timeScale 1). Erros: nome
    /// vazio/duplicado/built-in.
    [[nodiscard]] eng::core::Result<void> addLayer(std::string_view name);
    /// timeScale da camada (>= 0; 0 = pausada). Erro: camada inexistente/valor.
    [[nodiscard]] eng::core::Result<void> setLayerTimeScale(
        std::string_view name, float timeScale);
    /// Participação da camada nos estágios (update/física/render).
    [[nodiscard]] eng::core::Result<void> setLayerParticipation(
        std::string_view name, bool update, bool physics, bool render);
    /// Timestep FIXO da física em segundos (PhysicsTick no Play). Persiste
    /// na cena (chave aditiva "physicsFixedDt" — arquivo antigo = 1/60).
    [[nodiscard]] float physicsFixedDt() const noexcept
    {
        return physicsAccumulator_.fixedDt();
    }
    /// Erro se não finito, <= 0 ou > 0.25 s (honesto — sem clamp calado).
    [[nodiscard]] eng::core::Result<void> setPhysicsFixedDt(float fixedDt);

    // --- P4.7.0: kinematic_sweep ----------------------------------
    /// ON (default): `move` do KINEMATIC é VARRIDO (TOI+slide — o script
    /// ingênuo COLIDE; parede para, desliza, nunca atravessa). OFF: `move`
    /// é translação crua (teletransporte — semântica pré-P4.7). Persiste
    /// na cena (chave aditiva "physicsKinematicSweep"; arquivo antigo = ON).
    [[nodiscard]] bool kinematicSweep() const noexcept
    {
        return kinematicSweep_;
    }
    void setKinematicSweep(bool enabled) noexcept
    {
        kinematicSweep_ = enabled;
        sceneDirty_ = true; // persiste no próximo save (como fixedDt)
    }

    // --- P4.7.0: logic LOD -------------------------------------
    /// OFF (default): TODOS os scripts rodam sempre (semântica pré-P4.7).
    /// ON: scripts fora da vista pulam o `up update` (opt-out por script
    /// via NiScriptComponent::lodOptOut — gameplay crítico roda sempre).
    /// Persiste na cena (chave aditiva "logicLodEnabled"; ausente = OFF).
    [[nodiscard]] bool logicLodEnabled() const noexcept
    {
        return logicLodEnabled_;
    }
    void setLogicLodEnabled(bool enabled) noexcept
    {
        logicLodEnabled_ = enabled;
        sceneDirty_ = true;
    }

    // --- P4.6: camadas de COLISÃO nomeadas (project settings) ---
    // ≠ camadas de cena/tick (LayerInfo/ADR-051): estes bitfields filtram
    // PARES de colisão — (A.mask & B.layer) && (B.mask & A.layer) — e são
    // persistidos no project.goni.json ("collisionLayers", chave aditiva).
    struct CollisionLayerInfo {
        std::string name;
        std::uint32_t bit = 1;
    };
    /// Tabela do projeto (vazio = sem projeto; NUNCA vazio com projeto —
    /// default "default"/1 garantido pelo ProjectFile::configFromJson).
    [[nodiscard]] std::vector<CollisionLayerInfo> collisionLayers() const;
    /// Renomeia a camada do bit (valida: bit existe na tabela; nome não
    /// vazio/único). Marca projectDirty_ (flush em saveProject, padrão
    /// setProjectName).
    [[nodiscard]] eng::core::Result<void> setCollisionLayerName(
        std::uint32_t bit, std::string_view name);
    /// Nova camada com o MENOR bit livre (1..2^31). Erro: sem projeto,
    /// nome vazio/duplicado, sem bits livres.
    [[nodiscard]] eng::core::Result<std::uint32_t> addCollisionLayer(
        std::string_view name);

    // --- P4.6: grade do viewport (project settings) -----------
    /// Config corrente (default honesto quando sem projeto). O
    /// ViewportRenderer consome isto por frame (EditorHost repassa).
    [[nodiscard]] const eng::project::GridConfig& gridConfig() const noexcept
    {
        static const eng::project::GridConfig kDefaultGrid{};
        return hasProject() ? project_->config.grid : kDefaultGrid;
    }
    /// Define a grade (validação: cell > 0 finito; majorEvery 2..1024;
    /// cores 0..1). Marca projectDirty_ (flush em saveProject).
    [[nodiscard]] eng::core::Result<void> setGridConfig(
        const eng::project::GridConfig& grid);

    [[nodiscard]] bool sceneDirty() const noexcept { return sceneDirty_; }
    [[nodiscard]] bool projectDirty() const noexcept { return projectDirty_; }
    /// Path da cena ATUAL relativo ao projeto ("main.json") — definido por
    /// saveScene/loadScene; vazio = cena nunca salva (P4.2/B-A: é o que
    /// faz "Salvar projeto" persistir TUDO sem diálogo extra).
    [[nodiscard]] const std::string& currentScenePath() const noexcept
    {
        return currentScenePath_;
    }

    // --- entidades — REJEITADOS em Play ---------------------------

    [[nodiscard]] eng::core::Result<eng::ecs::Entity> createEntity(
        std::string_view name, eng::ecs::Entity parent);
    [[nodiscard]] eng::core::Result<void> deleteEntity(eng::ecs::Entity entity);
    [[nodiscard]] eng::core::Result<void> renameEntity(eng::ecs::Entity entity,
                                                       std::string_view name);
    /// Duplica o nó E SUBÁRVORE (componentes via encode/decode do catálogo).
    [[nodiscard]] eng::core::Result<eng::ecs::Entity> duplicateEntity(
        eng::ecs::Entity entity);
    /// parent == kNoEntity → vira raiz. Ciclo → erro propagado.
    [[nodiscard]] eng::core::Result<void> reparentEntity(
        eng::ecs::Entity entity, eng::ecs::Entity parent);

    [[nodiscard]] eng::core::Result<TransformDesc> transform(
        eng::ecs::Entity entity) const;
    [[nodiscard]] eng::core::Result<void> setTransform(eng::ecs::Entity entity,
                                                       const TransformDesc& desc);

    // --- seleção --------------------------------------------------------------

    [[nodiscard]] eng::core::Result<void> select(eng::ecs::Entity entity);
    void deselect() noexcept;
    [[nodiscard]] bool isSelected(eng::ecs::Entity entity) const noexcept;
    [[nodiscard]] std::optional<eng::ecs::Entity> selection() const noexcept
    {
        return selection_;
    }
    /// Revisão do estado de seleção/transform — incrementa a TODA mutação
    /// que o Inspector deve refletir (select/deselect/tap/setTransform/
    /// move/gizmo/duplicate/delete). O host (Activity) faz POLL por frame
    /// e atualiza os campos de UI ao vivo — sync bidirecional sem duas
    /// fontes de verdade (a verdade é o ECS; P1.9).
    [[nodiscard]] std::uint64_t selectionRevision() const noexcept
    {
        return selectionRevision_;
    }

    // --- ferramentas + gizmo ----------------------------------------

    [[nodiscard]] EditorTool tool() const noexcept { return tool_; }
    /// Trocar de ferramenta RE-ARMA o gizmo — nenhum estado
    /// de drag sobrevive (o drag de A nunca vira drag de B por troca de
    /// ferramenta no meio de um gesto).
    void setTool(EditorTool tool) noexcept
    {
        if (tool_ != tool) {
            gizmoDragEnd();
            // Transição 120ms — o pop do gizmo recomeça a cada
            // troca EFETIVA de ferramenta.
            toolChangedAt_ = std::chrono::steady_clock::now();
        }
        tool_ = tool;
    }

    /// Escala corrente do pop da transição (0.88..1.0) — o
    /// gizmoDraw multiplica os halfes dos handles por isto.
    [[nodiscard]] float gizmoHandlePop() const noexcept
    {
        const float ms = std::chrono::duration<float, std::milli>(
                             std::chrono::steady_clock::now() - toolChangedAt_)
                             .count();
        return TransformGizmo::transitionScale(ms);
    }

    /// Bounds REAIS da entidade selecionada (posição/rotação/escala/
    /// textura/ppu — P1.2): tamanho DESENHADO em mundo, o mesmo do
    /// renderer e do hit-test. `textures` resolve dimensões de pixel via
    /// cache do host (nulo → caminho da escala local).
    [[nodiscard]] GizmoBounds selectionBounds(TextureCache* textures) const;

    /// Toca no viewport com ferramenta de transformação ativa: acerta o
    /// HANDLE do gizmo (precedência sobre o corpo da entidade) e INICIA
    /// o drag capturando o transform inicial. None quando não acertou
    /// (o chamador segue para o fluxo normal de seleção). Em Play o
    /// gizmo não existe → None (edição é rejeitada em Play §8.7).
    [[nodiscard]] GizmoHandle gizmoDragBegin(float screenX, float screenY,
                                              TextureCache* textures);

    /// Arraste ativo → aplica o TRANSFORM ALVO ao ECS da cena em EDIÇÃO
    /// (dirty). Sem drag ativo → no-op Ok.
    [[nodiscard]] eng::core::Result<void> gizmoDragTo(float screenX,
                                                      float screenY);
    void gizmoDragEnd() noexcept;

    /// Geometria de desenho do gizmo (MUNDO) para o renderer do host.
    /// Vazia quando: sem seleção, tool Select, bounds inválido ou Play.
    [[nodiscard]] GizmoDrawData gizmoDraw(TextureCache* textures) const;

    /// ADD → Sprite: entidade nova com Name + SpriteData default
    /// (sem textura — placeholder claramente identificado no viewport),
    /// selecionada e marcada dirty. O nome recebe numeração automática
    /// para manter a hierarquia legível.
    [[nodiscard]] eng::core::Result<eng::ecs::Entity> createSprite(
        std::string_view name);

    // --- componentes — Inspector + guarda de modo ----------------------

    [[nodiscard]] std::vector<Inspector::Field> inspectorFields(
        eng::ecs::Entity entity, std::string_view component) const;
    [[nodiscard]] eng::core::Result<void> setInspectorField(
        eng::ecs::Entity entity, std::string_view component,
        std::string_view fieldPath, std::string_view value);
    [[nodiscard]] eng::core::Result<void> addComponent(
        eng::ecs::Entity entity, std::string_view component);
    [[nodiscard]] eng::core::Result<void> removeComponent(
        eng::ecs::Entity entity, std::string_view component);

    // --- componentes authoráveis (P2 §2/§14) -----------------------------------

    /// Um tipo de componente do catálogo, com metadados de AUTHORING.
    struct ComponentMeta {
        std::string name;          ///< nome canônico (API do catálogo)
        bool addable = false;      ///< false: built-in obrigatório (Name/Transform)
        std::string dependency;   ///< hint de dependência ("" quando nenhuma)
    };

    /// Catálogo de componentes ADDÁVEIS à entidade (P2 §2): tipos REALMENTE
    /// registrados no engine, MENOS os que a entidade já possui e os
    /// built-ins obrigatórios. Nada de componentes falsos — o Inspector
    /// lista o que o catálogo único tem.
    [[nodiscard]] std::vector<ComponentMeta> addableComponents(
        eng::ecs::Entity entity) const;

    /// Adiciona componente COM dependências (P2 §14): retorna a lista de
    /// TODOS os componentes criados (o pedido + os auto-criados seguros).
    /// Auto-criação é ADITIVA (nunca destrói dados): hoje Animator→SpriteData
    /// quando o clip tem frames; o resto é hint claro na UI.
    [[nodiscard]] eng::core::Result<std::vector<std::string>>
    addComponentWithDependencies(eng::ecs::Entity entity,
                                 std::string_view component);

    /// Importa asset da staging (".import_tmp/...") para a categoria com
    /// VALIDAÇÃO DE CONTEÚDO no import (P4.2/B-E — honestidade: lixo não
    /// entra no projeto; texturas já validavam, áudio não):
    ///   - textures: probe de decode (eng::image::decode);
    ///   - audio: probe de RIFF/WAVE PCM (eng::audio::Wav::parse) — não-WAV
    ///     é RECUSADO no import com mensagem clara ("apenas WAV PCM
    ///     suportado por agora"); OGG/MP3 é fase futura (roadmap).
    /// Falha → arquivo não é movido/registrado (remove da categoria) e o
    /// erro precisa volta para a UI. Devolve o nome FINAL (extensão
    /// preservada — recovery P0).
    [[nodiscard]] eng::core::Result<std::string> importAsset(
        std::string_view tempRelPath, std::string_view category,
        std::string_view name);

    /// Zip do projeto ATUAL (assets + scenes + project.goni.json + meta)
    /// para `<zipRelPath>` (relativo ao workspace; oculto ".goni_export.zip"
    /// no device), entradas embrulhadas na PASTA real do projeto (não no
    /// config.name — P4.2/B-A).
    [[nodiscard]] eng::core::Result<void> exportProjectZip(
        std::string_view zipRelPath);
    /// Extrai `<zipRelPath>` no workspace e devolve o nome da pasta criada
    /// (wrapper do zip vence; senão `preferredName`). NÃO abre o projeto —
    /// o chamador decide (openProject explícito).
    [[nodiscard]] eng::core::Result<std::string> importProjectZip(
        std::string_view zipRelPath, std::string_view preferredName);

    // --- play/stop ---------------------------------------------

    [[nodiscard]] eng::core::Result<void> play();
    void stop() noexcept;
    [[nodiscard]] bool isPlaying() const noexcept { return mode_ == Mode::Play; }
    /// Pausa do RUNTIME — tick() não avança o
    /// mundo (física/scripts/animação/áudio congelam), render e câmera
    /// continuam. Só tem efeito em Play.
    void setPaused(bool paused) noexcept { paused_ = paused; }
    [[nodiscard]] bool isPaused() const noexcept { return paused_; }
    /// Avanço do runtime por frame. Play: input + TICK SCHEDULER
    /// (evolução P0-5, ADR-051 — física com timestep fixo, animação,
    /// partículas, scripts e câmera agendados por (fase, ordem)) sobre o
    /// CLONE. Edit: parado (gestos não vazam — §6.4). Depois do frame a
    /// câmera de jogo ativa (se houver) toma o viewport.
    void tick(float deltaSeconds) noexcept;

    /// Agendador de ticks do runtime. Vazio em Edit; construído no
    /// play(). Diagnóstico/testes.
    [[nodiscard]] const eng::tick::TickScheduler* runtimeScheduler()
        const noexcept
    {
        return scheduler_.get();
    }

    /// Câmera de jogo ativa do último frame — o viewport a usa em
    /// Play quando a cena tem câmera ativa.
    [[nodiscard]] bool hasGameCamera() const noexcept
    {
        return gameCameraActive_;
    }

    /// Física do runtime (contatos do último passo — gameplay/debug).
    [[nodiscard]] const eng::physics::PhysicsWorld& runtimePhysics() const
        noexcept
    {
        return physicsWorld_;
    }
    /// Runtime de scripts NI-Script do clone.
    [[nodiscard]] const NiRuntime& runtimeScripts() const noexcept
    {
        return *niRuntime_;
    }
    /// Banco de animações do runtime (clips por nome — API C++ §9).
    eng::animation::AnimationBank& runtimeAnimations() noexcept
    {
        return runtimeAnimations_;
    }

    /// INPUT DO JOGO: os
    /// toques do viewport em Play alimentam ESTE sistema; bindings são
    /// configuráveis por asset (input.json — ActionBindings::fromJson).
    [[nodiscard]] eng::input::InputSystem& runtimeInput() noexcept
    {
        return runtimeInput_;
    }
    void setRuntimeBindings(eng::input::ActionBindings bindings)
    {
        runtimeInput_.setBindings(std::move(bindings));
    }
    /// Toque do jogo (em Play). Fase canônica: 0=Down,1=Move,2=Up,3=Cancel.
    void gameTouch(int canonicalPhase, std::uint32_t pointerId, float x,
                   float y, float pressure);
    void setGameViewportSize(float width, float height) noexcept;

    // --- viewport --------------------------------------------------------

    [[nodiscard]] Viewport& viewport() noexcept { return viewport_; }
    [[nodiscard]] const Viewport& viewport() const noexcept { return viewport_; }

    /// Tap → seleção (hit-test top-most). Sem hit → deselect.
    /// `textures` (opcional): cache do HOST para resolver as dimensões em
    /// pixels das texturas dos sprites — o hit-test usa o tamanho DESENHADO
    /// (região/ppu), não a escala local. Nulo → sprites usam o caminho da
    /// escala (quads de cor/sem resolução).
    /// Hit-test sem alterar a seleção (o host decide o que o gesto faz).
    [[nodiscard]] std::optional<eng::ecs::Entity> viewportPick(
        float screenX, float screenY,
        class TextureCache* textures = nullptr) const;
    [[nodiscard]] std::optional<eng::ecs::Entity> viewportTap(
        float screenX, float screenY,
        class TextureCache* textures = nullptr);
    void viewportPan(float screenDx, float screenDy) noexcept;
    void viewportZoom(float factor, float focusX, float focusY) noexcept;
    /// Arraste: move a entidade (delta de TELA → mundo). Edit: move na
    /// EDIÇÃO (dirty); Play: move no CLONE (debug — não vaza).
    [[nodiscard]] eng::core::Result<void> moveEntityScreen(
        eng::ecs::Entity entity, float screenDx, float screenDy);

    /// Snapshot da hierarquia — caminhada depth-first estável.
    struct HierarchyNode {
        eng::ecs::Entity entity{};
        std::string name;
        int depth = 0;
    };
    [[nodiscard]] std::vector<HierarchyNode> hierarchySnapshot() const;

    /// Cena em FOCO (render/consulta): edição em Edit, CLONE em Play.
    [[nodiscard]] eng::scene::Scene* sceneInFocus() noexcept;
    [[nodiscard]] const eng::scene::Scene* sceneInFocus() const noexcept;

    // --- assets -----------------------------------------------------------

    [[nodiscard]] AssetBrowser* assets() noexcept { return assets_.get(); }
    [[nodiscard]] const AssetBrowser* assets() const noexcept
    {
        return assets_.get();
    }

    /// Nome de exibição (Name component; "Entity" quando ausente).
    [[nodiscard]] static std::string nameOf(const eng::scene::Scene& scene,
                                            eng::ecs::Entity entity);

    /// Empacota/desempacota Entity para tráfico JNI (index+1|generation;
    /// 0 = "nenhuma" — valor, não ponteiro; auditoria §4).
    [[nodiscard]] static std::uint64_t packEntity(eng::ecs::Entity entity) noexcept;
    [[nodiscard]] static eng::ecs::Entity unpackEntity(
        std::uint64_t packed) noexcept;

    // --- scripts NI-Script (evolução P0-7, ADR-053) ------------------------------
    //
    // O asset .nis é a FONTE DE EDIÇÃO (assets/scripts/*.nis); a cena
    // carrega uma CÓPIA estável em NiScriptComponent.source (ADR-043 — o
    // runtime/serialização nunca dependem de arquivos externos à cena).
    // scriptAssign copia do asset para o componente; o workflow completo
    // é: criar → editar → compilar (validação) → anexar à entidade.

    /// Diagnóstico de compilação .nis (1-based).
    struct ScriptDiag {
        std::uint32_t line = 0;
        std::uint32_t col = 0;
        std::string message;
    };

    // --- animação authorável (P2 §8) ---------------------------------------------
    //
    // Assets .anim.json em assets/animations são a FONTE DE EDIÇÃO; o
    // banco do RUNTIME (AnimationBank real) é preenchido no play() com
    // TODOS os clips do projeto — o AnimationTick REAL os executa sobre
    // o clone. O PREVIEW em Edit usa o MESMO sampler (AnimationSystem)
    // aplicando/restaurando o Transform da entidade em edição.

    /// Resumo de um asset de animação (para a UI listar).
    struct AnimSummary {
        std::string name;       ///< nome do arquivo (com .anim.json)
        std::string clip;       ///< nome do clip
        float duration = 0.f;  ///< segundos (último key)
        std::size_t frames = 0;     ///< keys de frame (flipbook)
        std::size_t keys = 0;       ///< keys TRS totais
        bool loop = true;           ///< default ao atribuir
    };

    /// Lista os assets de animação do projeto (parse leve p/ resumo).
    [[nodiscard]] eng::core::Result<std::vector<AnimSummary>>
    animationList() const;

    /// Lê o CONTEÚDO cru do asset (JSON — editor de texto / round-trip).
    [[nodiscard]] eng::core::Result<std::string> animationRead(
        std::string_view name) const;

    /// Escreve o conteúdo cru (valida com o codec ANTES de gravar —
    /// lixo não entra no projeto). Arquivo novo é catalogado no registry.
    [[nodiscard]] eng::core::Result<void> animationWrite(
        std::string_view name, std::string_view json);

    /// Cria uma animação NOVA (template mínimo com 2 frames placeholder
    /// em branco — sem textura; o authoring adiciona frames reais depois).
    [[nodiscard]] eng::core::Result<void> animationCreate(
        std::string_view name);

    /// Apaga o asset de animação (arquivo + registry).
    [[nodiscard]] eng::core::Result<void> animationDelete(
        std::string_view name);

    // --- materiais (P3 §3): assets/materials/<nome>.mat.json -------------
    //
    // O material REAL do sprite 2D (shader + tint): o renderer consome via
    // resolveMaterials (cache interno por nome — invalidada em write/delete
    // e troca de projeto).

    /// Resumo de um material (para a UI listar/inspetor).
    struct MaterialSummary {
        std::string name;    ///< nome do arquivo (com .mat.json)
        std::string shader;  ///< "lit" | "unlit"
        float tintR = 1.f;
        float tintG = 1.f;
        float tintB = 1.f;
        float tintA = 1.f;
    };

    /// Lista os materiais do projeto (decode completo p/ resumo).
    [[nodiscard]] eng::core::Result<std::vector<MaterialSummary>>
    materialList() const;

    /// Lê o conteúdo cru (JSON — round-trip do editor).
    [[nodiscard]] eng::core::Result<std::string> materialRead(
        std::string_view name) const;

    /// O material de um sprite
    /// CONTA como lit para os defaults coerentes da luz? Semântica EXATA
    /// do P4.6 Bloco 2: asset vazio = lit; cache frio = lit (default do
    /// engine); cache quente decide pelo shader resolvido.
    [[nodiscard]] bool materialCountsAsLit(const std::string& asset) const;

    /// Escreve o conteúdo cru (valida com o codec ANTES de gravar).
    [[nodiscard]] eng::core::Result<void> materialWrite(
        std::string_view name, std::string_view json);

    /// Cria um material NOVO (template "lit" com tint neutro).
    [[nodiscard]] eng::core::Result<void> materialCreate(
        std::string_view name);

    /// Apaga o asset de material (arquivo + registry). Sprites que o
    /// referenciam caem no default (lit neutro) — sem estado quebrado.
    [[nodiscard]] eng::core::Result<void> materialDelete(
        std::string_view name);

    /// Nomes dos materiais do projeto (linhas — picker do Inspector).
    [[nodiscard]] eng::core::Result<std::vector<std::string>>
    materialNames() const;

    /// Resolve o material de cada QUAD de sprite (P3 §3): shader em
    /// materialShader + tint do material MULTIPLICADO em tintR/G/B/A.
    /// Material ausente/vazio → default "lit" neutro (o look clássico).
    /// Cache por nome; best-effort (JSON ilegível → default + log único).
    void resolveMaterials(std::vector<EntityQuad>& quads) const;

    /// Anexa o clip à entidade: Animator.clip = nome do clip (adiciona
    /// Animator default quando ausente; cria SpriteData quando o clip
    /// tem FRAMES e a entidade não tem sprite — auto-criação SEGURA,
    /// P2 §14). Só em Edit.
    [[nodiscard]] eng::core::Result<void> animationAssign(
        eng::ecs::Entity entity, std::string_view name);

    /// Acrescenta um FRAME ao fim da track de frames do asset (authoring
    /// rápido: UI escolhe textura; o tempo é o último + 1/fps do meta).
    [[nodiscard]] eng::core::Result<float> animationAddFrame(
        std::string_view name, std::string_view textureAsset);

    /// Define o LOOP/FPS do asset (metadados).
    [[nodiscard]] eng::core::Result<void> animationSetMeta(
        std::string_view name, bool loop, float fps);

    // --- P4.6: autoraria de keys TRS (timeline v1) ----------------
    // `track` ∈ {"position", "rotation", "scale"}; rotation em GRAUS
    // (convenção do autor — decode/encode .anim.json idênticos). Key no
    // MESMO tempo (ε 1e-4) SUBSTITUI o valor (gravar de novo = atualizar).
    /// Grava um key (inserção ordenada por tempo). Erro: track inválida,
    /// tempo negativo/não-finito, asset ausente.
    [[nodiscard]] eng::core::Result<void> animationAddKey(
        std::string_view name, std::string_view track, float time, float x,
        float y, float z);
    /// Lista os keys da track em TSV "index\ttime\tx\ty\tz" (rotation em
    /// graus; ordenados por tempo).
    [[nodiscard]] eng::core::Result<std::string> animationKeyList(
        std::string_view name, std::string_view track) const;
    /// Edita key por índice (tempo + valores — mover/editar são o mesmo
    /// verbo honesto). Re-ordena a track se o tempo mudar.
    [[nodiscard]] eng::core::Result<void> animationKeySet(
        std::string_view name, std::string_view track, std::size_t index,
        float time, float x, float y, float z);
    /// Apaga key por índice.
    [[nodiscard]] eng::core::Result<void> animationKeyDelete(
        std::string_view name, std::string_view track, std::size_t index);

    /// PREVIEW em Edit (§8 "reproduzir preview"): aplica o clip na
    /// entidade por dt avançando o tempo; restaura o Transform original
    /// no previewStop. Só uma entidade por vez (preview explícito do
    /// usuário). Retorna erro quando clip/entidade inválidos.
    [[nodiscard]] eng::core::Result<void> previewStart(
        eng::ecs::Entity entity, std::string_view clipName);
    /// Avança o preview (host chama por frame junto do render). Fora de
    /// preview → no-op.
    void previewTick(float deltaSeconds) noexcept;
    /// Encerra o preview E restaura o Transform original da entidade.
    void previewStop() noexcept;
    [[nodiscard]] bool previewing() const noexcept
    {
        return preview_.has_value();
    }

    // --- áudio authorável (P2 §12) -----------------------------------------------
    //
    // AudioSource (componente do catálogo) + o AudioMixer REAL da engine.
    // O device output vem do backend do HOST (AAudio no Android; null em
    // testes — os contadores do mixer provam o caminho real de DSP).

    /// Mixer REAL (vozes/buses/stats) — o host conecta o backend nele.
    [[nodiscard]] eng::audio::AudioMixer& audioMixer() noexcept
    {
        return audioMixer_;
    }
    /// Toca/para um asset WAV (preview manual — P4.3/N1 TOGGLE): 1º toque
    /// toca no bus de PREVIEW (isolado das vozes de jogo no master); 2º
    /// toque no MESMO asset para. Outro asset para a anterior e toca o
    /// novo (uma única voice de preview existe — sem sobreposição).
    [[nodiscard]] eng::core::Result<void> audioPreview(
        std::string_view assetName);
    /// Para o preview IMEDIATAMENTE (idempotente — sem voice
    /// viva é no-op). Chamado ao fechar painel/mudar categoria/importar/
    /// entrar em Play — a voice de preview nunca sobrevive ao contexto.
    void audioPreviewStop() noexcept;
    /// Há voice de preview VIVA agora? (fonte de verdade do
    /// toggle na UI — a voice pode ter terminado sozinha).
    [[nodiscard]] bool audioPreviewPlaying() const noexcept;
    /// Sound decodificado do asset (cache do documento — o AudioTick e o
    /// host usam; pública para o tick da camada de composição).
    [[nodiscard]] eng::core::Result<
        std::shared_ptr<const eng::audio::Sound>>
    soundFor(std::string_view assetName);

    /// Resultado de uma checagem de compilação.
    struct ScriptCheck {
        bool ok = false;                ///< compilou até bytecode?
        std::vector<ScriptDiag> diags; ///< TODOS os erros coletados
    };

    /// Lista os scripts do projeto (assets/scripts), por nome de arquivo.
    /// Pré-requisito: projeto aberto.
    [[nodiscard]] eng::core::Result<std::vector<std::string>> scriptList()
        const;

    /// Lê o conteúdo de um script .nis. Erros precisos.
    [[nodiscard]] eng::core::Result<std::string> scriptRead(
        std::string_view name) const;

    /// Escreve o conteúdo do script (substitui bytes; cataloga no
    /// registry quando é arquivo novo). Valida nome/anti-traversal.
    [[nodiscard]] eng::core::Result<void> scriptWrite(
        std::string_view name, std::string_view content);

    /// Cria um script NOVO com o template canônico (força .nis; recusa
    /// nome vazio/duplicado).
    [[nodiscard]] eng::core::Result<void> scriptCreate(std::string_view name);

    /// Apaga um script (arquivo + registry).
    [[nodiscard]] eng::core::Result<void> scriptDelete(std::string_view name);

    /// Valida a fonte .nis SEM executar: compila com a MESMA tabela de
    /// nativos do runtime de Play (&BL + host padrão — o que o jogo vê).
    /// O Result falha apenas em erros INTERNOS; o veredito está em `ok`.
    [[nodiscard]] eng::core::Result<ScriptCheck> scriptCompile(
        std::string_view source) const;

    /// Anexa o script à entidade: NiScriptComponent.source = conteúdo do
    /// asset (adiciona o componente quando ausente). Só em Edit.
    [[nodiscard]] eng::core::Result<void> scriptAssign(
        eng::ecs::Entity entity, std::string_view name);

private:
    EditorDocument() = default;

    /// Estado do PREVIEW de animação em Edit (P2 §8): entidade + clip +
    /// transform ORIGINAL (restaurado no stop). Nulo = sem preview.
    struct PreviewState {
        eng::ecs::Entity entity{};
        std::string clip;
        float time = 0.f;
        bool loop = true;
        TransformDesc original{};  ///< restaurado no previewStop
    };
    std::optional<PreviewState> preview_{};

    /// Carrega TODOS os assets de animação no banco do runtime (play). Erros
    /// individuais viram WARN (o jogo roda com os clips válidos).
    void loadAnimationBank();

    /// Aplica a track de FRAMES do clip do Animator ao SpriteData das
    /// entidades (editor→SpriteData; a engine não conhece o componente).
    /// Usado no Play (clone) — mesmo código do preview em Edit.
    void applyAnimatorFrames(eng::scene::Scene& scene);

    /// Cache de Sounds decodificados (asset → Sound — o mixer retém o
    /// shared_ptr; recarrega se o asset mudou de tamanho).
    // (soundFor é público — ver seção de áudio.)

    /// Garantia de modo: TODA escrita de edição passa por aqui.
    [[nodiscard]] eng::core::Result<void> requireEditMode() const;

    // --- P4.5 (internos do histórico) ------------------------------------------

    /// Entrada do histórico: snapshot JSON + rótulo do gesto + instante
    /// (para coalescer gestos contínuos num único passo de undo).
    struct HistoryEntry {
        std::string snapshot;
        std::string label;
        std::chrono::steady_clock::time_point time{};
    };
    /// Captura o estado ANTES da mutação (chamar NO INÍCIO de cada op
    /// mutante, após requireEditMode). Coalesce por (label, janela).
    /// `suppressed` (interno) desliga a captura para ops aninhadas
    /// (createSprite→createEntity etc. — a entrada entra UMA vez).
    bool pushHistory(std::string_view label) noexcept;
    /// Copia a cena atual para JSON (SceneSerializer::save). False em erro
    /// (log + sem entrada — o undo daquela op não existe; honesto).
    bool captureScene(std::string& out) noexcept;
    /// Limpa o histórico (newScene/loadScene/openProject/newProject).
    void clearHistory() noexcept;

    std::deque<HistoryEntry> undoStack_;
    std::deque<HistoryEntry> redoStack_;
    /// Componentes ligados ao script `name` recebem a fonte nova.
    void syncLinkedScripts(std::string_view name, std::string_view content);
    bool suppressHistory_ = false;
    int historyGroupDepth_ = 0;
    /// Undo do GESTO de gizmo — armado no begin, limpo no end
    /// (drag sem mudança real remove a própria entrada).
    bool gizmoUndoArmed_ = false;
    std::uint64_t revisionAtDragBegin_ = 0;
    bool snapTranslate_ = false;
    bool snapRotate_ = false;

    /// Escrita de campo TRS do Transform pela UI do Inspector:
    /// "position.x" | "rotation.y" (GRAUS) | "scale.z" → API TRS — nunca
    /// direto no quat (graus em componente de quat = lixo decomposto).
    [[nodiscard]] eng::core::Result<void> setTransformField(
        eng::ecs::Entity entity, std::string_view fieldPath,
        std::string_view value);

    /// Meio-tamanho mínimo visível em mundo (zoom→mundo, §8.6).
    [[nodiscard]] float pxToWorldMin() const noexcept;

    /// Inversa 2x2 da parte linear do world matrix do PAI da entidade
    /// (bug §5 R2, P2): converte deltas de MUNDO → espaço LOCAL do filho.
    /// Raiz → identidade. Det degenerado → identidade (sem NaN).
    [[nodiscard]] std::array<float, 4> parentInverse2D(
        eng::ecs::Entity entity) const noexcept;

    /// Traduz handle de EDIÇÃO → handle da cena em FOCO (bug do clone
    /// aleatório: save ordena por SceneEntityId/UUID — ADR-033 — e o
    /// clone recria nessa ordem, que não casa com os índices da edição).
    /// Em Edit: identidade. Em Play: o clone correspondente quando o
    /// handle veio da edição (seleção pré-Play, JNI); handles NATIVOS do
    /// clone (tap em Play) passam direto — pass-through é o fallback.
    [[nodiscard]] eng::ecs::Entity toFocus(
        eng::ecs::Entity entity) const noexcept;

    eng::fs::FileSystem* fs_ = nullptr;  ///< emprestado
    eng::fs::Path workspaceRoot_{};
    /// Cache de materiais por nome (P3 §3 — resolveMaterials). Mutable:
    /// resolução é leitura com memoização; invalidada em write/delete.
    mutable std::unordered_map<std::string, eng::render::SpriteMaterial>
        materialCache_{};

    std::optional<eng::project::ProjectFile> project_{};
    bool projectDirty_{false};

    std::optional<eng::scene::Scene> scene_{}; ///< cena em EDIÇÃO
    bool sceneDirty_{false};
    /// Path da cena atual ("main.json") — P4.2/B-A: saveProject usa para
    /// persistir a cena junto do projeto; vazio = nunca salva/carregada.
    std::string currentScenePath_{};
    std::optional<eng::scene::Scene> runtimeScene_{}; ///< só em Play (clone)

    enum class Mode : std::uint8_t { Edit, Play };
    Mode mode_{Mode::Edit};

    std::optional<eng::ecs::Entity> selection_{};
    std::uint64_t selectionRevision_ = 0;  ///< bump p/ live sync
    /// Seleção da EDIÇÃO capturada no play() (P4.2/T5: "Stop volta ao
    /// editor com seleção intacta" — a seleção remapeada ao clone é
    /// descartada com ele; esta é restaurada no stop()).
    std::optional<eng::ecs::Entity> selectionBeforePlay_{};
    /// Runtime pausado (tick não avança; render continua).
    bool paused_ = false;

    /// Ferramenta ativa + gizmo. O estado de drag
    /// vive no DOCUMENTO (não na Activity): o ECS continua a única
    /// fonte de verdade autoral; o gizmo só calcula alvos.
    EditorTool tool_{EditorTool::Select};
    /// Instante da ÚLTIMA troca de ferramenta (pop 120ms).
    std::chrono::steady_clock::time_point toolChangedAt_
        = std::chrono::steady_clock::now() - std::chrono::hours(24);
    TransformGizmo gizmo_{};

    /// Contexto do DRAG: texturas capturadas no begin (o
    /// dragTo resolve bounds com a MESMA fonte — pivot consistente) e a
    /// inversa do pai no begin (delta de mundo → local). Ambos morrem no
    /// gizmoDragEnd — o gizmo nunca opera sobre estado obsoleto.
    TextureCache* dragTextures_{nullptr};
    std::array<float, 4> dragParentInv_{1.f, 0.f, 0.f, 1.f};

    /// Edição → runtime (construído no play() via SceneIdentity; vivo
    /// enquanto o clone existir — ver toFocus()).
    std::unordered_map<eng::ecs::Entity, eng::ecs::Entity> editToRuntime_{};

    /// Espelha a câmera de jogo ativa no viewport: copia
    /// posX/posY/zoom para `gameCamera_` e entrega ao viewport, ou devolve
    /// a câmera do editor quando a cena não tem câmera ativa.
    void syncGameCamera(const eng::tick::ActiveCamera& active) noexcept;

    /// P4.7.0 B6: filtro do logic LOD (thunk para o ponteiro de função —
    /// sem captura). false = o `up update` do script pula este frame.
    static bool lodFilterThunk(void* user, eng::ecs::Entity self);
    [[nodiscard]] bool lodFilter(eng::ecs::Entity self) const;

    eng::input::InputSystem runtimeInput_{}; ///< input do JOGO
    eng::physics::PhysicsWorld physicsWorld_{};      ///< §7.1–§7.6
    eng::physics::TimestepAccumulator physicsAccumulator_{1.f / 60.f};
    /// P4.7.0 B5: varredura do kinematic (default ON — ver kinematicSweep()).
    bool kinematicSweep_ = true;
    /// P4.7.0 B6: logic LOD (default OFF — ver logicLodEnabled()).
    bool logicLodEnabled_ = false;
    eng::animation::AnimationBank runtimeAnimations_{}; ///< §7.7–§7.11
    eng::audio::AudioMixer audioMixer_{};      ///< P2 §12 — mixer REAL
    /// Voice/asset do PREVIEW (uma única; bus isolado do jogo).
    eng::audio::VoiceHandle previewVoice_{};
    std::string previewAsset_;
    std::uint32_t previewBus_{0};              ///< bus "preview" (criado no create)
    std::unordered_map<std::string, std::shared_ptr<const eng::audio::Sound>>
        soundCache_{};                         ///< assets WAV decodificados
    std::unique_ptr<class NiRuntime> niRuntime_;     ///< §FASE 11 (clone)
    std::unique_ptr<eng::tick::TickScheduler> scheduler_{}; ///< P0-5 (Play)
    /// P4.7.0 B4: o CameraTick do clone (não-dono — o scheduler possui);
    /// recebe o tamanho da vista por frame (clamp pós-zoom dos limites).
    /// Nulo fora de Play; resetado no stop() junto com o scheduler.
    eng::tick::CameraTickSystem* cameraTick_ = nullptr;
    Viewport::Camera2D gameCamera_{};      ///< cache da câmera ativa
    bool gameCameraActive_ = false;        ///< último refresh achou câmera?
    Viewport viewport_{};
    std::unique_ptr<AssetBrowser> assets_{};
};

} // namespace eng::editor
