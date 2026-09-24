# P4.6 — Colisão v2 · Luz · Grupos de Tick · Animação · Viewport/Grid/Orientação

HEAD base: `dca9922` (P4.5.2, device-verified). Fase em 6 blocos ordenados
(Bloco seguinte só com o anterior verde). Este documento é a adenda de
evidência por bloco com o status por item.

## Bloco 0 — R4 micro: versão visível

- `versionName "0.6.0"` / `versionCode 60` em `android/app/build.gradle.kts`.
- `buildFeatures { buildConfig = true }` + campos `PHASE_LABEL "P4.6"`,
  `BUILD_NAME "0.6.0"`, `BUILD_CODE 60`, `GONI_COMMIT` (hash injetado pelo
  CI via env `GONI_COMMIT`; builds locais ficam vazios — opcional por
  design).
- Splash: caption `P4.6` 12sp secundário sob o wordmark
  (`oni_splash_caption.png` por densidade, gerado por
  `scripts/gen_splash_caption.py P4.6` — regenerável por fase; o texto
  DINÂMICO real vive em BuildConfig, não no PNG).
- Sheet Configurações → "Versão": `P4.6 · build 0.6.0 (60) · <hash>` —
  100% BuildConfig, zero hardcode.

| Item | Status |
|---|---|
| versionName/Code/PHASE_LABEL via BuildConfig | VERIFIED (código; APK no fecho) |
| caption no splash drawable | VERIFIED (asset gerado; visual no device round 6) |
| linha Versão na sheet | VERIFIED (código; visual no device round 6) |

## Bloco 1 — Colisão v2 (padrões Godot/Unity)

- **BodyType** (`eng::physics::BodyType`): `Static` (nunca integra, mesmo
  `mass > 0`), `Kinematic` (integra SOMENTE velocidade autorada — sem
  gravidade/damping; empurra dinâmicos e não é empurrado),
  `DynamicLite` (comportamento integral pré-P4.6 — default). Refletido
  (Inspector = dropdown) e serializado.
- **Migração aditiva**: `EditorDocument::loadScene` injeta
  `bodyType` ausente em cenas pré-P4.6 (`mass==0 → Static`, senão
  `DynamicLite`) — idempotente, fast path para cenas novas. Teste com
  strip real da chave do JSON salvo.
- **Camadas de colisão nomeadas** no `project.goni.json` (chave aditiva
  `collisionLayers`, default `default` bit 1; bit = potência de 2, nome
  único; parse estrito). Document API: `collisionLayers()`,
  `setCollisionLayerName`, `addCollisionLayer` (menor bit livre).
- **Chips no Inspector**: `Collider.layer/mask` editam por NOME (chips
  multi-select, sync in-place B-B); bits fora da tabela aparecem em mono
  (nunca somem). A regra de interação `(A.mask & B.layer) &&
  (B.mask & A.layer)` JÁ era a matemática do par (`masksOverlap`,
  Physics.cpp:231) — agora com regressão bidirecional explícita e autoria
  nomeada.
- **NI-Script**: `move(dx,dy)` = translação CRUA (teletransporte —
  documentado no §07-bindings); `move_and_slide(dx,dy)` = varredura da
  esfera do CharacterBody com deslize, **substeps anti-túnel** (≤ meio
  raio, teto 64) e mask do PRÓPRIO corpo decidindo contra quem desliza
  (padrão Godot cinemático). Sem CharacterBody → fault PRECISO
  ("precisa de CharacterBody"), nunca deslize silencioso. Ambos operam
  sobre `self` em unidades de mundo.

| Item | Status |
|---|---|
| BodyType (3 tipos) integra/resolve por tipo | VERIFIED (7 testes de física) |
| Migração de cena antiga | VERIFIED (teste com strip real) |
| Tabela nomeada + persistência no projeto | VERIFIED (project + editor tests) |
| Chips layer/mask no Inspector | VERIFIED (código; visual no round 6) |
| move/move_and_slide (incl. fault sem CharacterBody) | VERIFIED (4 testes NI + física) |
| REPRO: script empurra vs estático → para/desliza/nunca atravessa | VERIFIED (e2e no Play real, 120 ticks) |
| Anti-túnel com parede fina | VERIFIED (movimento 6u > raio) |
| Accumulator de fixed timestep | VERIFIED (regressões P4.3 intocadas + verdes) |

## Bloco 2 — Light2D correta

- **Defaults coerentes**: luz nova casa com a camada da cena onde vivem
  os sprites lit (contagem por `LayerMember`; material vazio = lit;
  material explícito só conta com shader lit resolvido; empate = 1ª
  camada vista — determinístico).
- **Chips de layer na luz**: `Light2D.layer` é CAMADA DE CENA (a fonte
  real do filtro de iluminação é `DrawList::packUniformsFor` por camada,
  ADR-051) — agora autorável por chips single-select com os nomes REAIS
  da registry (impossível digitar camada inexistente). Bitfields de
  colisão NÃO foram duplicados na luz: seriam um segundo sistema de
  filtro conflitante com o ADR-051 (decisão arquitetural documentada).
- **Hint unlit**: material `unlit` no Inspector do sprite mostra
  "luzes não afetam shader unlit" (parse do `.mat.json` via
  `materialRead`).
- **Regressões**: readback lit≠unlit existente (`EditorTests` P3)
  intocado e verde; filtro de empacotamento por grupo + sanity
  intensidade/raio dos defaults (CPU).

| Item | Status |
|---|---|
| Defaults coerentes (UI/GAME/empate) | VERIFIED (2 testes) |
| Chips de camada da luz | VERIFIED (código; visual no round 6) |
| Hint unlit | VERIFIED (código; visual no round 6) |
| Readback lit≠unlit (topologia device = GLES) | VERIFIED no CI (GPU); device round 6 |

## Bloco 3 — IA de camadas

- Sheet "Ticks & Camadas" → **"Grupos de Tick"** (+ chip "+ Grupo",
  diálogos "Grupo:"): scheduling por camada da cena (ADR-051) — NADA
  mudou no comportamento, só o nome e a explicação honesta no painel
  ("Camadas de colisão (bitfields) ficam em Configurações").
- Migração de cenas existentes: `Collider.layer/mask` já eram refletidos
  pré-P4.6 → compatibilidade por construção; round-trip completo
  (bodyType Kinematic + bits + tabela nomeada sobrevivem a save/load).

| Item | Status |
|---|---|
| Sheet renomeada + separação conceitual | VERIFIED (código; visual no round 6) |
| Round-trip de colisão v2 sem quebrar cenas | VERIFIED (teste e2e) |

## Bloco 4 — Animação v1

- **Keys TRS autoráveis**: `animationAddKey` (mesmo tempo ε 1e-4
  SUBSTITUI; inserção ordenada), `animationKeyList` (TSV
  `index\ttime\tx\ty\tz` — rotation em graus), `animationKeySet`
  (editar+mover — re-ordena), `animationKeyDelete`. Erros precisos.
- **Serialização `.anim`**: codec EXISTENTE (`position/rotation/scale`
  tracks + `loop`) — a authoria escreve com o mesmo codec canônico;
  round-trip testado (grava/substitui/edita/move/apaga).
- **Playback no Play** via AnimationTick REAL (contrato P2 preservado: o
  autor liga `playing` no Inspector — assign não vira autoplay, flipbook
  intocado; `apply*` por track não-vazia).
- **UI**: menu da animação → "Keys TRS (timeline)…": chips Pos/Rot/Esc,
  tempo + "＋ Key da seleção" (captura o Transform corrente — graus),
  lista de keys com editar/mover/apagar; diálogo reabre a cada mutação
  (estado sempre fresco).

| Item | Status |
|---|---|
| API de keys (add/list/set/delete + erros) | VERIFIED (teste completo) |
| .anim round-trip | VERIFIED (mesmo codec; conteúdo assertado) |
| Playback no Play (position/scale + loop amarrado) | VERIFIED (e2e 180 ticks) |
| Timeline UI | VERIFIED (código; visual no round 6) |
| Flipbook coexistindo intocado | VERIFIED (suíte P2 100% verde) |

## Bloco 5 — Viewport, Grid v2, Orientação, Gizmos

### L1 Orientações sem bugs
- Portrait: layout atual preservado. Landscape: top bar COMPACTA (Cena/
  backend fora), tool pills LADO A LADO com o snap row (thumb zone
  horizontal), zoom/undo colados ao rodapé (12dp); painéis já eram side
  drawer à direita (46%, P4.5).
- **ZERO re-parent** (regra N2): só orientation/visibility/margins, com
  flip guardado (padrão B-B). Grid correto nos dois aspects por
  construção (projeção px-space N4 — OverlayMapper isotrópico).

| Item | Status |
|---|---|
| Chrome nas duas orientações | VERIFIED (código; device round 6 = fluxo completo) |
| Grid correto nos dois aspects | VERIFIED (projeção px testada; device round 6) |

### L2 Grid v2
- `GridConfig` no projeto: cell em **unidades de mundo** (default 1u),
  `majorEvery` (default 8 — "Primary Line Every" Godot), cores
  minor/major, show/hide. Persistido no `project.goni.json` (aditivo,
  validação estrita).
- **LOD adaptativo** (`computeGridLod` — função PURA): majors nunca mais
  densas que 10px (zoom-out escala o nível — majors viram minors);
  minors FADEAM 16→6px (lerp para o fundo — pipeline sem blending) e
  NEM desenham abaixo do fim do fade (anti-moiré duro); subdivisão
  emerge ao aproximar.
- **Eixos da origem coloridos**: X vermelho / Y verde (mesma família do
  gizmo de move). Major mais clara E mais grossa (2px vs 1.4px).

| Item | Status |
|---|---|
| LOD (zoom 48/2/1/0.5: fade, hide, subdivisão) | VERIFIED (função pura) |
| Config + persistência + validação | VERIFIED (project + editor tests) |
| Posição px das linhas | VERIFIED (projeção worldToScreen — regressões P4.3) |
| Visual no device (majors, fade, axes) | device round 6 |
| Settings da grade (UI) | VERIFIED (código; visual no round 6) |

### L3 Scale com setas
- 4 pontas direcionais nas arestas (28dp, clamped 22–34px), orientadas ao
  eixo local, apontando PARA FORA — coerentes com as setas do move.
- Alvos de toque ≥48dp: `hitPx = 24dp` de raio (48 ⌀) — VERIFIED (já
  era o contrato D2; re-assertado).

| Item | Status |
|---|---|
| 12 quads no SCALE (4 cantos + 4 marcas + 4 setas) | VERIFIED (layoutQuads CPU) |
| Setas apontam para fora | VERIFIED (as 4 direções) |
| Arrasto limpo (D4: aresta move UM eixo) | VERIFIED (regressão intocada) |

### L4 Estética de gizmos
- **Handles arredondados**: octógono EXATO (corte 45° com `h = c/√2`,
  c = 0.4·half — a face do corte É a corda, zero bleed).
- **Outline subtil**: rim da cor do fundo sob cada handle — contraste
  garantido sobre sprites claros.
- **Stroke 2dp consistente**: segmentos com meia-espessura `uiScale`
  (2px na densidade 1, 4px na densidade 2 — dp honesto).
- **Transição entre tools 120ms**: pop ease-out cúbico 0.88→1.0 nos
  handles (função pura + aplicada em `gizmoDraw`); hit-test INALTERADO.
- Contrato de contagem atualizado: MOVE = 5 handles × (outline+fill
  chamferados) + hastes = **324 verts** (era 54).

| Item | Status |
|---|---|
| transitionScale (pura, monótona, limites) | VERIFIED |
| gizmoHandlePop (clock) | VERIFIED (<1 logo após, ==1 após 140ms) |
| Contagem MOVE 324 | VERIFIED (readback GPU no CI; SKIP local sem driver) |
| Pixel central = handle sobre sprite | VERIFIED (readback no CI) |
| Estética final no device | device round 6 |

## Gates

- **Por bloco**: linux-debug + linux-release verdes (ASan+UBSan+Werror)
  com regressões novas — 31/31 suítes nas duas configs a cada commit de
  bloco (0+1, 2, 3, 4, 5).
- **Matriz JNI**: 129 símbolos Kotlin ⇄ 129 definições C++ — 0 faltantes,
  0 extras, 0 divergências (`scripts/jni_binding_matrix.py`); dlsym
  contract test com `STATIC_REQUIRE(kExpected == 129)`.
- **Gate anti-mangling** (ci-android): intocado — toda função JNI nova
  nasceu DENTRO do bloco `extern "C"`.
- **Preservação**: P4.5/P4.5.1/P4.5.2 intocados (micro-marks, guard,
  snap/fit, 120⇄120); B-A/B-B/N1/N2 sem regressão (suítes verdes);
  flipbook coexistindo; zero UI morta (todo controle novo escreve em
  estado real via JNI/documento).
- **CI final**: Linux #66 ✅ + Android #60 ✅ @ `cbacb72` (runs
  35721741580 / 35721741537). Fix intermediário: 3 erros de Kotlin no
  primeiro push (#59 falhou — `collisionLayerRow` via receiver +
  `Oni.pill` sem radius), corrigidos em commit cirúrgico.
- **APK do CI**: SHA256
  `530420bf46a781c22220c99c400d8e5a19f20e71a5d0bf7cb377de45e2a03380`.
  Auditoria local do artefato: **0 símbolos JNI manglados**, 139 JNI
  limpos, **9/9 novos do P4.6 presentes** (GetGrid/SetGrid/
  CollisionLayerList/SetCollisionLayerName/AddCollisionLayer/
  AnimationAddKey/KeyList/KeySet/KeyDelete), caption do splash nas 4
  densidades embutido.
- **Round 6 no C33** (usuário): 1. repro colisão; 2. luz em lit + hint
  unlit; 3. chips filtram colisão/luz; 4. timeline grava e reproduz no
  Play; 5. landscape fluxo completo; 6. grid majors/fade/axes; 7. scale
  com setas; 8. splash/settings mostram `P4.6 · 0.6.0`.
