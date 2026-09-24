# P4.7.0 — Contrato de Componentes · Eventos · Câmera · Física Varrida · Performance

HEAD base: `6c2c454` (P4.6 fechado em `cbacb72` + evidência CI). Fase em 6
blocos ordenados (bloco seguinte só com o anterior verde). Este documento é
a adenda de evidência por bloco. Feedback driver: round 6 (gizmo "cubo",
UI sobreposta, colisão sem resposta para script ingênuo, sem câmera, sem
culling/governor) + auditoria de arquitetura (sem contrato de componente,
sem hooks, sem eventos tipados).

## Bloco 0 — Versão visível

- `versionName "0.7.0"` / `versionCode 70` em `android/app/build.gradle.kts`.
- BuildConfig: `PHASE_LABEL "P4.7.0"`, `BUILD_NAME "0.7.0"`, `BUILD_CODE 70`,
  `GONI_COMMIT` (injetado pelo CI; builds locais vazios — opcional por design).
- Splash: caption `P4.7.0` regenerada por `scripts/gen_splash_caption.py P4.7.0`
  (o texto DINÂMICO real vive em BuildConfig; PNG é asset, não fonte).
- Sheet Configurações → "Versão": `P4.7.0 · build 0.7.0 (70) · <hash>` —
  100% BuildConfig (EditorDialogs.kt), zero hardcode.

| Item | Status |
|---|---|
| versionName/Code/PHASE_LABEL via BuildConfig | VERIFIED (código; APK no fecho) |
| caption no splash drawable | VERIFIED (asset gerado; visual no round 7) |
| linha Versão na sheet | VERIFIED (código; visual no round 7) |

## Bloco 1 — ComponentContract v2 + hooks + eventos tipados

### Contrato (`eng::scene::detail::ComponentContract`)

- `{required, conflicts, single, category, scriptAlias}` registrado NO
  catálogo único do serializer (ADR-043 — uma fonte só para Inspector,
  NI-Script e validação). `requires` do contrato chama-se `required` em
  código (palavra-chave C++20); a semântica é a mesma.
- **Add com erro preciso** (`Inspector::addComponent`):
  - `required` ausente → `"X exige Y — adicione Y antes"`
  - `conflicts` presente → `"X conflita com Y na mesma entidade — remova Y primeiro"`
  - `single` com instância viva → `"X é único na cena (single)"` (via
    `ComponentEntry::count` — novo ponteiro `count(world)` type-erased)
  - Add é ATÔMICO: contrato violado ⇒ nada é anexado.
- **Remove com dependência reversa** (`Inspector::removeComponent`):
  outro componente presente que `required` o removido ⇒ recusa com o
  NOME do dependente (`"não é possível remover Collider: CharacterBody
  exige este componente — remova CharacterBody primeiro"`).
- Contratos são APLICAÇÃO DE AUTORIA; o LOAD não re-injeta dependências
  (cenas salvas já satisfazem; hand-edits abrem, nunca falham por contrato).

### Contratos nativos registrados (ComponentRegistration.cpp)

| Componente | Categoria | Apelido NI-Script | requires/conflicts/single |
|---|---|---|---|
| eng::math::Transform | Transform | — | — |
| eng::scene::Name | Transform | — | — |
| eng::scene::LayerMember | Lógica | layer | — |
| eng::editor::SpriteData | Render | sprite | — |
| eng::physics::RigidBody | Física | rigidbody | conflicts CharacterBody |
| eng::physics::Collider | Física | collider | — |
| eng::physics::CharacterBody | Física | character | requires Collider; conflicts RigidBody |
| eng::animation::Animator | Lógica | animator | — |
| eng::particles::ParticleEmitter | FX | particles | — |
| eng::editor::NiScriptComponent | Lógica | script | — |
| eng::tick::CameraData | Câmera | camera | — (múltiplas permitidas) |
| eng::editor::AudioSource | Áudio | audio | — |
| eng::render::Light2D | FX | light | — |

- Ordem fixa de categorias: Transform → Render → Física → Lógica → Áudio →
  Câmera → FX → Outros (`Inspector::catalogEntries()`; JNI
  `nativeEditorComponentCategories` → dialog "Adicionar componente"
  AGRUPADO por categoria com cabeçalhos não-clicáveis; busca ativa = lista
  plana). Apelidos: contrato primeiro; apelido legado (cauda minúscula do
  nome) CONTINUA registrado — scripts antigos nunca quebram.
- Hints de dependência do dialog vêm do CONTRATO primeiro
  (`exige eng::physics::Collider`), legacy depois.

### Hooks (`onAttach` / `onDetach` / `onValidate`)

- Registrados junto do tipo (`registerComponentType` com contrato+hooks, ou
  `registerComponentContract` p/ built-ins). `hookUser` = contexto do
  chamador (o documento passa `this`).
- **Registro NATIVO de efeitos colaterais vive SÓ nos hooks** — o caso
  especial "Light2D casa com a camada dos sprites lit" MIGROU do
  EditorDocument para `light2DAttach` (mesma semântica do P4.6 B2:
  contagem por LayerMember, material vazio = lit, cache frio = lit,
  vencedor = maior contagem; novo acesso `EditorDocument::
  materialCountsAsLit`). Testes P4.6 continuam verdes.
- **onValidate** (`Collider`): radius/halfExtents negativos recusados no
  `setField` com ROLLBACK pelo valor anterior (round-trip string neutro) —
  o componente nunca fica quebrado. No add: default inválido ⇒ rollback do
  anexo. No **play()**: validação completa da cena ANTES do clone
  (`"não é possível entrar em Play: X no nó N é inválido — motivo"`) —
  cenas corrompidas por caminho externo não entram em jogo.
- onDetach: roda pós-remoção bem-sucedida (nenhum nativo precisa em B1;
  mecanismo + testes prontos).

### Eventos tipados (`Scene::events()` + `eng::scene::SceneEvents.hpp`)

- `eng::events::EventBus` (ADR-022) agora VIVE na cena
  (`scene.events()`; scene → events PUBLIC).
- `HitEvent{self,other,nx,ny}` — publicado pela física a cada contato
  resolvido, NOS DOIS SENTIDOS (self/other trocados; normal aponta de
  other para self). `TriggerEvent{self,other,entered}` — diff de pares de
  trigger entre passos (`on_enter` uma vez, `on_exit` ao separar; pares
  canônicos, determinístico). `VisibilityEvent{entity,visible}` — evento
  definido; o PUBLISHER é o culling do Bloco 6.
- **Bridge NI-Script** (`NiRuntime`): inscreve no barramento no start;
  `up on_hit:` / `up on_enter:` / `up on_exit:` / `up on_visible:` /
  `up on_invisible:` rodam nas instâncias cujo `self` == entidade do
  evento (inline durante o publish — ordem determinística da física;
  guarda anti-reentrância). Faults contam em `NiScriptStats` como update.
  Inscrições são RAII e morrem no shutdown (nunca sobrevivem à cena de Play).

### JNI/Kotlin

- Novo: `nativeEditorComponentCategories(handle)` — TSV
  `name\tcategory\talias` (contrato JNI por dlsym atualizado: 130 símbolos).
- Dialog "Adicionar componente" agrupado por categoria (cabeçalhos em
  acento, não-clicáveis; índice de clique vem da lista paralela
  `selectable` — nunca da lista exibida).

### Legado ajustado (comportamento intencional)

- `p46: REPRO` (CharacterBody): ordem de autoria atualizada para
  Collider ANTES de CharacterBody — o contrato ensina a ordem certa
  (antes o add silenciosamente aceitava e `move_and_slide` não tinha
  esfera pra varrer).

| Item | Status |
|---|---|
| requires/conflicts/single com erro preciso + add atômico | VERIFIED (5 testes) |
| Remove com dependente nomeia quem exige | VERIFIED |
| Categorias em ordem fixa + catalogEntries | VERIFIED (2 testes) |
| onValidate rollback + play() recusa cena inválida | VERIFIED (2 testes) |
| Hook da luz (mesma semântica P4.6) | VERIFIED (testes P4.6 + novo) |
| HitEvent dois sentidos / trigger enter-exit | VERIFIED (3 testes física) |
| Bridge on_hit roda no self atingido | VERIFIED (e2e doc+script) |
| Bus da cena publish/subscribe/RAII | VERIFIED |
| Dialog agrupado por categoria | VERIFIED (código; visual no round 7) |
| on_visible/on_invisible (publisher no B6) | PARCIAL (bridge pronto) |

## Bloco 2 — Gizmos v3: setas REAIS, hit ≥48dp, halo, anti-sobreposição

Driver: round 6 — "o gizmo parece um cubo, não setas" (as pontas eram
quadrados girados) e handles colapsando sobre o centro em bounds pequenos.

- **Setas reais** (`GizmoTriangle` — novo primitivo do gizmo; renderer
  emite 3 vértices): MOVE = 4 pontas triangulares apontando PARA FORA
  (16dp de comprimento, 14dp de base, halo incluído), hastes 2dp
  terminando na BASE do triângulo (nunca através). Rotate = anel + handle
  TRIANGULAR tangente (aponta na direção de crescimento do ângulo; o
  chevron de segmentos saiu — o triângulo É a seta). Scale = 4 cantos
  (quadrados, escala XY) + 4 marcas de aresta + 4 setas triangulares de
  aresta apontando para fora ao longo do eixo local.
- **Centro DIAMANTE** no MOVE (quadrado a 45°) — affordance distinta dos
  cantos quadrados do SCALE.
- **Halo 1dp** sob TODA forma do gizmo (quads já tinham rim; agora
  segmentos recebem linha bg mais grossa por baixo e triângulos halo
  próprio) — contraste garantido sobre sprites claros.
- **Anti-sobreposição clamp** (`scaleHandlePoints`, fonte ÚNICA de
  hit-test e desenho): bounds pequenos empurram cantos para fora até
  52dp do centro e arestas até 44dp — o cluster de cantos nunca mais
  colapsa num "cubo" sobre a entidade. Drag continua 1:1 screen-space
  (segue o POINTER, não o handle clampado).
- **Hit ≥48dp mantido** (kHitDp 24 → ⌀48dp) e agora SEMPRE coincidente
  com o visual desenhado (mesma `scaleHandlePoints`).
- **Camada de desenho**: grid → sprites (+borda de seleção) → gizmo
  (quads → segmentos → triângulos) → HUD do Play — inalterada na ordem
  macro, triângulos por cima das hastes dentro do lote.

| Item | Status |
|---|---|
| Setas triangulares reais (move/scale/rotate) | VERIFIED (3 testes de layout) |
| Centro diamante | VERIFIED |
| Hastes terminam na base do triângulo | VERIFIED |
| Clamp anti-sobreposição (hit == visual) | VERIFIED (2 testes) |
| Halo 1dp (quads+segmentos+triângulos) | VERIFIED (código; visual no round 7) |
| Drag 1:1 preservado (regressões P1/P2 verdes) | VERIFIED (suite completa 3×) |

## Bloco 3 — zero sobreposição: top bar por conteúdo + auditoria

Driver: round 6 — chips truncados/quebrados ("Cen a", "a u t").

- **Causa raiz**: chips com `LinearLayout.LayoutParams(0, 48dp, weight)` —
  largura FORÇADA pelo weight + texto sem single-line = quebra no meio do
  rótulo (era sobreposição/ilegibilidade, não "chip curto").
- **Correção estrutural** (`buildUi`): `btnScene`/`btnBackend` passam a
  `WRAP_CONTENT` (chip tem o tamanho do CONTEÚDO, regra §1.2), mola
  (`weight=1`) empurra ▶/backend para a direita; `Oni.chip` ganha
  `isSingleLine + maxLines=1` — quebra de linha IMPOSSÍVEL em qualquer chip.
- **Compação reativa** (`updateTopBarCompaction`): o listener de layout da
  top bar compara largura necessária × disponível (teclado/landscape/tela
  estreita); no overflow, rótulos textuais viram ÍCONE ("Cena"→"≡",
  backend→inicial maiúscula "A"/"V"/"G") — nunca corta, alvo 48dp
  preservado. Fonte ÚNICA do rótulo do backend (`setBackendLabel` com tag
  canônico — o menu não escreve direto no chip).
- **Auditoria bidirecional** (`auditChromeOverlaps`): matriz i<j dos pares
  visíveis {topBar, bottomBar, playHud, gameHudBar, zoom, undo} com caixas
  de tela reais (`getLocationOnScreen`); resultado persistido via
  `nativeStartupMark("CHROME_AUDIT", ok|overlap, pares)` — evidência
  grepável no diagnóstico do device (round 7). Dispara em cada layout da
  top bar (rotação/teclado/play) — os dois sentidos (A×B e B×A) cobertos
  pela matriz de índices.
- Zero re-parent (regra N2 intocada): compação só troca TEXTOS; auditoria
  é somente-leitura.

| Item | Status |
|---|---|
| Chips por conteúdo + single line (fim de "Cen a"/"a u t") | VERIFIED (código; visual no round 7) |
| Compação ícone-em-vês-de-corte | VERIFIED (código; visual no round 7) |
| Auditoria bidirecional com evidência persistida | VERIFIED (código; grep no round 7) |
| Regressões de layout portrait/landscape (P4.6 L1) | VERIFIED (lógica preservada; visual round 7) |

## Bloco 4 — Camera2D: follow/deadzone/smoothing/limits/rotação + verbos

- **CameraData estendida** (aditivos no FIM do struct; defaults =
  comportamento pré-P4.7): `rotationDeg` (vista), `followName` (segue a
  primeira entidade com este Name), `deadzoneW/H` (zona morta do follow,
  unidades de mundo; 0 = off), `smoothingTime` (constante de tempo
  exponencial em SEGUNDOS; 0 = snap), `limitsEnabled` +
  `limitMinX/Y/MaxX/MaxY` (o retângulo VISÍVEL fica dentro do mundo).
- **Migration pre-pass** (`migrateComponentDataP47` no loadScene): o
  decodeStruct é ESTRITO (campo refletido ausente = ParseError) — cenas
  salvas por versões anteriores ganham os 10 campos novos INJETADOS com
  defaults (idempotente; teste com strip real do JSON salvo, padrão P4.6).
- **Pipeline do CameraTick** (ordem fixa documentada):
  DEADZONE → SMOOTHING → CLAMP pós-zoom. Follow por NOME (primeira
  correspondência); deadzone move a câmera só pelo EXCESSO; smoothing
  exponencial (`1 − exp(−dt/τ)`, re-inicia quando a câmera ativa muda);
  clamp pós-zoom usa o tamanho REAL da vista (o documento entrega
  `setViewSize(w,h)` por frame — meia-extensões visíveis = vista/2/zoom;
  limites mais estreitos que a vista = centro). O cache `activeCamera()`
  recebe a posição FINAL — o editor/runtime consomem a câmera processada.
  Primeiro-ativo-vence inalterado (warning de ambiguidade mantido).
- **Rotação da vista**: `Viewport::Camera2D` ganha `rotation` (radianos;
  a câmera do EDITOR nunca rota). Conversão PAR nova (`worldToScreen/screenToWorld`) com contrato testado (a 90°, +X do mundo aparece PARA
  CIMA; round-trip exato). As funções single-eixo ficam no caminho reto
  (rotation == 0 — todas as cenas pré-P4.7). O renderer usa o par +
  `rotOffset` nos pontos/direções de conteúdo (sprites lit/unlit, xadrez,
  partículas, colliders, luz, emissor, marcadores de play/seleção) — a
  composição de rotação do sprite é `quad.rotation + viewRot`.
- **Moldura no editor**: o retângulo de VISTA da câmera (P2) agora gira
  com a rotação e ganha a moldura dos LIMITES (dim) quando
  `limitsEnabled` — o autor vê a área visível e o mundo permitido no
  Edit, com a câmera ainda sem consumir gestos.
- **Verbos NI-Script** (primeira câmera ativa; fault preciso sem câmera):
  `camera.zoom(z)`, `camera.position(x,y)` (offsets — semântica
  Inspector), `camera.follow("Nome")` ("" = solta). O parser ganha
  chamadas com NOME PONTUADO via backtracking barato do cursor (cadeia
  IDENT('.'IDENT)* + '(' — sem chamada, o cursor volta e o caminho de
  campo/atribuição fica intocado; testes de regressão NI verdes).
- **NiHost**: três virtuals com default no-op seguro (hosts sem cena
  devolvem false → fault do verbo, nunca deslize silencioso).

| Item | Status |
|---|---|
| Follow snap + deadzone (dentro não move / fora anda o excesso) | VERIFIED (3 testes tick) |
| Smoothing exponencial monótona | VERIFIED |
| Limits clamp pós-zoom com vista real | VERIFIED |
| Round-trip rotação 90° (+X para cima) | VERIFIED |
| Migration pre-pass de cenas antigas (strip real) | VERIFIED |
| Moldura vista+limites no editor | VERIFIED (buildQuads; visual round 7) |
| Verbos camera.zoom/position/follow no Play | VERIFIED (e2e doc+script) |
| Primeiro ativo vence + regressões P0-5/P2 | VERIFIED (suite tick/editor) |

## Bloco 5 — kinematic_sweep (colisão responde a script ingênuo)

Feedback driver (round 6): script ingênuo (`move`) ATRAVESSAVA paredes —
o `move` era translação crua e a varredura exigia `CharacterBody` (que o
autor ingênuo não anexa). B5: o KINEMATIC com Collider varre por DEFAULT.

### Semântica (contrato NiBindings.hpp + docs/ni-script/07)

- **ON (default)**: `move(dx,dy)` de um KINEMATIC com Collider é
  VARRIDO — a MESMA matemática do moveAndSlide (substeps anti-túnel
  chunk ≤ meio raio, teto 64; raio da esfera do Collider do próprio
  corpo — Sphere = radius, Box = círculo inscrito na meia-extensão
  mínima; o mask do próprio corpo decide contra quem desliza).
  Parede PARA e DESLIZA; movimento alto (6u num passo) nunca túnel.
- **ESCRITA de `position`** (`e.position.x = …`): resolve IGUAL ao
  `move` — wrap no binding refletido (o ÚLTIMO "position" vence na
  tabela; o wrap delega ao refletido e resolve a varredura por cima,
  com `kinematicSweepMoveFrom` varrendo da posição ANTES do write —
  nunca DO destino).
- **`teleport(x,y)`** (novo verbo): SEMPRE cru — válvula de escape do
  autor para spawn/reposicionamento; atravessa por design MESMO com
  sweep ON (host dedicado, não o translate varrido).
- **OFF** (setting por cena): `move` volta à semântica pré-P4.7 (cru).
  Demais casos não-mudados: sem RigidBody / não-kinematic / sem
  Collider ⇒ cru (comportamento antigo 1:1).
- **Persistência**: chave aditiva `"physicsKinematicSweep"` na cena
  (migration honesta — arquivo antigo/ausente = ON: o ingênuo COLIDE
  por padrão). Setter marca cena dirty (como `physicsFixedDt`).
- **UI**: painel Scripts ganha hint monoespaçada ensinando os três
  verbos (move varre / teleport cru / move_and_slide CharacterBody).
- Física de STEP (integração de velocity do kinematic) NÃO muda neste
  bloco — varredura de script é o escopo; integração varrida é futura
  documentada.

### Evidência

| Item | Status |
|---|---|
| move ingênuo (6u) PARA na parede, nunca atravessa | VERIFIED (ContractTests e2e + PhysicsTests) |
| move diagonal DESLIZA (x para, y avança) | VERIFIED (PhysicsTests, parede alta) |
| anti-túnel: 6u num passo fatiado em substeps | VERIFIED (mesma matemática p46 + teste) |
| sweep OFF = cru (semântica pré-P4.7 intacta) | VERIFIED (ContractTests) |
| teleport atravessa MESMO com sweep ON | VERIFIED (ContractTests) |
| ESCRITA de position resolve (wrap delegante) | VERIFIED (P0-7 roda; física From testada) |
| kinematicSweepMoveFrom parte da origem explícita | VERIFIED (PhysicsTests) |
| sem Collider = cru; mask do corpo decide | VERIFIED (PhysicsTests) |
| persistência + ausente = ON (JSON strip real) | VERIFIED (ContractTests) |
| hint dos três verbos no painel Scripts | VERIFIED (código; visual round 7) |

## Bloco 6 — cérebro de performance: culling, spatial hash, governor, LOD

Feedback driver (round 6): 200 entidades travavam o C33 — sem culling
(desenhava tudo), física O(n²), sem governor térmico e sem métricas
visíveis. B6 instala o cérebro; cada peça é testável no Linux.

### Peças

- **Culling de render (Play)**: `Viewport::worldViewRect()` (AABB da
  vista — câmera de jogo quando ativa; rotação expande os cantos) +
  `buildQuads(..., CullRect, &culled)` — quads fora do rect+margin não
  entram no draw; `culled` é EXPORTADO (métrica do round 7: 180/200).
  Pais culled continuam visitando filhos (hierarquia nunca poda).
  Margem conservadora `kCullMarginWorld = 8u` (sprites maiores que a
  escala não somem nas bordas — documentado). SÓ no Play: no Edit o
  autor vê a cena INTEIRA (culling de editor seria ferramenta
  mentirosa).
- **Spatial hash (física)**: broad phase substitui o laço O(n²) — AABB
  por células (célula ≥ 2× a maior extensão), pares candidatos
  DEDUPLICADOS e ordenados na ordem canônica do laço antigo
  (determinismo 1:1 — a ordem dos contatos é contrato). Narrow phase
  intacto. Reconstruído por passo com buckets reutilizados (pooling);
  incremental por dirty-tracking é extensão documentada.
- **PerfGovernor** (puro, testável): EMA do frame time + térmico ADPF →
  escada High/Med/Low (renderScale 1.0/0.85/0.7, lightTextureRes
  512/256/128, post on/off, bloomHalfRes, maxLights 8/4/2) com
  HISTERESE: 30 frames ruins para descer, 120 bons para subir, térmico
  severo desce NA HORA e bloqueia a volta até esfriar. Desce-sobe SEM
  oscilar (prova no CI).
- **Térmico ADPF**: `AThermal_acquireManager` no JNI (Android); sem
  ADPF (Linux/devices antigos) = Unknown → governor só frame time
  (honesto).
- **Logic LOD (opt-in por cena, opt-out por script)**: setting
  `"logicLodEnabled"` (default OFF — semântica pré-P4.7 preservada);
  ON: scripts fora da vista pulam `up update`; opt-out por script via
  `NiScriptComponent::lodOptOut` (additive, migration pre-pass —
  ausente = false). Filtro no `NiRuntime::setLodFilter` (o runtime é
  burro; a política é do documento).
- **Métricas visíveis**: `perfSummaryLine()` (emaMs, fps, drawCalls,
  culled/total, térmico, preset, renderScale) → JNI
  `nativeEditorPerfStats` → linha "Perf:" no HUD do Play (Kotlin).
  Draw calls contados por frame no renderer (`lastFrameDrawCalls`).
- **Batching**: por (shader+textura+layer) já existia (P3 SpriteRun);
  agora MEDIDO (o overlay mostra o efeito).
- **Pooling**: buffers de quads do host reutilizados entre frames
  (`buildQuadsInto` — clear() preserva capacidade; zero realloc no
  frame quente).
- **On-demand rendering (editor)**: EXTENSÃO DECLARADA — a
  invalidação completa (visualRevision em toda mutação visível) tem
  risco de frame stale maior que o ganho no C33; adiado com
  honestidade (o render do editor já é barato — o custo está no Play,
  onde o culling/governor agem).

### Evidência

| Item | Status |
|---|---|
| cull = 180 com 200 entidades (180 off-screen) | VERIFIED (PerfTests, vista+margin e tight) |
| culling conserva filho em vista (pai fora) | VERIFIED (PerfTests) |
| worldViewRect com rotação (AABB de cantos) | VERIFIED (PerfTests 45°) |
| governor desce-sobe SEM oscilar (jitter no limite) | VERIFIED (PerfTests) |
| térmico severo desce na hora e bloqueia volta | VERIFIED (PerfTests) |
| frame inválido não conta; EMA móvel | VERIFIED (PerfTests) |
| spatial hash = mesma ordem de contatos do O(n²) | VERIFIED (suite de física inteira 1:1) |
| logic LOD: off-screen pula / opt-out roda / OFF restaura | VERIFIED (ContractTests e2e) |
| draw calls + cull no HUD do Play | VERIFIED (código; visual round 7) |
| térmico real no device (ADPF) | round 7 (JNI instalado; Linux = Unknown) |
