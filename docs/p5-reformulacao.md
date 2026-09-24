# P5 — Reformulação de interface e de fluxo

Base: `1e8e71c` (P4.7.0). Plano de corte e reestruturação. Nenhum código
foi alterado neste passo. Aqui estão o diagnóstico, os cortes, o fluxo novo
e a ordem de execução.

---

## 1. Diagnóstico (medido no código, não em opinião)

| Fato | Número | Consequência |
|---|---|---|
| Linhas no repo (C++/Kotlin/docs) | ~105 mil | Muita engine para um produto que ainda não entrega um jogo |
| Coluna "Device tested" da matriz de funcionalidade | **100% PENDENTE** | Nada foi validado no aparelho real; todo polimento foi às cegas |
| `eng::build` (export) linkado no editor | **não** | O usuário **não consegue exportar um jogo**. O ciclo *criar → testar → publicar* não fecha |
| `GoniActivity` | demo de triângulo | Não existe *player*: um jogo feito no editor não roda fora do editor |
| `EditorActivity.kt` | 2154 linhas, ~60 campos mutáveis | Estado espalhado pela UI; sincronização manual (inspector "diferencial", guardas de foco/IME) |
| Funções JNI do editor | **130** | Contrato por strings TSV, um par Kotlin↔C++ para cada botão |
| `EditorDocument.cpp` | 4184 linhas | Projeto, cena, seleção, histórico, play, scripts, animação e materiais numa classe só |
| Marcações de fase nos comentários (`P4.x`, `FASE`, `ADR`) | **1454** | O código conta o histórico em vez da intenção. O histórico já está no git |
| Infra de diagnóstico (Diagnostics.cpp + Mirror + Watchdog + CrashGuard + marks) | ~1900 linhas, 55 chamadas só no Kotlin | Resolveu os crashes do C33; hoje é peso morto no caminho de boot |
| Docs de auditoria/fase | 34 arquivos, 6,4 mil linhas | Ninguém lê. Duplicam o README e se contradizem com o tempo |
| Chrome da tela do editor | top bar + 4 segmentos de ferramenta + snap + zoom + undo/redo + 6 abas | Numa tela de 6,5" sobra pouco espaço para a cena. Daí a "auditoria de sobreposição" |
| Seletor de backend (auto/vulkan/gles) na UI | exposto ao usuário | Decisão técnica jogada em quem só quer fazer um jogo |
| `EditorTests.cpp` | 8780 linhas num arquivo | Difícil de achar e de manter |

**Resumo:** a base técnica (ECS, cena, serialização, física, NI-Script, RHI)
é boa e testada. O problema é o **produto**. A energia foi para gizmo com
halo de 1dp, governor térmico ADPF, logic LOD e auditoria de sobreposição de
chrome, enquanto o fluxo principal (fazer e exportar um jogo) nunca fechou
e nunca foi validado no aparelho.

---

## 2. Princípios da P5

1. **Um fluxo fechado vale mais que dez recursos soltos.** Nada entra antes
   de *criar → editar → jogar → exportar → rodar o APK exportado*.
2. **Pronto = funciona no Realme C33.** O CI verde é pré-requisito, não o fim.
3. **Uma fonte de verdade.** O estado vive no C++. O Kotlin só *desenha* um
   snapshot e *envia* comandos. Não há estado espelhado nem polling de campos.
4. **Toda mutação passa por um único caminho** (comando). Undo, dirty,
   revisão e validação de contrato vêm de graça.
5. **Comentário explica o porquê, não o histórico.** Fase e bloco ficam no
   commit.
6. **Menos chrome, mais cena.** Uma barra inferior contextual; o resto sai
   da frente.

---

## 3. Faca: o que sai, o que congela, o que fica

### 3.1 Remover

| Item | Ação |
|---|---|
| Seletor de backend na UI | Sai da tela. Vai para Configurações → Avançado (só em build debug). Em produção: `auto` |
| `auditChromeOverlaps`, `markUiTheme`, `markUiSplash`, micro-marks de boot no `onCreate` | Remover. O novo layout não sobrepõe por construção |
| `nativeStartupMark` espalhado (55 chamadas) | Reduzir a **um** crash handler nativo + **um** handler Kotlin gravando `crash.log`, e um botão "Exportar diagnóstico" em Configurações |
| Watchdog de main thread (heartbeat a cada frame) | Remover do caminho padrão; reativar só em build debug se voltar a haver hang |
| Marcações `P4.x / FASE / bloco / round` nos comentários | Varredura: apagar as narrativas e manter só a regra |
| `GoniActivity` + `GoniJni.cpp` (triângulo) | Substituir pelo **Player** (seção 5.3) |
| `phase11_audit/`, `phase12_audit/`, `docs/p3x-*`, `docs/p4x-*`, `docs/phase*_audit.md`, `scripts/p31_*`, `scripts/tsan_p3*` | Mover para `docs/archive/`. Fora do caminho de quem chega |
| Aba **Ticks** | Sai das abas. Grupos de tick e `physicsDt` vão para Configurações do projeto |
| Chips de snap permanentes na tela | Viram um toggle dentro do menu de ferramenta |
| `BuildConfig.PHASE_LABEL` e legenda de fase no splash | Versão = `versionName`. O splash não carrega texto de fase |

### 3.2 Congelar (fica no código, sem trabalho novo até o fluxo fechar)

- `PerfGovernor`, ADPF térmico, logic LOD. O culling por câmera fica.
- Backend Vulkan **ou** GLES: escolher **um** como caminho de produto pelo
  dado do aparelho (o C33 tem Unisoc T612 / Mali-G57). Medir os dois uma vez
  no roteiro da seção 7 e ficar com o que for estável. O outro continua
  compilando, mas sem features novas.
- Gizmo (setas, halo, clamp): já está bom. Nenhum ajuste visual até haver
  feedback do aparelho.
- `eng::ui` (UI de jogo): congelado até o Player existir. Depois vira o HUD
  dos jogos.
- Qualquer coisa 3D.

### 3.3 Manter (é o ativo do projeto)

ECS, Scene + SceneSerializer + catálogo único de componentes com
ComponentContract, física, animação, partículas, NI-Script + VM, assets e
registry, undo por snapshot, `eng::build` (que passa a ser **usado**).

---

## 4. Fluxo novo do usuário

```
[Início]──► lista de projetos (card: miniatura, nome, data)
   │          + Novo projeto (modelo: Vazio | Plataforma 2D | Top-down)
   ▼
[Editor]
   ┌───────────────────────────────────────────┐
   │ ← Projeto ▾        Cena ▾         ▶ Jogar │  ← top bar (3 itens)
   │                                           │
   │               VIEWPORT                    │
   │     (gizmo único: mover; alças de         │
   │      rotação/escala aparecem na seleção)  │
   │                                   ↶ ↷     │
   ├───────────────────────────────────────────┤
   │  Cena   │  Propriedades  │  Biblioteca  │ + │ ← 3 abas + ação rápida
   └───────────────────────────────────────────┘
   │
   ├─ tocar numa entidade → Propriedades abre sozinha (meia altura)
   ├─ "+" → Sprite / Entidade vazia / Câmera / Texto / Script… (com modelo)
   ├─ Biblioteca = Imagens · Sons · Scripts · Animações · Materiais (filtro)
   │     script abre a janela de código; animação abre o editor de animação
   ▼
[Jogar] ──► tela cheia, pill ■ ❚❚, fps só no debug
   ▼
[Exportar] (menu Projeto) ──► valida (scripts compilam, cena inicial
            definida) ──► gera bundle ──► APK instalável / .goni
   ▼
[Player] ── roda o bundle sem o editor
```

Mudanças concretas em relação a hoje:

| Hoje | P5 |
|---|---|
| 6 abas (Hierarquia, Inspector, Assets, Scripts, Animação, Ticks) | 3 abas: **Cena · Propriedades · Biblioteca** |
| Barra de ferramenta Select/Move/Rotation/Scale sempre visível | Gizmo combinado. A ferramenta vira um botão que só aparece com seleção |
| Tocar numa entidade e depois ir na aba Inspector | A seleção abre Propriedades sozinha |
| "Nova entidade" e depois "Adicionar componente" (a ordem certa só vem do contrato) | "+" oferece **modelos** (Sprite com física, Personagem, Câmera…) já com os componentes na ordem certa |
| Scripts e animações em abas próprias | Itens da Biblioteca por tipo |
| Configurações de física/ticks/grade em abas e chips | Uma folha de Configurações do projeto |
| Sem exportação de jogo | Projeto → **Exportar jogo** |

---

## 5. Lógica nova (arquitetura)

### 5.1 C++: `EditorDocument` vira serviços + um despachante de comandos

```
EditorSession
 ├─ ProjectService     (abrir/criar/salvar/listar, settings)
 ├─ SceneService       (entidades, hierarquia, componentes via Inspector)
 ├─ Selection          (seleção + revisão)
 ├─ History            (undo/redo por snapshot; já existe, só é extraído)
 ├─ PlaySession        (clone, NiRuntime, tick, pausa)
 ├─ LibraryService     (assets, scripts, animações, materiais: um só registry)
 └─ ExportService      (liga eng::build — NOVO)

CommandBus::execute(Command) -> Result
   1. valida (contrato + modo Edit/Play)
   2. snapshot p/ History (se muta)
   3. aplica
   4. marca dirty + incrementa revisão
```

Regra: **nenhum** serviço muta a cena por fora do `CommandBus`. Hoje cada
método do documento decide sozinho se grava undo, se marca dirty e se
recusa em Play. É daí que vêm os casos especiais.

### 5.2 JNI: de 130 funções para cerca de 15

| Grupo | Funções |
|---|---|
| Ciclo de vida | `create`, `destroy`, `onPause`, `onResume` |
| Surface/frame | `surfaceCreated/Changed/Destroyed`, `renderFrame(dt)` |
| Entrada | `viewportTouch(pointer, action, x, y)` (gizmo, pan/zoom e toque do jogo decididos no C++) |
| Estado | `snapshot(sinceRevision) -> JSON` (retorna vazio se nada mudou) |
| Ação | `command(json) -> {ok, error, result}` |
| Consulta pesada | `query(json) -> JSON` (campos do inspector, listas da biblioteca, diagnósticos de script) |

JSON único (nlohmann já está no projeto) no lugar de TSV ad hoc. Um arquivo
`EditorProtocol.md` documenta os comandos. O teste de contrato JNI passa a
ser um teste de protocolo **no Linux**, sem APK.

### 5.3 Player (fecha o ciclo)

- `PlayerActivity` + `PlayerJni.cpp`: abre um bundle do `eng::build`, cria
  Scene + PlaySession sem editor, input e áudio, e roda.
- O mesmo código de `PlaySession` do editor. Garante que o jogo exportado se
  comporta igual ao ▶ do editor.
- Export: `ExportService` gera o bundle. Primeira entrega: bundle `.goni`
  aberto pelo Player no próprio app ("Jogar exportado"). Segunda entrega:
  APK standalone (template de APK + bundle em `assets/`, assinado com chave
  de debug).

### 5.4 Kotlin: tela = função do snapshot

- `EditorActivity` fica fino (surface + roteamento).
- `EditorState` (data class desserializada do snapshot) + `EditorViewModel`
  (aplica o snapshot, envia comandos).
- Telas/painéis como classes próprias: `ProjectsScreen`, `SceneSheet`,
  `PropertiesSheet`, `LibrarySheet`, `SettingsSheet`, `ScriptWindow`
  (mantém), `AnimationEditor`.
- Fim do polling de revisão por campo: um `snapshot(sinceRevision)` por
  frame, barato quando nada mudou.

**Decisão pendente: Compose ou Views?**
Recomendação: **Jetpack Compose** para a shell (o viewport continua como
`SurfaceView` via `AndroidView`). Motivo: toda a complexidade de "sync
diferencial para não perder o foco do IME" existe porque a UI imperativa é
reconstruída à mão. No Compose isso é o comportamento padrão. Custo: entra
AndroidX/Compose (+~3–4 MB no APK) e o `minSdk` 24 continua ok. Se você
preferir zero dependências, dá para fazer com Views, mas a divisão em classes
e o estado único (5.4) valem nos dois casos.

---

## 6. Ordem de execução

Cada etapa só começa quando a anterior estiver **verde no CI e validada no
C33**. Cada etapa termina com um APK que você instala.

### Etapa 0 — Limpeza (sem mudança de comportamento)
- Arquivar os docs e scripts de fase (3.1). Reescrever o README com 1 página:
  o que é, como buildar, estado.
- Varrer os comentários de histórico (`P4.x`, `FASE`, `round`, `bloco`).
- Remover os marks de boot, a auditoria de chrome e o watchdog do caminho
  padrão; deixar um crash handler.
- Dividir `EditorTests.cpp` por área (scene, play, scripts, library, history).
- **Pronto quando:** o APK abre, cria entidade e dá play como antes; o diff
  é só remoção/movimento.

### Etapa 1 — Núcleo de comando
- Extrair os serviços de `EditorDocument` e criar o `CommandBus`.
- Implementar `snapshot` / `command` / `query` no JNI **ao lado** dos antigos.
- Testes de protocolo no Linux cobrindo todos os comandos que a UI usa hoje.
- **Pronto quando:** cada ação atual da UI tem um comando equivalente testado.

### Etapa 2 — Shell nova de UI
- Tela de projetos, editor com 3 abas, "+" com modelos, Propriedades
  automática, Configurações unificadas.
- Migrar a UI para `snapshot`/`command` e **apagar** as ~115 funções JNI
  antigas no mesmo PR em que a última chamada sumir.
- **Pronto quando:** o roteiro de aceite (seção 7) passa no C33.

### Etapa 3 — Fechar o ciclo
- `ExportService` + `PlayerActivity`. "Exportar" → "Jogar exportado".
- Depois: APK standalone.
- **Pronto quando:** um jogo de plataforma simples feito **no celular** é
  exportado e roda no Player com o mesmo comportamento do ▶.

### Etapa 4 — Só então, polimento guiado por uso
- Lista curta vinda do uso real no aparelho. Nada de "rounds" de
  microajuste visual sem um defeito observado.

---

## 7. Roteiro de aceite no aparelho (substitui a matriz de 30 linhas)

Um único roteiro, executado a cada etapa, com resultado registrado em uma
linha no PR:

1. Abrir o app → lista de projetos em < 2 s.
2. Novo projeto "Plataforma 2D".
3. "+" → Personagem; mover no viewport; desfazer/refazer.
4. Importar imagem da galeria → aplicar no personagem.
5. Criar script de movimento, compilar, anexar.
6. ▶ Jogar: o personagem anda por toque e colide com o chão. ■ volta à
   edição intacta.
7. Fechar o app, reabrir: tudo salvo.
8. Exportar → Jogar exportado: comportamento igual ao passo 6.

---

## 8. O que NÃO fazer na P5

- Nenhuma feature de engine nova (3D, shaders, luzes novas, rede).
- Nenhum documento de auditoria/evidência por bloco. O PR descreve o que
  mudou, e o roteiro da seção 7 diz se funciona.
- Nenhum ajuste visual sem defeito observado no aparelho.
- Nenhuma otimização de performance sem uma medição que mostre o problema
  no C33.

---

## 9. Decisões que preciso de você

1. **Compose ou Views** para a shell nova (recomendação: Compose).
2. **Backend de produto no C33:** qual dos dois roda estável no seu
   aparelho hoje? Se você não souber, eu meço na Etapa 0 e congelo o outro.
3. **Primeiro tipo de jogo alvo** para os modelos de projeto e para o
   roteiro de aceite (recomendação: plataforma 2D).
4. **Export:** começar pelo bundle aberto no próprio app (recomendação) ou ir
   direto ao APK standalone.
