# G.ONI — Matriz de Funcionalidade (auditoria §23)

> Cada célula vazia era um defeito a investigar. Esta é a matriz viva do
> produto: o que existe de verdade, integrado, persistido, consumido pelo
> runtime, renderizado e testado em Android. Atualizada a cada reparo.

Commit de referência: `3cade4a` (P0-RECOVERY).

Legenda de status (§31): ✓ = verificado por teste/evidência;
C = apenas código (CODE_ONLY); — = não existe (e não se apresenta).

## Matriz de botões/UI (§23)

| UI element | Backend action | Persistence | Runtime effect | Android tested (CI) | Device tested |
|---|---|---|---|---|---|
| New Project (`Novo projeto…`) | `newProject` — cria estrutura + `project.goni.json` | ✓ registry/config em disco | ✓ projeto reabre | ✓ | **PENDENTE** |
| Open Project (`Abrir projeto…`) | `openProject` | ✓ | ✓ | ✓ | **PENDENTE** |
| Save Project (`Salvar projeto`) | `saveProject` | ✓ | ✓ | ✓ | **PENDENTE** |
| Project Settings (`Configurações…`) | `setProjectName` + dirty | ✓ | ✓ | ✓ | **PENDENTE** |
| New Scene (`Nova cena`) | `newScene` | — (cena em memória) | ✓ | ✓ | **PENDENTE** |
| Save Scene (`Salvar cena…`) | `saveScene` (relativo) | ✓ `scenes/*.json` | ✓ load restaura | ✓ | **PENDENTE** |
| Load Scene (`Carregar cena…`) | `loadScene` | ✓ | ✓ | ✓ | **PENDENTE** |
| Entity/Tick creation (`Nova entidade`) | `createEntity` + Name | ✓ serializado | ✓ quads refletem hierarquia | ✓ | **PENDENTE** |
| Hierarchy (rename/duplicate/delete/reparent) | `renameEntity`/`duplicateEntity`/`deleteEntity`/`reparentEntity` | ✓ | ✓ | ✓ | **PENDENTE** |
| Sprite (`addComponent SpriteData`) | catálogo refletido | ✓ | ✓ texturizado na tela — **tamanho/orientação/hit validados por PIXEL** (regressão P0) | ✓ (24 asserções visuais) | **PENDENTE** |
| Import (`Importar` — SAF) | staging → `import` | ✓ registry + arquivo | ✓ TextureCache | ✓ (§26 + nome final com extensão — B7/B8) | **PENDENTE** |
| Asset rename/move/delete (long-press) | `rename`/`move`/`remove` | ✓ id preservado (ADR-029) | ✓ | ✓ | **PENDENTE** |
| Asset search (`Buscar…`) | filtro local Kotlin | — | — | ✓ | **PENDENTE** |
| Asset preview (duplo-toque) | decode + bitmap | — | ✓ | ✓ | **PENDENTE** |
| Inspector (todos os campos) | `setComponentField` → reflection real | ✓ | ✓ quads/transform mudam | ✓ (12 casos) | **PENDENTE** |
| Texture picker | lista texturas reais do projeto | ✓ SpriteData.textureAsset | ✓ sprite usa | ✓ | **PENDENTE** |
| Color picker (hex) | escreve canais via fieldPath | ✓ | ✓ tint | ✓ | **PENDENTE** |
| Collider (add/edit) | componente registrado/serializado | ✓ | ✓ PhysicsTick | ✓ | **PENDENTE** |
| RigidBody / CharacterBody | idem | ✓ | ✓ timestep fixo | ✓ | **PENDENTE** |
| Script create (`+ Novo script`) | `scriptCreate` | ✓ arquivo + registry | ✓ | ✓ (§26 regression) | **PENDENTE** |
| Script editor (write/save) | `scriptWrite` (multi-KB) | ✓ | ✓ | ✓ (>512B test) | **PENDENTE** |
| Compile (`Compilar`) | lexer→parser→semântica→IR | — | ✓ diagnósticos line:col | ✓ | **PENDENTE** |
| Attach (`Anexar`) | `scriptAssign` → NiScriptComponent | ✓ serializado | ✓ PLAY roda a VM | ✓ | **PENDENTE** |
| Play/Stop (▶/■) | clone por serialização (ADR-044) | — (authoring intacto) | ✓ física/animação/partículas/scripts/câmera | ✓ | **PENDENTE** |
| Viewport gestures (tap/pan/zoom) | hit-test/câmera | — | ✓ | ✓ | **PENDENTE** |
| Move entity (drag) | `moveEntity` | ✓ via save | ✓ | ✓ | **PENDENTE** |
| Game touch em Play | input do jogo (§6.4 separado) | — | ✓ | ✓ | **PENDENTE** |
| Backend seletor (auto/vulkan/gles) | `setBackend` a quente | — | ✓ recria renderer | ✓ | **PENDENTE** |
| Camera (CameraData active/zoom) | componente + câmera de jogo | ✓ | ✓ toma o viewport em Play | ✓ | **PENDENTE** |
| 3D Object creation | **—** (não existe UI) | — | — | — | §15 do roadmap P1 |
| Material 3D | **—** | — | — | — | §17 do roadmap P1 |

## Matriz de pipeline (§1 — o loop exigido)

| Elo da cadeia | Status | Evidência |
|---|---|---|
| SOURCE CODE | ✓ | FASES 1–12 + P0-1..7, ~30 módulos eng::* |
| ENGINE API | ✓ | EditorDocument/AssetBrowser/Inspector contratos testados |
| EDITOR UI | ✓ | Activity completa (painéis hierarquia/inspector/assets/scripts) |
| PERSISTENCE | ✓ | §26 regression: JSONs sem paths absolutos, round-trip |
| RUNTIME | ✓ | Play roda física/animação/partículas/scripts/câmera sobre clone |
| APK | ✓ | CI Android verde; libgoni.so com o fix (auditado) |
| REAL ANDROID DEVICE | **PENDENTE** | Realme C33 — teste de aceitura §32 agendado (§33) |

## UI morta conhecida (eliminada neste ciclo)

- ~~"Carregar cena" listava diretório errado (sempre "Nenhuma cena salva")~~ — corrigido em `3cade4a`.
- ~~`.import_tmp` aparecia no seletor de projetos~~ — corrigido em `3cade4a`.
- UI 3D: **não existe** e não se apresenta como existente (botões 3D não
  foram criados — honestidade §24). Implementação é P1 após P0.5.

## Regra permanente

Um ✓ na coluna "Device tested" só entra quando o passo correspondente do
teste de aceitação (§32) passar no dispositivo físico, registrado em
`goni_android_validation.md`.
