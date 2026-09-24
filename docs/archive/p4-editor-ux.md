# P4.1 — Editor no device: bugs críticos + gizmos utilizáveis

Base: `203e5c9` (P3.5). Fase de CORREÇÃO + USABILIDADE — nenhuma arquitetura nova.
Primeira sessão real de UX no Realme C33 (Unisoc T612) produziu a lista D1–D8;
cada defeito virou código, teste e estado abaixo. Gate: **31/31 Linux debug+release**.

## Tabela de defeitos

| # | Defeito (repro device) | Causa raiz | Correção | Estado |
|---|---|---|---|---|
| D1 | 1ª seleção move; ao selecionar outra coisa as setas não aparecem e o drag não responde | **`selectEntity()` (hierarquia/menus) só atualizava a var Kotlin** — nunca chamava `nativeEditorSelect`; a seleção C++ (fonte do gizmo/hit-test/render) ficava na entidade velha. Além disso nenhum evento (tool/play/stop/tap) matava um drag em voo | Fonte única de verdade: `selectEntity` agora chama `nativeEditorSelect` (erro honesto via toast). Re-armo determinístico no documento: `select()`/`deselect()`/`setTool()`/`play()`/`stop()`/`viewportTap()` chamam `gizmoDragEnd()` — nenhum estado de drag sobrevive a mudança de contexto | **FIXED** |
| D2 | Gizmo de mover "calcula mal" a entrada na cena | **Bug de unidade do P1**: `screenDistanceTo` devolve px de TELA mas o raio de acerto era convertido px→mundo (`pxToWorld(kHitPx)`) — tolerância escalava com o zoom (~4px em zoom mínimo; ~256px em zoom máximo). Os testes antigos só passavam porque tocavam no centro EXATO dos handles | Hit-test compara px de tela contra px de tela (`hitPx(uiScale)`) — alvo constante no espaço do ecrã em qualquer zoom; novo teste de regressão com zoom 8 e 48 | **FIXED** |
| D3 | Gizmo de rotação minúsculo, sem seta, impossível de usar | Raio do anel derivado dos bounds sem clamp mínimo em px; sem affordance | Raio do anel = `max(borda dos bounds + 26dp, 64dp)` — agarrável em qualquer zoom; handle dot acoplado ao ângulo + spoke radial + chevron (ponta de seta); alvo de toque 48dp de diâmetro | **FIXED** |
| D4 | Sem gizmo utilizável de escala | Handles de canto existiam a 13px, sem alvo de toque ≥48dp, sem affordance | Métricas em dp × densidade do device (`Viewport::setUiScale`): handle visual 28–40px, alvo de toque 48dp; MOVE = 4 setas (±X/±Y) + quadrado central; SCALE = 4 cantos + **4 handles de aresta** (escala de UM eixo — `ScaleEdgeE/W/N/S`) | **FIXED** |
| D5 | Script criado+anexado, Play → nada acontece; sem feedback de erro | **Ni-Script não tinha operadores compostos** (`+=`, `-=`, `*=`, `/=`) — o script do autor (`position.x -= dt`) NÃO compilava; o erro ia só para o logcat (ENG_ERROR), invisível na UI | (a) Lexer/Parser: `+= -= *= /=` com desugar `x op= v → x = x op v` (reusa o caminho NEST_SET/DYN_SET do emitAssign); (b) `NiScriptStats` no runtime (found/compiled/failed/instances/ticks/faults/primeiro erro/entidade falha); (c) marcos `SCRIPT_COMPILE`/`SCRIPT_FAULT` no diagnóstico persistido; (d) UI: toast com o erro + HUD do Play ("Scripts: N inst · T ticks · F faults") ao vivo | **FIXED** |
| D6 | Áudio adicionado, Play → silêncio (AAudio builder null no C33 → NullBackend), sem aviso | Sem backend alternativo; estado do backend não chegava à UI | (a) **OpenSlEsBackend** (dlopen libOpenSLES.so — caminho Legacy AudioTrack; buffer queue i16 estéreo; AudioAdapt + CallbackGate, disciplina P3.4); (b) **AutoAudioBackend**: cadeia AAudio → OpenSL ES → erro composto, seleção logada (`AUDIO_BACKEND_SELECTED`); (c) `EditorHost::audioStatusLine()` + JNI + **HUD do Play** ("Áudio: running: opensl …" / "null: device sem AAudio/OpenSL …") + toast na entrada do Play | **FIXED** (som real se o HAL abrir; aviso honesto quando não abre — sem fallback silencioso) |
| D7 | Assets com opções mortas | Menu de áudio mostrava 4 itens com handler de 3 índices: "Ouvir (preview)" disparava RENOMEAR; "Apagar" era no-op (o `nativeEditorAudioPreview` existia e não era chamado) | `assetMenuDialog` reescrito com handlers EXPLÍCITOS por item (lista de lambdas, sem índice compartilhado): áudio = Ouvir/Renomear/Mover/Apagar; scripts = Abrir no editor/Renomear/Mover/Apagar; texturas = **Aplicar no sprite selecionado**/…; apagar com confirmação; erro honesto em todas as falhas | **FIXED** |
| D8 | Botão de configuração do projeto abre "criar novo" | O "☰" abria o menu com "Novo projeto…" primeiro; "Configurações" era só um renomear disfarçado | "Configurações do projeto…" é o PRIMEIRO item e abre sheet REAL: nome editável (salva via `setProjectName`), camadas da cena, timestep da física (1/60 fixo, acumulador) e **estado vivo do backend de áudio** | **FIXED** |

## Regras invioláveis — cumprimento

- **Zero UI morta**: D7/D8 rewired; nenhuma opção sem handler; erros sempre mostrados.
- **Zero gizmo fake**: todas as affordances são hit-testáveis (4 setas, arestas, anel).
- **Nenhum fallback silencioso**: cadeia de áudio loga a seleção; HUD mostra o estado; scripts expõem erro/entidade.
- **Device-verified**: o que NÃO foi testado em device está marcado abaixo.
- **Histórico preservado**: nenhum sistema existente removido; métricas do P1 ampliadas (dp), não trocadas.

## Áudio — comportamento em device

1. `createDefaultBackend()` (Android) devolve a cadeia Auto.
2. AAudio tenta abrir (mesmos marcos granulares do P3.4/P3.5).
3. Recusado (o caso do Unisoc: builder null) → OpenSL ES tenta (marcos `AUDIO_OSLE_*`).
4. Vencedor anunciado em `AUDIO_BACKEND_SELECTED`; HUD do editor mostra
   `running: <backend> <params>`; recusas compostas → NullBackend gracioso do host
   (P3.5) + HUD `null: device sem AAudio/OpenSL — som indisponível (nova tentativa no
   próximo resume)`.

Mixer/vozes intocados — a troca é só do backend (regra da missão).

## NI-Script — linguagem

- `x += v`, `x -= v`, `x *= v`, `x /= v` (globais, locais e campos de binding —
  `me.position.x -= delta()` incluído).
- Desugar no parser: `x op= v` → `x = x op v` (nó do alvo reusado como operando
  esquerdo; arena do parser — sem dupla posse). Sema/Compiler 1:1 o caminho de
  atribuição existente.

## Testes novos (regressão editor `[p41]`)

- D1: A→drag→B→drag funciona; troca de ferramenta/play/stop matam drag vivo.
- D2: raio de acerto constante em px (zoom 8 e 48).
- D3: anel ≥64px em zoom mínimo (com pan).
- D4: arestas E/N escalam um eixo; cantos priorizados quando tocados.
- D3/D4: `uiScale=2` amplia alvos (48dp); handle visual 28–40px.
- D5: `+= -= *= /=` em globais e campos (engine: `[ni][p41]`); CRLF tolerado.
- D5: stats visíveis (erro de compilação + entidade; ticks; faults; first update).

## Gates

- Linux debug: **31/31** (`ctest --preset linux-debug`) — inclui 127 casos do editor.
- Linux release: **31/31**.
- CI Android (APK): segue no workflow (`assembleDebug` arm64-v8a) — o binário device
  desta fase foi validado por compile+testes; o teste em DEVICE REAL (Realme C33)
  permanece o passo do utilizador conforme protocolo das fases anteriores.
- APK+SHA256: gerados pelo pipeline de release da mesma forma que o P3.5.

## Estado honesto / limitações

- Os testes de crash forense (P3.1/P3.5, fork+ASan) mostraram-se intermitentes em
  sandbox local SEM GPU/CI (presentes também na base `203e5c9` sem as mudanças
  desta fase) — no CI oficial são estáveis. Nada desta fase os toca.
- D6: o OpenSL ES abre o caminho Legacy do framework, mas o HAL do dispositivo é
  a última palavra; se AMBOS recusarem, o estado é mostrado (nunca calado).
- As camadas no sheet de configurações listam hoje o estado real do engine
  (GAME padrão — render+física); edição de camadas é trabalho do P4.2.

---

# Adenda P4.1.1 — CI vermelho em `52231a8`: causas raiz (CI Linux #51 + CI Android #45)

A rede de segurança fez o trabalho dela: 31/31 locais, mas os compiladores/ambientes
do CI apanharam o que o sandbox não apanhava. Diagnóstico por log completo dos runs,
classificação por item, e correção SEM skip/delete/enfraquecimento de teste.

## CI Linux #51 (debug E release) — UMA falha real, zero flake

**Falha real (determinística, ambos os presets):** o teste P1 de readback
("renderer desenha GIZMO por cima do sprite") exige `gizmoVerts.size() == 30` —
a geometria do MOVE **antiga** (3 handles × 6 vértices + 2 eixos × 6). O P4.1
(D3/D4) REDESENHOU a affordance do MOVE (4 setas ±X/±Y + quadrado central): o
layout novo produz 5 quads × 6 + 4 hastes × 6 = **54** (o próprio CI observou
`54 == 30`). O teste acompanhou a especificação nova: contagem re-pinada em 54,
provas de pixel (handle central amarelo sobre o sprite) e de drag INTACTAS.
Não é enfraquecimento — o contrato de topologia do gizmo NOVO fica fixado.

**Os "crashes" P3.1/P3.5 no log NÃO são falhas do processo pai (prova):** o
binário tem 127 casos; o log mostra TRÊS summaries — `121|119|2` (FILHO do teste
P3.1: tally herdado de 120 + o próprio, morto por sinal por design),
`127|125|2` (FILHO do teste P3.5) e `127|126|1` (PAI — a única falha é o gizmo).
O banner `FAILED ... due to a fatal error condition: SIGSEGV` é o Catch2 do
FILHO apanhando o sinal re-entregue e imprimindo no mesmo stream — exatamente o
que o comentário do teste documenta ("o handler encadeia p/ o handler ANTERIOR,
que pode terminar em SIGABRT (Catch2 chama abort) ou exit(1) (ASan Die())").
O pai PASSOU nos dois testes forenses nos dois jobs. Zero mudanças neles.

## CI Android #45 — 6 causas raiz (clang do NDK r27, `-Werror`; o ninja morreu
em `eng_audio` e NUNCA chegou a `EditorJni.cpp`)

1. `OpenSlEsBackend.cpp`: chamava `formatResult(SLresult)` que nunca existiu
   neste TU (o do AAudioBackend tem outra assinatura). Adicionado helper local
   com a tabela de códigos do especificação Khronos (nunca `nullptr`).
2. `refuse()`/`cleanupAndRefuse()` devolviam `eng::core::Error` puro em funções
   com contrato `Result<void>` — `Result` NÃO converte `Error` implicitamente.
   Corrigido para `eng::core::makeUnexpected(...)` (8 pontos de retorno).
3. `SL_IID_*`: são VARIÁVEIS globais `const SLInterfaceID` (= `const
   SLInterfaceID_ *const` — PONTEIRO). O `dlsym` devolve o endereço da
   variável; o valor vem da desreferência. O código antigo fazia
   `static_cast<SLInterfaceID>(dlsym(...))` e desreferenciava nos chamados —
   conversão inválida. Corrigido: null-check ANTES da desreferência na carga,
   `api.iidX` passado direto nos `GetInterface`/`ids[]`.
4. `SLEngineItf` é DUPLO ponteiro (`const SLEngineItf_ *const *`):
   `engine->CreateOutputMix(...)`/`CreateAudioPlayer` → `(*engine)->Fn(engine, ...)`
   (mesmo idioma dos objetos, já usado no próprio TU).
5. `AutoBackend.cpp:53`: `createAAudioBackend()` chamado sem DECLARAÇÃO em
   `Audio.hpp` (a definição existia em `AAudioBackend.cpp` sob `__ANDROID__`).
   Declarada ao lado de `createOpenSlEsBackend()`.
6. `EditorActivity.kt:369`: `0x99000000` > `Int.MAX_VALUE` — literal não conforma
   a `Int` em Kotlin (em Java seria wrap silencioso). Corrigido para
   `0x99000000.toInt()` (padrão já usado em todo o resto do ficheiro).

`EditorJni.cpp` — o suspeito histórico — **compila limpo**: build local NDK r27
arm64-v8a + x86_64, Release, `-Werror`: 230/230 alvos, `libgoni.so` linkada,
APK `assembleDebug` completo montado localmente.

## Verificação local do fix (P4.1.1)

- linux-debug (ASan+UBSan, `-Werror`): build OK, **31/31** (`rhi_hardware` SKIPA
  no sandbox sem drivers — comportamento por design; no CI rodam).
- linux-release (LTO): build OK, **31/31**.
- Android arm64-v8a + x86_64 (NDK r27, Release, `-Werror`): 230/230 alvos.
- `./gradlew assembleDebug`: BUILD SUCCESSFUL (APK 7,2 MB — valida C++ E Kotlin).

---

# P4.2 — Device bugs round 2 + Modo Jogo (G1)

Base `0b029bd` (P4.1.1, CI verde). Sessão de teste no Realme C33 com
`goni-p4.1.1-debug.apk` produziu 5 bugs device-verified (B-A…B-E). Esta
fase corrige persistência/UX/gizmos/áudio e entrega o Modo Jogo.

## Estado dos defeitos

| Defeito | Estado | Causa raiz + correção |
|---------|--------|------------------------|
| **B-A** save/export → reload perde a cena | **FIXED** | Três falhas compostas: (1) `openProject`/`ensureStartupProject` nunca restauravam a cena (`newScene()` era o "default" — e a Activity ainda chamava `nativeEditorNewScene` DEPOIS de abrir/importar); (2) "Salvar projeto" escrevia só `project.goni.json` (a cena só existia com "Salvar cena…" manual); (3) zip export (Kotlin) não embrulhava as entradas numa pasta → import derivava o nome do projeto da ÚLTIMA entrada ("assets"/"scenes") → lixo no workspace + open falhava. Correção: marker `.goni_last_scene` na raiz do projeto + restore no `openProject` (falha de load = ERRO explícito — nunca vazio silencioso); `saveProject` = salvamento COMPLETO (projeto + cena, default `main.json`); zip do projeto movido para C++ (`ProjectZip`, store-only, wrapper = nome da PASTA real, anti-traversal, CRC32) — testável no Linux; Activity sem `newScene` pós-open/import. |
| **B-B** teclado abre e fecha no Inspector | **FIXED** | `updatePanelPlacement()` re-parentava o `panelContainer` (remove+addView) a CADA dispatch de insets — abrir o teclado disparava insets → view destacada → foco perdido → IME fechava → loop. Agora só re-parenta quando o MODO (portrait/landscape) muda. `refreshInspector()` virou sync DIFERENCIAL: assinatura da estrutura; mesma estrutura → valores in-place com diff antes de `setText` e views com foco NUNCA tocadas (o padrão já usado por `updateTransformFieldsLive`). |
| **B-C** gizmo de rotação inoperante por toque | **FIXED** | (1) O hit-test aceitava só o DOT do handle — tocar no anel a 90° do dot devolvia `None` e o gesto virava PAN. Agora o ANEL INTEIRO é alvo (banda \|dist−raio\| ≤ raio de acerto). (2) O ângulo TOTAL era normalizado contra o grab fixo: dedo além de 180° flipava o sinal e a entidade girava PARA TRÁS. Agora cada evento contribui com o DELTA curto contra o ângulo anterior e ACUMULA (voltas completas somam). |
| **B-D** gizmo de move errado nos DOIS eixos | **FIXED** | Não era a matemática de conversão (px→mundo estava correta e única) — eram as PERIFERIAS do gesto: (1) `event.x/y` são sempre do pointer 0: um 2º dedo rouba o pointer e a entidade teleporta; agora o drag é dono de um pointer ID (`findPointerIndex`), `POINTER_UP` do dono encerra honesto, e os detectores (pinch!) não veem eventos durante o drag — zoom a meio do drag re-projetava o grab capturado no begin (salto nos 2 eixos); (2) tocar na HASTE (fora da pontinha) caía no fallback do `onScroll` (mover relativo com slop) — hastes agora são alvo; (3) precedência: com alvos em dp, o raio da haste cobria o centro — centro agora é avaliado PRIMEIRO; (4) raio de seleção do tap era 14 px FIXOS (~7dp no device) — escala com a densidade. Regressões com os parâmetros do device: 720×1600, density 2.0, 2 zooms, drags X e Y separados, e o teste "o dedo segue" (tap no ponto final re-encontra a entidade). |
| **B-E** `ParseError: wav: não é RIFF/WAVE` no preview | **FIXED** | O import validava texturas apenas (e só no JNI); áudio aceitava qualquer bytes e o erro estourava DEPOIS no preview. A validação de conteúdo vive agora em `EditorDocument::importAsset` (testável no Linux; JNI delega): textura → probe de decode; áudio → probe RIFF/WAVE PCM com recusa NO IMPORT e mensagem clara ("apenas WAV PCM suportado por agora — OGG/MP3 é fase futura"). Picker de áudio filtra MIME de WAV. **Roadmap: OGG/MP3 = fase futura (declarado, não implementado).** |
| **T5** Modo Jogo (G1) | **IMPLEMENTED** | Play → fullscreen (topBar/bottomBar/painéis fora) com HUD próprio: STOP, PAUSE/CONTINUE e linha de estado (backend de áudio, fps barato por janela de 30 frames, estado de PAUSE). PAUSE congela o TICK (nenhum sistema avança; render e câmera vivos). Input 100% roteado ao jogo durante o Play (`gameWantsTouch` não exige mais tool 0 — rota morta eliminada). STOP volta ao editor com SELEÇÃO e CÂMERA intactas: contrato "stop reseta seleção" (a7fd366) REVISTO pelo prompt — o handle da edição é capturado no play() e restaurado no stop(); a câmera do editor nunca saiu do lugar (regressão pinada). |

## Verificação local (P4.2)

- linux-debug (ASan+UBSan, `-Werror`): build OK, **133 passed + 12 skipped**
  (`rhi_hardware` SKIPA no sandbox sem drivers; no CI rodam), 0 failed —
  18 casos novos `[p42]` (B-A round-trips, zip traversal/clash, gizmos em
  parâmetros de device, import de áudio/textura, pausa/seleção/câmera do
  Modo Jogo, contratos do ProjectZip).
- linux-release (LTO): build OK, **133 passed + 12 skipped**, 0 failed.
- Android arm64-v8a + x86_64 (NDK r27, Release, `-Werror`): build OK.
- `./gradlew assembleDebug`: BUILD SUCCESSFUL (APK 9,2 MB — valida C++ E Kotlin).

## Contratos revistos (declarados)

1. **"stop() RESETA a seleção" (a7fd366) → "stop() PRESERVA a seleção da
   edição"** — exigido pelo Modo Jogo ("Stop volta ao editor com seleção e
   câmera intactas"). Testes atualizados (não enfraquecidos: a nova
   asserção é MAIS forte — exige o handle certo e entidade editável).
2. **"Salvar projeto" agora persiste também a cena** (antes: só o
   `project.goni.json` — a causa principal do B-A). Cena nunca salva →
   default `main.json`, sem diálogo extra.
3. **Em Play TODOS os toques vão ao jogo** (antes: só com tool Select —
   os outros tool eram rota morta: gizmo inexistente em Play e pan/zoom
   no-op sob câmera de jogo).
4. **`saveProject` é rejeitado em Play** (antes: escrevia o projeto com a
   cena em estado de runtime — agora coerente com "cena somente-leitura
   em Play").

## Verificação no device (Realme C33) — passos

1. **B-A**: criar entidades → ☰ Salvar projeto → matar o app → reabrir:
   as entidades ESTÃO lá (logcat `project-op: última cena restaurada
   'main.json' (N entidades)`). Exportar zip → apagar projeto → importar
   zip: entidades ESTÃO lá.
2. **B-B**: abrir Inspector → tocar num campo de texto → digitar: o
   teclado PERMANECE. Girar o device com o teclado aberto continua
   reposicionando o painel (única condição de re-parent).
3. **B-C**: tool ROTACIONAR → tocar no anel em QUALQUER ponto e arrastar
   volta completa: rotação segue o dedo, sem inverter no meio.
4. **B-D**: tool MOVER → agarrar o CORPO da entidade e arrastar: a
   entidade acompanha o dedo nos dois eixos (tap final re-seleciona).
   Arrastar pela haste X: só X; haste Y: só Y. Segundo dedo durante o
   drag não teleporta a entidade.
5. **B-E**: Assets → categoria audio → importar um `.m4a`/`.ogg`: recusado
   NO IMPORT com toast claro. Importar `.wav`: entra e o preview toca.
6. **T5**: Play → editor some, HUD com STOP/PAUSE + fps aparece; PAUSE
   congela; STOP volta com a seleção e a câmera de onde estavam.

---

# Adenda P4.3 — EDITOR COMPLETO (fusão 4.3+4.4+4.5) + bugs round 3

Base `fcdaa40` (P4.2). Round 3 no Realme C33 confirmou as vitórias do
P4.2 (teclado/inspeção = B-B núcleo morto, WAV preview toca, escala boa)
e nomeou 4 restos (N1–N4). Decisão de produto: hierarquia (ex-P4.3),
inspector+ticks (ex-P4.4) e materiais+luzes (ex-P4.5) fundidos numa fase
em BLOCOS ORDENADOS com gate próprio (bloco seguinte só com o anterior
verde no CI). **Authoring de animação (P4.6) e authoring de GLSL custom
ficam FORA — futuro declarado, nada implementado.**

## Estado dos defeitos do round 3

| Defeito | Estado | Causa raiz + correção |
|---------|--------|------------------------|
| **N1** preview de áudio sem stop (toca até reiniciar a app) | **FIXED** | `audioPreview` descartava o `VoiceHandle` do mixer — nenhuma voz podia parar. Agora: **toggle** (2º toque no MESMO asset = stop; outro asset troca com stop da anterior — uma única voice de preview existe), `audioPreviewStop()` idempotente por handle, `audioPreviewPlaying()` como fonte de verdade do botão (a voice pode terminar sozinha), bus dedicado "preview" isolado do master (vozes de jogo intocáveis), e stop automático ao **fechar/trocar painel, mudar categoria de assets, importar e entrar em Play**. |
| **N2** com IME aberta, tab bar sobrepõe o painel e campos ficam esbatidos | **FIXED** | A altura do painel era FIXA (62% do display cheio): o IME (adjustResize) encolhia a root, mas o painel continuava grande — bottomBar desenhava POR CIMA do conteúdo. Correção: altura recalculada da root ATUAL (min(62% visível, root−barras)) + painel ancorado ACIMA da bottomBar (bottomMargin) + campo focado rola à vista — **tudo via LayoutParams (requestLayout), NUNCA re-parent/detach**: o fix B-B (foco/IME intocáveis) não pode voltar. Regressão Linux limitada (sem IME no sandbox) — passos device abaixo. |
| **N3** rotação deforma sprites + anel elíptico no portrait | **FIXED** | A deformação NÃO era scale corrompido no ECS (o drag de rotação nunca escreve scale — regressões 90/180/360° com ε + filho de pai escalado provam). Era o RENDER: quads rotacionados dentro do CLIP SPACE anisotrópico (X escala w/2 px, Y escala h/2 px) — sprite rodado 90° esticava 2,22× (720×1600) e a espessura dos segmentos do anel variava com a direção (elipse perceptual). Ver N4. |
| **N4** refino de viewport — overlays sem conversão única | **FIXED** | **Regra única nova (`OverlayMath.hpp`)**: toda a geometria do editor (grid, bounds/bordas, sprites lit/unlit, xadrez, partículas, contornos de collider, retângulo de câmera, emissor, gizmos, preview de luz) nasce em PX DE TELA — rotação aplicada com a projeção mundo→tela (flip Y) — e só vira clip no último passo, por eixo. Círculos são círculos, quadrados são quadrados, espessuras constantes em px em qualquer aspect. |

## Estado por bloco

| Bloco | Estado | Conteúdo |
|-------|--------|----------|
| **Bloco 0 — N1–N4** | **FIXED** | Tabela acima. Nota honesta: o CI (com lavapipe) apanhou um passo do refactor que o sandbox local (sem driver) não podia — a primeira versão rotacionava cantos SEM o flip Y da projeção (sprite virado no readback PNG). Corrigido com regressão aritmética sem GPU (clip y 0.375) + o CI verde a confirmar. |
| **Bloco 1 — hierarquia completa (ex-P4.3)** | **JÁ OPERATIVO + ordem estável** | Criar/renomear/duplicar(apagar)/parentear por toque + menu de contexto por entidade já existiam (P4.1/P4.2). O gap era a ORDEM: o save canônico ordenava por SceneEntityId (UUID) e o load recriava nessa ordem — **raízes e irmãos embaralhavam** após save/load. Fix na raiz: ordem canônica do save = **DFS pela hierarquia** (raízes pela ordem que o autor vê; filhos na ordem interna do Scene) — determinismo ADR-033 preservado (mesmo estado → mesmos bytes). Round-trips de topologia e de duplicação de subárvore pinados. |
| **Bloco 2 — inspector completo + Ticks visíveis (ex-P4.4)** | **IMPLEMENTED** | Add/remove de TODOS os componentes registados já existia (catálogo único ADR-043 — 11 tipos; hints de dependência). O novo: sheet **"Ticks"** (6ª tab) — camadas GAME/SUBGAME/nomeadas com **timeScale por camada (0 = pausada)** e **participação update/física/render** (toca a `LayerRegistry` REAL — `timeScaleOf`/`participatesIn` do runtime mudam), **"+ Camada"** por toque, e **timestep fixo da física (s)** — configura o `TimestepAccumulator` do PhysicsTick e **persiste na cena** (chave aditiva `physicsFixedDt`; arquivo antigo = 1/60). Zero campos mortos: tudo guarda/configura estado vivo. |
| **Bloco 3 — materiais & luzes (ex-P4.5)** | **JÁ OPERATIVO + preview de luz** | Materiais `.mat`: criar/editar (lit/unlit + tint/alfa, swatch com preview) e atribuir por picker — operativos desde o P3 com readback. **Novo**: **preview visual da Light2D** no viewport (anel de alcance = raio×zoom px, círculo perfeito + dot central; luz desligada não desenha) e atalho **"＋ Luz 2D"** no menu de contexto (1 toque cria + seleciona; cor/raio/falloff/camada no Inspector). **Sem authoring de GLSL custom (futuro declarado).** |

## Verificação local (P4.3)

- linux-debug (ASan+UBSan, `-Werror`): build OK, **100% (31/31)**, 0 failed —
  9 casos novos `[p43]` (OverlayMath isotrópico, anel círculo px no
  portrait, rotação 90/180/360 sem scale, pai escalado, preview de áudio
  toggle/stop/isolamento, topologia round-trip, subárvore duplicada,
  camadas/timestep, dados do marker de luz).
- linux-release (LTO): build OK, **100% (31/31)**, 0 failed.
- CI Linux (debug+release) e CI Android (APK arm64-v8a+x86_64): verdes por
  bloco (gate: bloco seguinte só com o anterior verde).

## Contratos revistos (declarados)

1. **Ordem canônica do save: SceneEntityId → DFS de hierarquia.** A ordem
   antiga embaralhava a cena visível a cada save/load. Byte-estável
   mantido; resave de clone idêntico; órfãos impossíveis entram no fim
   (zero perda).
2. **`audioPreview` é TOGGLE** (antes: sempre tocava uma voice nova).
   Vozes de jogo nunca são alvo do stop do preview (bus dedicado + stop
   por handle — nunca `stopAll`).
3. **Cena ganha chave aditiva `physicsFixedDt`** (documento escreve/lê;
   o loader do serializer ignora chaves desconhecidas — arquivos antigos
   carregam com 1/60).
4. **Espessuras de overlay agora são px honestos** (grid 1.4, collider/
   câmera 1.2, emissor 1.0, gizmo 2.0 px) — antes variavam com a direção
   e com w/h da surface.

## Verificação no device (Realme C33) — round 4

1. **N1**: Inspector → AudioSource → "Ouvir": toca; 2º toque: PARA.
   Assets → áudio → "Ouvir/Parar": idem. Fechar o painel ou trocar de
   categoria com o som tocando: para sozinho. Play com preview ativo:
   preview morre, vozes de jogo seguem.
2. **N2**: Inspector → tocar num campo (teclado abre): a tab bar NÃO
   sobrepõe o painel; o campo focado fica visível (sobe à vista). Digitar
   contínuo não fecha o teclado (B-B continua morto).
3. **N3**: tool ROTACIONAR → rodar um sprite 90/180/360°: a ARTE não
   deforma (proporção px preservada) e o anel é um CÍRCULO com linha
   uniforme no portrait. Inspector confirma escala inalterada.
4. **N4**: gizmos (move/rotate/scale) com handles quadrados; contornos de
   collider uniformes; borda de seleção acompanha sprites rotacionados.
5. **Bloco 1**: criar 2 raízes com filhos → salvar → matar app → reabrir:
   a ORDEM da hierarquia é a mesma.
6. **Bloco 2**: tab Ticks → camada UI (nova) com timeScale 0.25 → entidade
   nela → Play: anda a 25%. Desligar render da camada: some do viewport.
   Dt física 1/120 → salvar/reabrir: permanece.
7. **Bloco 3**: menu da entidade → "＋ Luz 2D": anel âmbar aparece no
   viewport; ajustar raio/cor no Inspector: anel e iluminação mudam
   juntos. Material unlit/lit + tint por picker.

## Limitações declaradas (não implementado — futuro)

- **N2 no Linux**: regressão limitada (sandbox sem IME) — validação por
  passos device (item 2 acima); a mecânica (LayoutParams sem detach) é
  coberta por inspeção do fluxo e o gate B-B tem regressão própria.
- **OGG/MP3** no áudio: recusado no import com mensagem clara (fase futura).
- **Authoring de GLSL custom**: fora do escopo (futuro declarado).
- **Authoring de animação (P4.6)**: fora desta fase (raio de explosão).
