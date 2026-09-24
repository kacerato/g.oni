# Auditoria Completa do Engine — G.ONI (pré-evolução)

> Auditoria PHASE 0 da missão ENGINE EVOLUTION MASTER TASK. Nada neste
> documento é baseado apenas em nomes de arquivos: a implementação e os
> testes de cada módulo foram lidos no código-fonte. Esta auditoria é o
> ponto de partida do plano de evolução (2D/2.5D/3D, Tick, imagens,
> editor) — ela NÃO substitui a `final_phase4_10_audit.md`, que cobriu as
> FASES 4–10 em detalhe.

Classificações: **[A]** confirmado correto · **[B]** drift de
documentação · **[C]** bug real de código · **[D]** risco/limitação
arquitetural · **[E]** teste faltante · **[F]** extensão futura (NÃO é bug).

## 1. Baseline do repositório

| Item | Valor |
|---|---|
| Branch | `main` (única) |
| HEAD auditado | `889356c` — `docs: relatório final de evidências das FASES 11-12` |
| Working tree | limpa |
| Remote | `origin = github.com/criandojogodenovo-sketch/g.oni.git`, em sync |
| CI no HEAD | CI Linux: `success` (linux-debug + linux-release) · CI Android: `success` (assembleDebug arm64-v8a + inspeção de APK) |
| Histórico | 40 commits, FASES 1–12 completas, histórico preservado (sem rebase/amend/force) |

Cadeia de fases (verificada via `git log`): `d9b2d9d`(F1) → `8469f7c`(F2)
→ `2c87f7e`(F3) → `b36f350`(F4) → `24d3950`(F5) → `35ac978`(F6) →
`004aec4`(F7) → `9a144e4`(F8) → `969ac2d`(F9) → `f8314d8`(F10) →
`555ccc5`/`8be0be8`(auditoria+docs) → `524a3d8`(F11) → `87c5393`(F12) →
`889356c`(evidências).

## 2. Mapa de arquitetura real (lido do código)

```
eng::core ← eng::math ← eng::log ← eng::mem
    ↑
eng::reflect ← eng::events ← eng::jobs
    ↑
eng::ecs ← eng::scene ──────────────┐
    ↑                               │ (SceneSerializer, catálogo único)
eng::fs ← eng::platform             │
    ↑                               │
eng::serial ← eng::assets ← eng::project
    ↑
eng::rhi (Renderer/Frame) ← backends {vulkan, gles}   [sem texturas — ver §4]
    ↑
eng::input · eng::ui (sem consumidor) · eng::audio (sem consumidor)
eng::physics · eng::animation · eng::particles
eng::niscript (lexer→parser→sema→compiler→VM→bindings)
eng::build (manifest→cook→cache→bundle→export)
    ↑
editor/ (EditorDocument/Inspector/AssetBrowser/Viewport/ViewportRenderer/
         NiRuntime/EditorHost)  ←— ADR-042/043/044
    ↑
android/ (EditorJni.cpp + EditorActivity.kt; GoniJni.cpp + GoniActivity.kt
          = runtime-demo TRIÂNGULO; AndroidRuntime state machine)
```

Módulos: 22 em `engine/` + `editor/` + `android/`. ~48.000 linhas
C++/Kotlin. 28 suites de teste.

## 3. O que FUNCIONA e é verificável [A]

| Subsistema | Evidência |
|---|---|
| Fundação (core/math/mem/log/reflect/events/jobs/ecs/scene) | FASES 1–2, testes densos, TSan no jobs |
| Serialização (JSON + envelope GONI + migrations + StructCodec) | FASE 3, round-trip testado, corrupção testada |
| Assets (AssetId UUIDv4, Registry, Resolver) | FASE 3, determinístico |
| RHI buffers/shaders/pipelines/frames | FASE 4, validação completa; Vulkan real (F5, lavapipe no CI); GLES real (F6, EGL+llvmpipe no CI) |
| Android runtime lifecycle/JNI/surface | FASE 7, APK arm64-v8a zero permissões no CI |
| Editor document model (Edit×Play por clone) | FASE 8/ADR-044, 35 casos/354 asserções incl. play de scripts |
| Input canônico (multitouch por pointer ID) | FASE 9 + correções C-5/C-6 da auditoria final |
| Física (esfera/box, layers/masks, triggers, raycast, timestep fixo, CharacterBody) | FASE 10 + remediação |
| Animação TRS (clips, cross-fade aplicado, state machine) | FASE 10 + correção C-13 |
| Partículas CPU determinísticas | FASE 10 + drift D6 corrigido (viewport desenha) |
| NI-Script (linguagem→VM determinística→bindings ECS) | FASE 11, 58 casos/516 asserções |
| Build/export (manifest FNV-1a, cook GONI, cache conteúdo-endereçado, bundle verificado, determinismo byte-a-byte) | FASE 12, 23 casos/232 asserções |
| CI Linux (matrix debug/release) + CI Android (APK+inspeção) | Verdes no HEAD |

## 4. Achado central — o CAMINHO CRÍTICO bloqueia TUDO que é visual

A cadeia **imagem → textura → sprite → render** não existe em NENHUM
nível. Isto é o gargalo que bloqueia 2D real, 2.5D, 3D, thumbnails,
preview de asset e UI com imagens:

1. **RHI não tem texturas** [D]: `RhiBackend.hpp` não possui
   `createTexture`/`createSampler`/binding de textura; `Types.hpp` não
   tem `TextureHandle`/`TextureDesc`/`SamplerDesc`. `maxTextureSize`
   existe em capabilities mas NADA o consulta. Os backends Vulkan/GLES
   não têm caminho de upload de imagem.
2. **ViewportRenderer é pos+cor** [D]: `editor/src/ViewportRenderer.cpp`
   desenha quads de COR SÓLIDA (vértice = pos vec4 + cor vec4; sem UV,
   sem blending). Grade, entidades e partículas são quads coloridos.
3. **Assets Texture/Mesh/Material/Shader/Audio/Script são enums
   RESERVADOS sem loader** [D]: `AssetType.hpp` declara os valores
   (100–105) com "SEM loader nesta fase" no comentário. A categoria
   "textures" do AssetBrowser cataloga BYTES crus — sem decode, sem
   dimensões, sem formato, sem preview.
4. **Não existe componente visual** [D]: `buildQuads()` (Viewport.cpp)
   deriva tamanho/rotação da TRANSFORM e a cor de `hueOf(entity.index)`
   — "marcador, não classificador". Não há Sprite/Mesh/Material/Camera/
   Light como componentes; a entidade É o retângulo colorido.
5. **Sem decode de imagem**: nenhum PNG/JPEG decoder no repositório.

**Consequência**: o workflow "importar imagem → ver na cena" da missão é
IMPOSSÍVEL hoje — e é P0.

## 5. Análise pelos 18 pontos da missão

**1. O que já funciona** — §3 (fundação, serialização, RHI de buffers,
VM NI-Script, pipeline de build, editor document model, CI honesto).

**2. Parcialmente implementado**:
- Física: só esfera/box; sem círculo 2D, cápsula, polígono, mesh 3D [F/D]
- Áudio: WAV PCM (8/16/24/32f), mixer, AAudio — mas SEM consumidor de
  runtime (limitação registrada na FASE 9) [D]
- UI: widgets/draw-list/font 5×7 — sem consumidor de runtime [D]
- Animação: TRS property-tracks com cross-fade; skeletal = extensão [F]
- Editor: hierarquia/inspector/assets/viewport funcionam, mas primitivos (§6)

**3. Placeholder**:
- Entidades = retângulos coloridos (hue por índice) — "colored rectangles
  pretending to be images" exatamente como a missão proíbe [C para o
  objetivo da missão; foi desenho intencional da FASE 8, agora é dívida]
- `GoniActivity` = demo triângulo FASE 7 (não carrega bundles!) [D]
- AssetType Texture/Mesh/Material/Shader = reservados sem implementação [D]

**4. Testado apenas em software rendering** — Vulkan/GLES do CI rodam em
lavapipe/llvmpipe; no dispositivo Realme C33: NUNCA testado (sem KVM/
device no ambiente — declarado honestamente em todas as fases) [E].

**5. Implementado mas não exposto no editor**:
- `eng::build` inteiro (export não tem UI/botão — só testes) [D]
- AnimationBank do runtime (clips só via API de teste) [D]
- InputActionBindings (input.json não é editável no editor) [D]

**6. Implementado mas inalcançável**:
- **Bundles exportados não têm CONSUMIDOR**: `verifyBundle()` é usado
  por testes; nenhum runtime carrega um bundle e roda o jogo [D crítico]
- eng::ui/eng::audio sem consumidor (reconhecido) [D]

**7. Visualmente quebrado** (estrutural; validação em device pendente):
- Botões 48–52dp em toda a UI ("oversized buttons") [C]
- Painel de fundo fixo de 300dp — em paisagem come metade da tela [C]
- Sem tratamento de WindowInsets/status/nav bar [C]
- Sem layout responsivo portrait×landscape (só `configChanges` evita
  recriação da Activity; o FrameLayout não se adapta) [C]
- Sem ícones, sem thumbnails, sem hierarquia visual, sem identidade [C]
- Text-only Lists com `minHeight=48` por linha (densidade baixíssima) [C]

**8. Arquitetura inconsistente**:
- O conceito de domínio é "Entity" genérico (nome default "Entity" →
  "Entity Empty" feeling da missão); NÃO existe Tick/taxonomia de tipos
  (SpriteTick, CameraTick, MeshTickObject...) [D]
- Sem LINKS entre entidades (só parent/child da hierarquia) [D]
- Sem LAYERS (GAME/SUBGAME/...) — camada não é cidadã [D]
- Sem CameraTick — a câmera do viewport é estado do EDITOR, não da cena [D]
- Shaders do editor são fixtures duplicados do triângulo (sem sistema de
  shader asset) [D]

**9. Faltando para 2D**: sprites/texturas (§4), região/pivot/flip/tint/
sort/z-order, tilemap, física 2D além de box/sfera (círculo, edge,
polígono), câmera de jogo com limites/segue, spritesheet/frames.

**10. Faltando para 2.5D**: TUDO — sem câmera ortográfica 3D, sem
billboards/sprite-facing-camera, sem profundidade/parallax/layered worlds.

**11. Faltando para 3D**: TUDO — sem mesh asset/loader (OBJ/glTF), sem
pipeline de mesh no RHI (só VBO dinâmico de quads), sem materiais, sem
luzes, sem câmera perspectiva, sem skeletal, sem terreno.

**12. Faltando para Android**: insets, UX portrait/landscape real,
gizmos de toque (rotate/scale), console de saída/erros no editor,
carregamento de bundle no runtime (§6), teste em dispositivo físico.

**13. Faltando para scripting**: UI de script no editor (ADIADA e
declarada na FASE 11 — a missão exige como P0), visual scripting,
edição de input bindings, asset de script (hoje script vive em
componente de entidade, não em arquivo .nis do projeto editável).

**14. Faltando para assets**: decode de imagem (PNG/JPG), metadados
(dimensões/formato/alpha), thumbnails no browser, preview ao abrir,
import de mesh (OBJ/glTF), OGG, atribuição de asset a objeto (o
workflow "assign texture to sprite" não existe).

**15. Faltando para animação**: editor de animação (timeline, keyframes),
sprite-frames, skeletal (hierarchy/clips/blending/IK são extensões).

**16. Faltando para terreno**: TUDO (TerrainTick, chunks, escultura,
paint layers, colisão, LOD).

**17. Faltando para materiais/texturas**: TUDO (o conceito não existe no
runtime; só nome de categoria no browser).

**18. Faltando para editor UX**: modos (Scene/2D/3D/Script/...), painéis
colapsáveis/drawers, ferramentas de viewport além de PAN/MOVE (rotate/
scale/focus/frame/snap), gizmos, output/debug (FPS, frame time, draw
calls), Undo/Redo, multi-seleção.

## 6. Bugs de documentação encontrados [B]

1. `README.md` roadmap: linhas das FASES 11 e 12 dizem `planejada` —
   mas o texto acima diz "as 12 fases do roadmap estão CONCLUÍDAS" e as
   seções FASE 11/12 existem completas. Drift.
2. `README.md` diz "CI Linux executa os 27" vs "28 suites" no mesmo
   parágrafo (contagem inconsistente).
3. `EditorShaders.hpp` documenta que `gen_editor_shaders.py` não existe
   (corrigido na auditoria final — OK, mantido como histórico).

## 7. Plano de evolução (prioridade da missão)

### P0 — corrigir o editor móvel e o caminho visual crítico
1. **RHI: texturas + samplers + blending** — `TextureHandle`,
   `TextureDesc` (formato, mips, dados iniciais), `SamplerDesc`,
   `createTexture/updateTexture/createSampler/frameBindTexture` nos 3
   backends (Vulkan staging+descriptor, GLES glTexImage2D, Fake p/ testes).
2. **Image pipeline** — decoder PNG/JPEG (dependência terceira via
   FetchContent com URL_HASH, padrão Catch2/nlohmann), loader de asset
   Texture (metadados: dimensões, canais, alpha), categoria funcional,
   preview no editor, thumbnails.
3. **Sprite real** — componente `SpriteData` (assetId de textura, região
   UV, pivot, flip, tint, opacity, sort/z), shader texturizado (GLSL+SPIR-V),
   pipeline com blending, render de sprites no viewport (e não de quads hue).
4. **Editor mobile rework** — identity dark compact, toolbars 40dp,
   insets, portrait/landscape adaptativo, painéis como drawers/sheets,
   viewport dominante, listas densas com ícones/typography.
5. **Tick architecture** — taxonomia de tipos de Tick sobre o ECS
   (composição, não árvore de herança), links tipados com handles estáveis
   + detecção de ciclo, layers GAME/SUBGAME/Named com participação
   (update/física/render), CameraTick.
6. **Inspector/Asset Browser** — campos por categoria, enums/bools/cores
   reais, busca; browser com thumbnails/metadados/preview.
7. **NI-Script UI** — painel de script (lista/editar/compilar/diagnósticos),
  asset .nis do projeto.

### P1 — 2.5D, 3D core, física, animação, luz
Câmera perspectiva/orto no RHI + viewport 3D (gizmos, grid, navegação),
mesh assets (OBJ primeiro, glTF depois), materiais (base color, texturas,
alpha mode), luzes direcional/ponto/spot + ambiente, física 3D
(box/sphere/capsule), character body 3D, spritesheet anim 2D, animation
editor mínimo.

### P2 — terreno, splines, visual scripting, streaming
TerrainTick com chunks/heightmap/escultura/paint/colisão/LOD, splines,
nó-grafo visual scripting interoperável com NI-Script, arquitetura de
streaming.

### Consumidor de runtime (transversal P0/P1)
O runtime Android deve carregar BUNDLES exportados (cena+assets+scripts)
e rodar o jogo — hoje só existe o demo triângulo. Sem isso, o pipeline
de export é um cano sem água.

## 8. Gate de aceitação (resumo)

Cada entrega P0 exige: implementação + testes unitários + integração +
CI verde (Linux+Android) + build de APK + inspeção do APK + (quando
disponível) device. Nada é marcado COMPLETE por "build passou".
DEVICE TESTED = NO até que um dispositivo físico valide (Realme C33).
