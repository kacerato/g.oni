> **CORREÇÃO (auditoria final 4–10):** a prova de conteúdo do GLES
> NÃO usa `readCenterPixel` (não existe no editor — os contadores e o
> backend são a evidência); o EditorJni expõe 49 funções (não ~35);
> `EditorActivity` é `exported="true"` (launcher); cores são hue
> determinístico por entidade. A integração de input do jogo em Play foi
> corrigida (C-4/C-5/C-6: viewport do jogo + eventos brutos com pointer
> ID). Ver `docs/final_phase4_10_audit.md`.

# Design FASE 8 — Native Mobile Editor

- **Base:** `f1bd4ce` + auditoria `docs/phase8_audit.md` (decisões D1–D7).
- **Escopo (§17):** o primeiro editor NATIVO Android. Nada de Web/Flutter/
  export/loja/multiplayer/NI-Script.

## 1. Arquitetura

```text
Kotlin (EditorActivity — Views nativos: chrome touch)     ── SEM lógica de engine
        ↓ JNI (EditorJni.cpp — 2º TU com jni.h; marshalling only)
eng::editor (C++ novo, editor/ — testável no Linux)       ── ESTADO + COMANDOS
   ├── EditorDocument  (projeto/cena/seleção/modo/dirty)
   ├── Inspector       (reflect: campos por offset ↔ string)
   ├── AssetBrowser    (fs + AssetRegistry compostos)
   ├── Viewport        (Camera2D, hit-test, lista de quads)
   └── ViewportRenderer(RHI: pipeline pos+cor, VBO dinâmico CPU→clip)
        ↓
eng::scene/serial/project/assets/fs/reflect  (EXISTENTES — intocados)
        ↓
eng::rhi → backends Vulkan/GLES (FASES 4–7 — intocados)
```

Princípio §2/§9: **as APIs existem no C++**; a UI (Kotlin) apenas as utiliza.
Cada capacidade do editor é função C++ testada; o JNI só faz marshalling.

## 2. Mudanças aditivas em engine/ (auditadas D1/D2)

1. `engine/scene/include/eng/scene/Name.hpp` — `struct Name{std::string
   value;}` + `ENG_REFLECT` + registro built-in no `SceneSerializer`
   (`Name` vira componente persistido de cena). Hierarchy/Inspector/§18
   (NI-Script) usam nomes dele; default `"Entity"`.
2. `SceneSerializer::detail::ComponentEntry` ganha `emplaceDefault` (constrói
   `T{}` no world). `registerComponentType<T>` passa a preenchê-lo. Isso
   fecha G2 sem segundo registry.

## 3. editor/ — módulo C++ (consumidor de engine/, como android/runtime)

```text
editor/
  include/eng/editor/EditorDocument.hpp   # doc + comandos + play/stop
  include/eng/editor/Inspector.hpp         # campos reflect ↔ string
  include/eng/editor/AssetBrowser.hpp      # descoberta/import/operações
  include/eng/editor/Viewport.hpp           # Camera2D + hit-test + quads
  include/eng/editor/ViewportRenderer.hpp  # RHI: pipeline + VBO dinâmico
  src/*.cpp   tests/EditorTests.cpp   CMakeLists.txt
```

### 3.1 EditorDocument (dados e comandos, §8.9)

```text
struct EditorDocument {
    // projeto
    std::optional<ProjectFile> project;  // + fs dono (Native/Memory)
    bool projectDirty;
    // cena em EDIÇÃO
    Scene scene;  bool sceneDirty;
    // seleção
    std::optional<eng::ecs::Entity> selection;
    // modo
    enum class Mode { Edit, Play };
    Mode mode;
    std::optional<Scene> runtimeScene;   // existe só em Play (clone)
    // viewport
    Viewport viewport;                   // câmera + foco do hit-test
};
```

- `sceneInFocus()` → `&scene` ou `&*runtimeScene` (o único caminho de leitura
  do viewport; escrita de edição é REJEITADA em Play com erro explícito).
- Comandos (todos `Result`): createEntity(name, parent), deleteEntity,
  renameEntity, duplicateEntity (subtree; nomes com sufixo), reparent (ciclo
  → erro do `Scene::attach` propagado), setTransform (TRS + Euler↔Quat),
  addComponent/removeComponent (via catálogo; Transform/Name/Hierarchy são
  protegidos de remoção), select/deselect.
- Play/Stop (§8.7, D5): `play()` = `SceneSerializer::save(scene)` → nova
  `Scene` + `load` (clone) + `mode=Play`; `stop()` = descarta `runtimeScene`.
  O runtime cresce nas FASES 9/10 (input/audio/physics tick em Play).
- Snapshot de hierarquia (para JNI): caminhada depth-first com
  `{depth, packedId, name}`; seleção marcada à parte por id.

### 3.2 Inspector (reflect-driven, §8.4)

- Lista de componentes da entidade = `componentEntries()` filtrada por
  `has(world, e)`.
- Campos = `TypeInfo::properties`; valor lido por `offset` + `typeName`
  (bool/i32/f32/string direto; `eng::math::Vec3|Quat` → subcampos x..w;
  enums → nome do enumerador).
- `setField(entity, comp, "position.x", "2.5")` → parse por tipo → write por
  offset + `sceneDirty=true`. Erros precisos (campo desconhecido, valor
  inválido, entidade obsoleta).

### 3.3 AssetBrowser (§8.5)

- Categorias = `AssetType` (Scene/Prefab/Json/Texture/Mesh/Material/Shader/
  Audio/Script).
- `list(category)` = registry (`all()`) + varredura `fs::list(assetsRoot)`
  não catalogada (marcada "unregistered").
- `import(tempRelPath, category, name)` = move bytes para
  `assets/<category>/<name>` + `upsert(AssetMeta{generate()...})`.
- rename/delete/move = `fs::rename/remove` + upsert/remove no registry;
  persistência = `asset_registry.json` (formato existente).
- Tudo relativo ao root do projeto (§8.1 — paths absolutos proibidos).

### 3.4 Viewport + ViewportRenderer (§8.6, D3)

- `Viewport`: `Camera2D{posX, posY, zoom}`; `worldToScreen/screenToWorld`;
  hit-test top-most (itera snapshot em ordem inversa de profundidade);
  `buildQuads(scene)` → lista `{x, y, size, rot, color, selected}` do
  `computeWorldMatrix` (escala→tamanho do quad; posição do mundo).
- `ViewportRenderer` (dono do `Renderer` RHI, criado na surface):
  - shaders = **cópia exata** dos demo (procediência anotada, igual FASE 7);
  - VBO dinâmico: grid + quads + borda de seleção, tudo transformado
    world→clip na CPU (D3); `updateBuffer` por frame;
  - `render(...)` = begin → clear(cinza-escuro) → viewport → pipeline →
    vbo → draw → end → present; cores: câmera âmbar, jogador azul,
    padrão cinza-azulado; selecionado = borda branca; Play = borda verde.

## 4. Android (EditorActivity + EditorJni)

- `EditorActivity` (LAUNCHER, `com.goni.editor`... não — **mesmo package**
  `com.goni.runtime`, `exported=false`, lançada por ícone):
  toolbar (nome do projeto, SAVE, PLAY/STOP ≥56dp), barra de painéis
  (Hierarchy | Inspector | Assets | Viewport), painéis em `LinearLayout` +
  `ScrollView`/`ListView`, dialogs para rename/new/open/settings,
  `SurfaceView` para o viewport com detector de gestos (tap=seleção,
  drag=mover/pan, pinch=zoom, botão alternando mover/pan), teclado virtual
  tratado (`adjustResize` + scroll), 48dp mínimo de alvo de toque.
- `EditorJni.cpp` (TU com jni.h nº 2): ~35 funções de marshalling 1:1 com
  `EditorJni.kt`; listas atravessam como snapshot em texto TSV (uma chamada,
  zero tipos C++ expostos); entidades = `jlong` (index|generation).
- Loop de frame: Choreographer na UI thread (ADR-039 inalterado); render
  só com surface; em Play o mesmo loop (tick do runtime v1 = no-op
  documentado — FASES 9/10 o preenchem).
- Storage: projetos em `filesDir/projects/<name>` (interna, sem permissões);
  import via SAF copia bytes e o C++ só vê `eng::fs`.

## 5. Testes (§8.10 — todos no Linux, Catch2, suite `editor`)

- Projeto: new/open/save/round-trip/settings + paths relativos.
- Entidades: create/delete/duplicate/rename/reparent(+ciclo)/seleção.
- Hierarchy: snapshot order/depth/nome; deleção limpa seleção.
- Componentes: catálogo contém Name/Transform; add/remove; proteção dos
  core; inspector get/set (float/string/enum) + erros.
- Cena: save/load/undo sujo→limpo; dirty flags corretos.
- Play/Stop: clone idêntico; edição rejeitada em Play; mutação em Play
  (drag) não vaza para edição; stop descarta; segundo play re-clona.
- Viewport: matemática câmera ida-e-volta; hit-test; quads com hierarquia.
- AssetBrowser: import/list/rename/delete/move/registry (MemoryFileSystem).
- ViewportRenderer (rhi_hardware): GLES/llvmpipe — quad vermelho no centro
  lido por `readCenterPixel` (técnica FASE 6); Vulkan/lavapipe — contadores
  submitted/presented (sem readback na abstraction — evidência honesta).
- Zero warnings nos dois presets; suites 1–19 não removidas.

## 6. CMake

- Raiz: `add_subdirectory(editor)` sob `ENG_BUILD_TESTS` (como android/) —
  alvo `eng::editor` (STATIC) + `eng_editor_tests`.
- APK (`android/app/src/main/cpp/CMakeLists.txt`): adiciona `EditorJni.cpp`
  + fontes de `editor/` ao `libgoni.so` + `target_link_libraries(... eng::editor)`.
- ADRs: 042 (arquitetura do editor + Views + CPU-transform), 043 (Name +
  catálogo emplaceDefault), 044 (play/stop por clone de serialização).
