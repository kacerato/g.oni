# G.ONI — Auditoria de Recuperação (RECOVERY / FULL FUNCTIONALITY FREEZE)

> Diretiva mestra: congelar features novas e tornar o produto existente
> funcional de ponta a ponta. Este documento é a auditoria viva do estado
> real — não do estado desejado. Atualizado a cada reparo significativo.

Data: 2026-09-19 · Commit de referência: `3cade4a` · Ciclo: P0-RECOVERY

---

## 1. Os dois bugs que abriram a recuperação (reproduzidos e corrigidos)

Relato do usuário em APK instalado em dispositivo Android:

| # | Sintoma no APK | Causa raiz (auditada) | Status |
|---|----------------|----------------------|--------|
| 1 | `InvalidArgument: AssetBrowser: destino absoluto é proibido` ao importar asset | `EditorActivity` passa o workspace **absoluto** (`filesDir/projects`) por JNI; `EditorDocument` o recebia inteiro (violando o próprio contrato §8.1); `ProjectPaths::assetsRoot()` computava absoluto; a validação **correta** de `AssetBrowser::import` rejeitava | **CORRIGIDO** — `3cade4a` |
| 2 | `InvalidArgument: EditorDocument: caminho absoluto proibido` ao criar script | Mesma cadeia: `scriptWrite` valida `path.isAbsolute()` e o path carregava o root físico | **CORRIGIDO** — `3cade4a` |

**Por que o CI nunca pegou:** a suíte Linux usava workspace **relativo**
(a topologia de teste), nunca a topologia Android (workspace absoluto). A
validação anti-absoluto estava certa; a **conversão de fronteira** que
faltava era a camada de mapeamento do workspace.

## 2. A correção (arquitetural — sem enfraquecer segurança)

NENHUMA validação foi removida. O que foi adicionado é a camada que a
arquitetura exigia (missão §3):

```
Android/SAF URI absoluto            (aquisição — papel da plataforma, §D7)
        ↓  Kotlin copia o stream para staging DENTRO do workspace
eng::fs::RootedFileSystem           (workspace mapping — A camada nova)
        ↓  (root / path).normalized()  — absoluto entra e MORRE aqui
EditorDocument / AssetBrowser       (paths relativos VALIDADOS)
        ↓  validações anti-absoluto/anti-traversal INTACTAS
FileSystem base (Native/Memory)     (I/O real)
```

Contratos do `RootedFileSystem` (engine/fs, ADR-027 estendido):

- Entrada é sempre relativa: **absoluto → `InvalidArgument`** (violação de
  fronteira é bug do chamador, não se corrige calando);
- `..` que escapasse do root → `InvalidArgument`;
- join **normalizado** (paridade com `MemoryFileSystem::keyOf`): `./a` e
  `a` são o mesmo lugar;
- `list()` re-relativiza: o chamador **nunca vê nem persiste o root**;
- `EditorHost::create` absorve o root físico e entrega `Path{"."}` ao
  documento — o editor inteiro opera relativo ao workspace.

## 3. Defeitos adicionais encontrados e corrigidos no ciclo

| Defeito | Onde | Correção |
|---------|------|----------|
| Diálogo "Carregar cena" listava `<workspace>/scenes` — sempre vazio (UI morta) | `EditorActivity.loadSceneDialog` | Lista `<workspace>/<projeto>/scenes` (o mesmo que `ProjectPaths` resolve) |
| Diretórios de staging (`.import_tmp`) apareciam como "projetos" no seletor | `EditorActivity.openProjectDialog` | Filtro de ocultos |

### 3.1 FASE 0 — "a imagem não aparece corretamente no viewport" (6 defeitos)

Rastreio completo do pipeline (AssetBrowser → decode → upload GPU →
ViewportRenderer → pixel), com **reprodução visual** lendo pixel real da
surface (llvmpipe) e teste de regressão de 24 asserções:

| # | Defeito | Causa raiz | Correção |
|---|---------|-----------|----------|
| B1 | Sprite desenhado com **metade** do tamanho mundial | fator 0.5 espúrio no half-extent NDC (`worldW*zoom*0.5/w`) — a imagem ficava menor que a própria borda de seleção | `ViewportRenderer`: half-extent = total_px/w |
| B2 | Sprite renderizado **de ponta-cabeça** | stb decodifica top-down; GL tem v=0 na BASE — ninguém convertia | `TextureCache`: flip de linhas NA FRONTEIRA de upload; contrato: UV do sprite = imagem em pé |
| B3 | Imagem real cobria a tela inteira ("mar de cor") | `pixelsPerUnit` default 1 → foto 1080px = 51.840px de tela | default 48 (1 texel = 1 px de tela no zoom padrão) |
| B4 | Toque na imagem não selecionava | hit-test usava escala local, não o tamanho desenhado (região/ppu) | `EntityQuad` carrega dimensões da textura; hit usa a mesma fórmula do renderer |
| B5 | Contorno do collider com metade do tamanho físico | meia-extensão mundial tratada como offset NDC direto (faltava o fator 2 do px→NDC) | ×2 — o autor vê o shape que a física resolve (promessa §10) |
| B6 | Marcadores/partículas com metade do tamanho; grade 0.7px em vez de 1.4px | mesmo erro de conversão px→NDC do B1 | corrigidos no mesmo lugar |
| B7 | Nomes de asset curtos (≤ comprimento da extensão) perdiam a extensão | guard `size() >= ext.size()+1` em `AssetBrowser::import` | guard correto `size() >= ext.size()` |
| B8 | Import de textura com nome longo falhava na validação JNI | validação lia `name` (sem extensão) em vez do arquivo final (com extensão) | `import` devolve `outFinalName`; JNI valida pelo nome final |

Além disso: o teste "host renderiza sprite TEXTURIZADO" usava janela
`nullptr` e **skipava até no CI** — o upload de textura nunca tinha sido
validado de verdade (é por isso que B2 sobreviveu). Agora usa janela-marker
e roda.

### 3.2 Instrumento permanente: readback de pixels

`RhiBackend::readCenterPixel` (virtual, default `NotSupported`) +
implementação GLES + passthrough `Renderer` — o instrumento da regra
"feature visual é validada visualmente" para todos os testes futuros.

## 4. Cobertura de regressão (§26 — "nunca mais")

Teste `editor: host com workspace ABSOLUTO — import e script funcionam
(regressão Android §26)` (EditorTests, roda no Linux com a **topologia
Android**: workspace absoluto em tmpdir, exatamente o que a Activity passa
por JNI):

1. `newProject` → estrutura completa criada;
2. staging `.import_tmp` → `import` (o bug nº 1) → **ok**, listado,
   registrado com id;
3. `scriptCreate` (o bug nº 2) + `scriptWrite` > 512 bytes → **ok**,
   round-trip idêntico;
4. `saveScene`/`loadScene` → hierarquia + transform íntegros;
5. `project.goni.json`, `asset_registry.json`, cena: **zero paths
   absolutos persistidos**;
6. Segundo host abre o projeto: assets e scripts sobrevivem.

Mais 4 casos unitários do `RootedFileSystem` em FsTests (mapeamento,
rejeição de absoluto/escape, `list` relativo, paridade Memory/Native).

## 5. Modelo de status por subsistema (§31)

Legenda: `CODE_ONLY` → `EDITOR_PARTIAL` → `EDITOR_FUNCTIONAL` →
`PERSISTENT` → `RUNTIME_FUNCTIONAL` → `APK_FUNCTIONAL` →
`DEVICE_VALIDATED`.

| Subsistema | Status | Evidência |
|-----------|--------|-----------|
| Project lifecycle (new/open/save/settings) | APK_FUNCTIONAL | §26 regression test; CI Android verde |
| Scene lifecycle (new/save/load/reopen) | APK_FUNCTIONAL | idem (round-trip com transform) |
| Asset import + registry + thumbnails | APK_FUNCTIONAL | idem; preview/preview thumb na Activity |
| Script editor (create/write/compile/assign) | APK_FUNCTIONAL | idem; painel P0-7 completo |
| 2D viewport (grid, câmera, pan/zoom, seleção) | APK_FUNCTIONAL | suíte editor: câmera world↔screen, hit-test, quads por hierarquia, renderer GLES/lavapipe + Vulkan |
| Sprite/textura (import→GPU→tela) | APK_FUNCTIONAL | "host renderiza sprite TEXTURIZADO" (decode→RHI→bind→draw, UV completo) |
| Inspector (leitura/escrita real de dados) | APK_FUNCTIONAL | 12 casos inspector + gameplay catálogo; campos com kind/options |
| Física/colisão em Play | APK_FUNCTIONAL | PhysicsTick timestep fixo sobre clone; Collider (shape/layer/mask/trigger) registrado e serializado |
| NI-Script em Play | APK_FUNCTIONAL | "PLAY roda scripts NI-Script do clone" + VM determinística (FASE 11) |
| Play/Stop (clone/restore, separação editor×runtime) | APK_FUNCTIONAL | play clona, edição rejeitada, mutação não vaza |
| 3D viewport/objetos | CODE_ONLY | backends 3D (Vulkan/GLES, meshes) existem e renderizam; autoría 3D no editor NÃO existe ainda (§15–18 do roadmap de recuperação) |
| Workspace boundary (absoluto↔relativo) | APK_FUNCTIONAL | este commit; **DEVICE_VALIDATED pendente** |

**Tudo acima é `NOT DEVICE VALIDATED` até o APK deste commit ser testado
em dispositivo físico (§33).** O apontamento de todos os status no nível
APK vem de: suíte Linux com backends REAIS (llvmpipe/lavapipe — os mesmos
binários do APK) + CI Android verde + inspeção do APK (o fix está no
`libgoni.so` do artefato). Não é validação de CI passando por
validação de produto (§36).

## 6. Auditoria do APK (§22)

Artefato do CI Android `3cade4a` (`app-debug.apk`):

| Componente | Tamanho (não-comprimido) | Observação |
|-----------|--------------------------|------------|
| APK total | 3,62 MB (download comprimido ~1,87 MB) | build **debug** |
| `classes.dex` | 2,31 MB | Kotlin debug — maior componente; release com minify reduziria muito |
| `lib/arm64-v8a/libgoni.so` | 1,45 MB | **a engine inteira**: RHI Vulkan+GLES, editor, NI-Script VM, física, ECS, serialização |
| `lib/arm64-v8a/libc++_shared.so` | 1,29 MB | libc++ debug sem strip |
| ABI | arm64-v8a apenas | compatível com Realme C33 (Unisoc T612) |

Verificação de conteúdo: `RootedFileSystem` presente no `libgoni.so`
(16 referências de símbolo + mensagens da fronteira). O APK contém a
funcionalidade nativa exigida — o tamanho não é artificial nem inflado;
é um debug build honesto. O caminho de otimização (release + minify +
strip de libc++) fica registrado para o marco pós-recuperação.

## 7. Próximos passos (ordem §37)

1. **P0.5**: instalar o APK `3cade4a` no Realme C33 e executar o teste de
   aceitação §32 (passos 1–27) — registrar em `goni_android_validation.md`;
2. fechar os buracos de UI morta remanescentes da matriz (§23);
3. P1 (3D viewport de verdade) somente após P0.5 validado em dispositivo.
