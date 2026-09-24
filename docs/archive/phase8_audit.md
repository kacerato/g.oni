# Auditoria FASE 8 — Native Mobile Editor

- **Base:** `f1bd4ce` (HEAD FASE 7), branch `main`, working tree limpa.
- **Baseline re-executada nesta sessão:** linux-debug e linux-release
  configurados do zero — 222/222 alvos, **zero warnings**, `ctest` **19/19**
  em ambos (drivers reais lavapipe/llvmpipe via gfx-sysroot reconstruído).
- **Ambiente restaurado** (máquina resetada pós-FASE 7): cmake 3.31.6 +
  ninja 1.13.2 (pip/venv), gfx-sysroot (mesa 25.0.7 lavapipe, validation
  layers 1.4.309, glvnd EGL/GLES), Android SDK cmdline-tools + NDK r27 +
  platform-34 + build-tools 34.0.0 + cmake SDK 3.31.6.

## 1. Inventario do que EXISTE e é reutilizável

| Sistema | Estado | Uso pelo editor |
|---|---|---|
| `eng::scene::Scene` | nós/hierarquia/transforms, `eachChild`, `destroyNode` cascata, `world()` | Hierarchy, operações de entidade |
| `eng::scene::SceneSerializer` | save/load JSON determinístico; catálogo `componentEntries()` (nome→has/encode/decode) | save/load de cena, **clone para PLAY/STOP**, catálogo de componentes |
| `eng::reflect` | `TypeInfo` com `PropertyInfo{name, offset, typeName}`; enums por nome; primitivas bool/i../u../f../string | **Inspector genérico** (get/set de campos por offset) |
| `eng::project::ProjectFile` | parse/serialize de `project.goni.json`; paths relativos obrigatórios | Project Manager |
| `eng::assets` | `AssetRegistry` (upsert/remove/serialize), `AssetId::generate()`, `AssetType` (Scene/Prefab/Json implementados; Texture..Script reservados) | Asset Browser |
| `eng::fs` | `FileSystem` (read/write/mkdirs/rename/remove/list recursive), `NativeFileSystem`, `MemoryFileSystem` | tudo de I/O (testes incluídos) |
| `eng::rhi` | `Renderer`/`Frame` (clear/viewport/pipeline/vbo/draw/present), buffers com `updateBuffer`, seleção Auto/Vulkan/GLES | Viewport (mesmo pipeline pos+cor do triangle) |
| FASE 7 runtime | `AndroidRuntime` state machine de surface, Choreographer na UI thread (ADR-039), `GoniJni.cpp` como padrão de fronteira JNI, shaders demo embutidos (GLSL ES + SPIR-V) | padrões de lifecycle/JNI/shader para o editor |

## 2. Lacunas (auditoria de código, não de intenção)

- **G1 — Entidades não têm nome.** `Scene` tem identidade (`SceneIdentity`
  UUID, FASE 3) mas nenhum componente de nome exibível. Hierarchy/Inspector
  exigem nomes legíveis.
- **G2 — Catálogo de componentes não constrói default.** `componentEntries()`
  tem `has/encode/decodeAndEmplace`; `decodeStruct` é ESTRITO com campos
  ausentes (ADR-030) — não há como adicionar um componente default-construído
  via catálogo (o Inspector precisa para "Add Component").
- **G3 — Não existe módulo editor.** Nenhum `EditorState/ProjectState/
  SceneState/SelectionState/InspectorState/ViewportState` (missão §8.9).
- **G4 — A abstraction RHI não tem uniforms.** `GraphicsPipelineDesc`/`Frame`
  não expõem uniforms/push-constants/descriptors (decisão FASE 4 — não era
  escopo). Um viewport com câmera 2D (pan/zoom) precisa transformar vértices.
- **G5 — Não existe app de editor Android.** FASE 7 entregou somente o
  runtime-demo (`GoniActivity` + `GoniRuntime`).
- **G6 — Não existe sessão de PLAY separada.** Nada separa estado de edição de
  estado de runtime (missão §8.7).
- **G7 — Asset Browser é composição inexistente.** Registry (catálogo) e fs
  (bytes) existem, mas não há descoberta/importação/renomeação/movimentação
  compostas (missão §8.5).
- **G8 — Inspector genérico não existe.** Nada lê/escreve campos por
  offset+typeName fora do serializer.

## 3. Decisões da auditoria (com justificativa)

- **D1 (G1):** novo componente `eng::scene::Name{std::string}` — no módulo
  `scene` (não no editor) porque nome de entidade é conceito do DOMÍNIO
  (FASE 11 NI-Script fará lookup por nome), registrado no reflect + built-in
  do `SceneSerializer` (persistido em cena). Aditivo, pequeno, testado.
- **D2 (G2):** `ComponentEntry` ganha `emplaceDefault` (fn-ptr que faz
  `world.emplace<T>(e, T{})`). Catálogo ÚNICO continua sendo o do serializer
  — o editor não cria um segundo registry. Aditivo.
- **D3 (G4):** viewport v1 transforma vértices na **CPU** (world→clip em C++,
  VBO dinâmico por `updateBuffer`) e reusa os shaders pos+cor das FASES 5–7.
  **Nenhuma mudança na abstraction RHI nesta fase** (uniforms serão desenhados
  quando o pipeline de render de jogo exigir — ADR registrado). Editor com
  centenas de quads: Custo CPU desprezível; correto e simples (§8.6
  "CORRETO > COMPLEXO").
- **D4 (G5):** editor Android = **Activity própria + Views nativos**
  (`android.widget`), SEM Compose/Flutter/Web. Razões: 3,9 GB RAM e 2 núcleos
  no host (build Compose é risco desnecessário), zero dependências de
  terceiros continua valendo (ADR-041), Views suprem tudo (listas, botões,
  dialogs) com alvos de toque ≥48dp. `EditorActivity` vira o LAUNCHER;
  `GoniActivity` permanece (demo do runtime, `exported=false`).
- **D5 (G6):** PLAY/STOP via **clone por serialização**: `SceneSerializer::
  save(editorScene)` → `load(runtimeScene)` (round-trip já testado FASE 3).
  Sem cópia estrutural manual; separação de estado por construção.
- **D6:** threading do editor = **igual FASE 7** (UI thread + Choreographer,
  ADR-039). §11 exige auditoria antes de tocar threading — não há evidência
  de necessidade (nenhuma aberração de frame no demo; editor é leve). Não se
  introduz render thread nesta fase.
- **D7 (G7):** import de arquivo externo via SAF (Kotlin copia bytes para o
  diretório do projeto) e o editor C++ só enxerga `eng::fs` — aquisição de
  arquivo é papel da plataforma, não da engine.

## 4. Fronteira JNI (estendida, ainda mínima por função)

A fronteira JNI existente (FASE 7, 8 funções) cobre runtime-demo. O editor
precisa de ~35 funções pequenas (documentadas 1:1 no ADR-042): projeção,
 cena, entidades, componentes/inspector, viewport, play/stop, assets. Regras
inalteradas: handle é o único objeto C++ que cruza a fronteira; strings são
copiadas para buffers locais no TU JNI; entidades cruzam como `jlong`
(index|generation empacotado — valor, não ponteiro); nenhum
std::string/std::vector/tipo RHI é exposto.

## 5. Riscos e mitigação

| Risco | Mitigação |
|---|---|
| Surface grande de JNI (≈35 fn) | cada fn ≤ 30 linhas, sem lógica — só marshalling; padrão GoniJni.cpp replicado |
| Estado editor × runtime confundido | `Mode::Edit/Play` único campo de modo; todas as operações passam por `sceneInFocus()`; edição em Play devolve erro explícito |
| RHI sem uniforms limita viewport futuro | ADR-042 registra decisão + gatilho de revisão (pipeline de render de jogo) |
| Views XML-less (programático) verboso | helpers de layout no Kotlin; app único, sem multi-ABI extra |
| GLES: atributos VAO dependem do VBO vinculado (lição FASE 6) | nenhuma mudança no backend; viewport usa o caminho já testado (bind vbo → draw) |
| Dispositivo físico indisponível | evidência por estágio (§13) — APK PACKAGED/INSPECTED; testes reais no Linux |

## 6. Testes existentes que NÃO podem regredir

19 suites: core, math, mem, log, reflect, events, jobs, ecs, scene, fs,
platform, serial, assets, project, rhi, rhi_vulkan, rhi_gles, integration,
android_runtime — +1 nova (`editor`) planejada.
