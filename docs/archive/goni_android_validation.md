# G.ONI — Validação Android em Dispositivo (§21/§33)

> Regra: CI verde NÃO é validação de produto (§36). Este registro existe
> para separar o que foi comprovado em HARDWARE do que não foi. Enquanto
> uma linha não for preenchida com evidência, o subsistema correspondente
> é **NOT DEVICE VALIDATED**.

## Dispositivo de referência (alvo)

| Campo | Valor |
|---|---|
| Modelo | **Realme C33** (primário de referência — missão §33) |
| SoC/CPU | Unisoc T612, octa-core (4×A75 + 4×A55) — **arm64-v8a** |
| GPU | Mali-G57 MP1 |
| Android | 12 (Realme UI GO Edition) *(confirmar na instalação)* |
| Vulkan | Mali-G57: Vulkan 1.1 no papel — validar `vkEnumerateInstanceLayerProperties` no logcat |
| GLES | OpenGL ES 3.2 |
| RAM | 3/4 GB |

## Builds a validar

| APK | Commit | CI | Sha256 | Status |
|---|---|---|---|---|
| goni-debug-f391c28.apk (CI Android artifact) | `f391c28` | verde | `a5fe81566f80449d7f6d365755ce2b363fc92783435a031e6b158b3cbc81579b` | **PENDENTE** |
| app-debug.apk (CI Android artifact) | `3cade4a` | verde | `e4863448204601cca740b38bbc5cd45cca7b39b56681d6600e557577b915234c` | **PENDENTE** (superseded) |

> `f391c28` é o primeiro candidato com a FASE 0 completa: imagem no
> viewport com tamanho/orientação/seleção corretos (reproduzida e corrigida
> com validação por pixel no Linux; APK auditado — libgoni.so contém o
> readback novo e os 8 fixes B1-B8).

## Roteiro de aceitação §32 (preencher por passo)

| # | Passo | Resultado | Observação |
|---|---|---|---|
| 1 | Instalar e abrir G.ONI | | |
| 2 | Criar projeto novo | | (anota o comportamento do toast/estrutura) |
| 3 | Criar cena | | |
| 4 | Criar entidade/Tick | | |
| 5 | Criar Sprite/Image Tick | | |
| 6 | Importar imagem (SAF) | | **o bug nº 1 — validar com atenção** |
| 7 | Ver a imagem em Assets | | |
| 8 | Ver a imagem no viewport | | |
| 9–11 | Mover / rotacionar / escalar | | |
| 12 | Editar no Inspector | | |
| 13–14 | Adicionar colisão + ver o shape | | |
| 15–18 | Criar/compilar/anexar NI-Script | | **o bug nº 2 — validar com atenção** |
| 19–22 | Play: script+física+render | | |
| 23–24 | Stop + Save | | |
| 25–27 | Fechar, reabrir, cena intacta | | |
| 28–34 | 3D: modo, cubo, câmera, Play | | **bloqueado até P1** (não implementado — §15) |
| 35–38 | Material, luz | | **bloqueado até P1** (§17/§19) |
| 39–41 | Save/reload 3D | | **bloqueado até P1** |

## Métricas a registrar na primeira execução validada

- [ ] Versão do Android real / build number
- [ ] CPU/ABI confirmado (`adb shell getprop ro.product.cpu.abi`)
- [ ] GPU/renderer ativo (logcat: `Backend selected` / `GPU renderer`)
- [ ] Backend escolhido (auto/vulkan/gles) e API version
- [ ) FPS médio do editor e em Play (Choreographer)
- [ ] Comportamento de startup (tempo até primeiro frame)
- [ ] Erros de logcat (`adb logcat -s GONI`)

## Pendências conhecidas que o teste pode expor

1. Staging de import: o SAF copia para `<workspace>/.import_tmp/` — o
   arquivo é movido (renamed) para o projeto no import; o diretório vazio
   permanece (cosmético — filtrado do seletor de projetos).
2. Primeira execução cria automaticamente o projeto "MeuJogo"
   (`ensureProjectOnFirstRun`).
3. Se a imagem importada não decodificar (PNG/JPG/WebP corrompido), o
   import é revertido com erro preciso — comportamento esperado.
