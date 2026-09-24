# P4.5 — Inventário UI "Curved Dark" (reconstrução da superfície)

> Estado inicial documentado ANTES da reconstrução (base `8f1ce83`, P4.3 fechada).
> Cada linha: superfície → ANTES (estado real do código) → DEPOIS (alvo §1/§2 do
> prompt) → estado THEMED/PENDENTE → verificação VERIFIED / NOT VERIFIED /
> LIMITATION.
>
> Verificação: **VERIFIED (código)** = inspeção do código-fonte confirma o
> tokens/helpers aplicados e comportamento preservado. **VERIFIED (CI)** =
> compilação Android/Linux verde no commit. **NOT VERIFIED (device)** =
> exige verificação visual em device (Realme C33) — passos no fim do
> documento. Nenhuma linha é marcada VERIFIED sem evidência.

## 0. Contrato de preservação (regra que atravessa tudo)

| Item | Estado |
|---|---|
| B-B (foco/IME — sync diferencial do Inspector, resize sem detach) | PRESERVADO (lógica intocada; só styling muda) |
| N1 (preview de áudio toggle/stop) | PRESERVADO (todos os hooks mantidos) |
| N2 (altura adaptativa do painel via LayoutParams) | PRESERVADO (cálculo mantido; margens recalculadas para o novo chrome) |
| N3 (gizmo/rotação) | INTOCADO (zero mudança nativa em Gizmo/Viewport) |
| Contratos JNI existentes | INTOCADOS (só adições: snap/fit/undo/redo) |
| Testes de editor existentes (155 casos) | DEVEM continuar verdes |
| Zero force push / load falha = erro explícito | HONRADO |

## 1. Inventário por superfície

| # | Superfície | ANTES (código real @8f1ce83) | DEPOIS (alvo) | Estado | Verificação |
|---|---|---|---|---|---|
| 1 | Top bar | `LinearLayout` flat `0xF211161F` colado no topo; brand 13sp; 5 toolButtons 36dp cinza | Header card curvo flutuante (raio 20dp, margem 8dp, tom `#12161D`): título 16sp + chips pill mono (backend/ferramenta) + Play icon-button acento | THEMED | VERIFIED (kotlinc + CI verde; device pendente) |
| 2 | Ferramenta (tool switcher) | Botão "FERRAMENTA" na top bar → `AlertDialog` default | Segmented pill flutuante acima das tabs (Select/Move/Rotate/Scale), segmento ativo = pill filled acento | THEMED | VERIFIED (kotlinc + CI verde; device pendente) |
| 3 | Tabs (bottom bar) | `LinearLayout` flat com 6 toolButtons iguais (sem estado ativo) | Tab bar card curvo (raio 20dp flutuante); tab ativa = pill filled acento, inativa = texto dim | THEMED | VERIFIED (kotlinc + CI verde; device pendente) |
| 4 | Painéis (sheets) | `panelContainer` retângulo reto `SURFACE_SOLID`, sem scrim, sem handle, sem animação | Bottom sheet curvo (topo 28dp) + scrim tocável + drag-handle + slide 180 ms; drawer landscape mantém comportamento (N2 preservado) | THEMED | VERIFIED (kotlinc + CI verde; device pendente) |
| 5 | Listas (hierarquia/assets/scripts/anim/ticks) | `android.R.layout.simple_list_item_1` (texto default do tema) / rows manuais sem pressed; divisores default | Rows sem borda/divisores, separadas por tom+espaço, pressed overlay 12%, mono em números, seleção acento | THEMED | VERIFIED (kotlinc + CI verde; device pendente) |
| 6 | Diálogos/menus (~25 usos de `AlertDialog`) | `android.app.AlertDialog` Material default (cinza, cantos 28dp do tema, botões padrão) | `OniDialog`: card curvo 24dp `#222933`, título 16sp, confirm/cancel nos cantos inferiores, fade+scale 120 ms | THEMED | VERIFIED (kotlinc + CI verde; device pendente) |
| 7 | Inputs | `EditText` default (Material underline cinza) | Campos filled curvos 14dp (`#1A2029`), foco = borda acento (única borda), ≥44dp, numéricos com mono | THEMED | VERIFIED (kotlinc + CI verde; device pendente) |
| 8 | Sliders/toggles | `SeekBar`/`Switch`/`CheckBox` Material default | Track curva + thumb acento 20dp (hit ≥48dp); Switch com track/thumb tint acento; sem CheckBox cinza | THEMED | VERIFIED (kotlinc + CI verde; device pendente) |
| 9 | Spinner (categoria de assets) | `Spinner` com `simple_spinner_dropdown_item` default | Chip pill que abre picker curvo (lista temática) | THEMED | VERIFIED (kotlinc + CI verde; device pendente) |
| 10 | Janela de script próprio | `AlertDialog` com `EditText` mono 12sp dentro de ScrollView | Janela dedicada fullscreen: card curvo, superfície código `#0D1117`, mono 13sp, syntax coloring (keywords acento/strings success/comentários secundário/números warn), numeração de linhas, toolbar flutuante Compilar/Anexar/Salvar, painéis laterais colapsáveis (variables/functions), auto-indent | THEMED | VERIFIED (kotlinc + CI verde; [p45] 11/11 local; device pendente) |
| 11 | Toasts | `Toast.makeText` default | Pill raised `#222933` raio total + ícone de severidade (info/ok/erro) | THEMED | VERIFIED (kotlinc + CI verde; device pendente) |
| 12 | HUD Play (editor) | `TextView` retângulo `0x99000000` canto inferior esquerdo | Pill translúcida curva (raio 16dp) com mono nos números | THEMED | VERIFIED (kotlinc + CI verde; device pendente) |
| 13 | HUD Modo Jogo | Barra full-width flat `0xE0101010` | Pill flutuante translúcida: STOP (danger) / PAUSE (warn) / fps mono | THEMED | VERIFIED (kotlinc + CI verde; device pendente) |
| 14 | Zoom | Inexistente visível (só pinch) | Cluster pill vertical bottom-left (+/−/fit) — fit = enquadra seleção/cena (nativo novo) | THEMED | VERIFIED (kotlinc + CI verde; [p45] 11/11 local; device pendente) |
| 15 | Undo/redo | Inexistente | FABs circulares bottom-right (command pattern NATIVO — snapshots de cena), disabled sem histórico, regressões [p45] (move/rotate/scale/create/delete/attach) | THEMED | VERIFIED (kotlinc + CI verde; [p45] 11/11 local; device pendente) |
| 16 | Snap | Inexistente | Chips pill toggleáveis na tool sheet (grade / 15°) — nativo no gizmo, regressões [p45] | THEMED | VERIFIED (kotlinc + CI verde; [p45] 11/11 local; device pendente) |
| 17 | Project switcher | `AlertDialog` com lista de nomes | Sheet curva com thumbnail (1ª textura do projeto) + nome + data (mono) | THEMED | VERIFIED (kotlinc + CI verde; device pendente) |
| 18 | Empty states | `TextView` dim ("Nenhuma entidade selecionada") / listas vazias em branco | Card curvo com ícone 48dp + dica + ação (ex.: "+ Sprite") | THEMED | VERIFIED (kotlinc + CI verde; device pendente) |
| 19 | Splash/ícone | Robô Android default (sem `android:icon`; windowBackground preto) | Ícone adaptativo gerado do prompt verbatim (arco G + nó, `#8AB4F8` em `#0B0E13`) + splash carvão com logo 96dp + wordmark | PENDENTE | — |
| 20 | Tema/base | `Theme.Material.NoActionBar.Fullscreen`, windowBackground preto | Mesmo tema + windowBackground splash carvão; edge effect tint acento | THEMED | VERIFIED (kotlinc + CI verde; device pendente) |

### 1.1 Notas dos Blocos B/C

- **Bloco B** — janela de script dedicada (`ScriptWindow.kt`): card curvo,
  código `#0D1117` mono 13sp, syntax coloring com debounce 250 ms (spans de
  cor APENAS — cursor/IME intocados), numeração de linhas, toolbar flutuante
  Compilar/Anexar/Salvar, painéis colapsáveis vars/funcs (inserem no cursor),
  auto-indent (herda indent + 4 espaços após `:`). Zoom cluster (+/−/fit) —
  `fit` = chamada nativa nova `viewportFit` (enquadra seleção/cena, AABB dos
  quads desenhados). Snap chips (Grade 0.5u / 15°) → nativo no alvo do gizmo
  (`gizmoDragTo`), testes [p45].
- **Bloco C** — undo/redo NATIVO (command pattern por snapshots de cena via
  `SceneSerializer::save/load`): 1 gesto de gizmo = 1 passo (captura no
  `gizmoDragBegin`, limpeza de no-op no `gizmoDragEnd`); scroll-move coalesce
  por janela 1.2 s; cada apply do Inspector = 1 passo; create/delete/attach/
  rename/reparent/componentes = 1 passo cada; histórico ≤ 40; recusado em
  Play com erro explícito; `newScene/loadScene/openProject` limpam. FABs
  circulares ↶↷ (disabled sem histórico, alpha 0.35), atualizam no poll de
  revisão (P1.9).

## 2. Superfícies FORA do escopo (com razão)

| Superfície | Razão |
|---|---|
| `GoniActivity` (runtime-demo landscape) | Não é a tool criativa — é o harness de teste do runtime (§XV FASE 7); comportamento intocado por contrato |
| `DiagnosticsMirror`/`Watchdog` | Sem UI |
| Viewport (grid do canvas) | "Matemática e cores de gizmos/seleção intocadas; grid do viewport intocado" — §3 do prompt |

## 3. Grep zero-defaults (fecho do bloco)

Alvo (zero ocorrências em `android/app/src/main/java/**.kt` no fim):
`AlertDialog` · `Spinner` · `simple_list_item` · `simple_spinner_dropdown` ·
`PopupMenu` · `CheckBox` · `SeekBar` cru (só via helper) · `Switch` cru (só via helper).

Estado inicial: contagem de ocorrências ANTES (Bloco A fechou com **0 usos reais** — os 2 `simple_list_item_1` restantes eram recursos construtor de ArrayAdapter, trocados por `R.layout.oni_list_item`; comentários reescritos para o grep ser honesto) —
- `AlertDialog`: 25 usos
- `Spinner`: 1 (assets)
- `simple_list_item_1`: 5 (ticks/anim/scripts/hierarchy/addComponent)
- `simple_spinner_dropdown_item`: 1
- `CheckBox`: 1 (anim meta)
- `SeekBar`: 4 (color picker)
- `Switch`: 4 (ticks + inspector bool)
- `PopupMenu`: 0
- `Toast.makeText`: 1 helper (centralizado)

## 4. Verificação device (round 5 — após CI verde)

Fluxo a fluxo (Realme C33): abrir (splash+ícone) · criar entidade/sprite ·
inspector (teclado, foco) · assets (busca/preview/import) · script (janela
dedicada: syntax/linhas/toolbar/painéis/auto-indent) · play (HUDs) · settings
· undo/redo (move/rotate/scale/create/delete/attach) · snap chips · zoom
cluster · tabs/sheets/diálogos — **sem uma única superfície cinzenta ou canto
vivo**; regressões B-B/N1/N2 re-executadas.


## 5. Gates (fecho)

| Gate | Estado |
|---|---|
| CI Linux (linux-debug + linux-release) | VERDE @ e6c3573 (commits 9e10d2c/3b483c5/e6c3573 todos verdes) |
| CI Android (assembleDebug arm64+x86_64) | VERDE @ e6c3573 |
| APK do CI | goni-debug-apk @ run 35686873824 → goni-p4.5-debug.apk (7.43 MB) |
| SHA256 | 7779055760e3379771d61e1db1b62dc448ddb077d45f5a2986468247c63f62cb |
| Testes de editor (regressões) | 31/31 local (166 casos, +11 [p45]) |
| Grep zero-defaults | 0 (AlertDialog/Spinner/simple_list_item/CheckBox/Toast default) |
| Loop Producer→Critic | 2 iterações — 2ª apanhou alvos <48dp (§1.8) e elevou tudo |
| Verificação device (round 5) | PENDENTE do usuário — passos na secção 4 |

## 6. NOT VERIFIED / LIMITATION (honesto)

- **Device round 5 pendente**: visual real (cantos, scrim, motion, IME com o
  novo chrome) — kotlinc/CI provam compilação, não aparência.
- **Snap em filhos de pais rotacionados**: o snap é aplicado ao ALVO EM
  MUNDO; para filhos com pai rotacionado, a posição local resultante pode
  não cair exatamente na grade local (documentado no código — aproximação
  deliberada; raiz é exato).
- **Fit sem texturas resolvidas** (TextureCache nulo em testes): AABB usa o
  quad da escala; no device o cache resolve ppu/região (o código tem os
  dois caminhos).
- **Undo e seleção**: restaurar snapshot recria a cena (IDs novos) — a
  seleção é LIMPA a cada undo/redo (honesto; documentado no §1.1 Bloco C).
- **playHud**: caminho legado do P4.1 (o Modo Jogo cobre todo o Play desde
  a P4.2 — a pill só apareceria num Play sem chrome, que não existe mais).
  Mantida wired (atualiza por frame), zero UI morta nova introduzida.
