# G.ONI — Arquitetura de Autoria (authoring path architecture)

> Como o dado do usuário atravessa o editor até a GPU e volta — o mapa
> que a auditoria de recuperação usa para traçar qualquer bug de ponta a
> ponta (missão §1/§3).

Commit de referência: `3cade4a` (P0-RECOVERY).

## 1. Fronteira de armazenamento (a correção do P0)

O problema arquitetural que quebrou o APK: a representação de
armazenamento do Android (paths absolutos de `filesDir`, URIs de SAF)
vazava para DENTRO da abstração de projeto-relativo da engine. A regra
agora é estrutural:

```
Camada                        Representação           Quem converte
─────────────────────────────────────────────────────────────────────
Kotlin / SAF            URI content:// e File abs  (aquisição — plataforma)
                               │ ACTION_OPEN_DOCUMENT → InputStream
                               ▼ copia p/ staging DENTRO do workspace
JNI (EditorJni.cpp)     jstring → buffer local     (fronteira 1:1, sem lógica)
                               ▼
EditorHost::create      root FÍSICO (abs ou ".")    ABSORVE o root aqui
                               ▼
eng::fs::RootedFileSystem                        ← A CAMADA DE MAPEAMENTO
   (root / path).normalized()                    (absoluto morre aqui)
   list() → re-relativizado ao root
                               ▼
EditorDocument          paths RELATIVOS ao workspace (contrato §8.1)
AssetBrowser            validação: relativo, sem ".." — INTACTA
ProjectPaths (ADR-032) resolve() NORMALIZA (relativo no disco)
                               ▼
FileSystem base         Native (real) / Memory (testes)
```

Invariantes (testados em FsTests + §26):

1. Path absoluto NUNCA chega ao EditorDocument — quem tentar recebe
   `InvalidArgument` na fronteira (RootedFileSystem), não um comportamento
   silencioso.
2. Nada de absoluto é persistido: `project.goni.json`,
   `asset_registry.json` e cenas carregam apenas relativos (regressão
   verifica byte a byte).
3. `list()` devolve paths na forma que o chamador usaria para voltar ao
   arquivo (relativo ao workspace, normalizado) — o AssetBrowser cruza
   com `meta.sourcePath` sem conversão.

## 2. Estrutura de projeto no disco (§4)

```
<workspace>/                       ← filesDir/projects no Android
├── MeuJogo/
│   ├── project.goni.json          ← config: sceneRoots=["scenes"],
│   │                                 assetRegistryPath (relativo)
│   ├── scenes/main.json           ← SceneSerializer (ADR-033)
│   └── assets/
│       ├── asset_registry.json    ← AssetId → sourcePath (relativo)
│       ├── textures/  scripts/  models/  materials/
│       ├── shaders/    audio/     scenes/  prefabs/  json/
└── .import_tmp/                   ← staging SAF (copiado e movido p/ dentro)
```

- `AssetId` (UUIDv4, ADR-028) é a identidade; renomear/mover asset
  preserva o id (ADR-029).
- Projetos são portáteis: mover o workspace não invalida nada porque
  NENHUM caminho absoluto é persistido.

## 3. Pipeline de autoria 2D (P0 completo)

```
Import (SAF)          staging .import_tmp/x.png (Kotlin copia o stream)
  → AssetBrowser::import   valida relativo → mkdirs → rename → meta upsert
  → eng::image::decode     rejeita imagem corrompida NO IMPORT (P0-2)
SpriteData            componente refletido no catálogo (kind texture)
  → textureAsset      nome do asset de textura (Inspector com picker)
  → TextureCache      decode→RHI TextureHandle (upload sob demanda)
  → ViewportRenderer  quad com UV completo → GLES/Vulkan → tela
Inspector             fieldPath → reflection → dado REAL do Tick
  → muda transform/cor/textura → quads do viewport refletem
Play (§8.7, ADR-044)  clone por serialização → runtime opera o clone
  → TickScheduler (P0-5): física(timestep fixo)/animação/partículas/
    scripts(NI-Script VM)/câmera de jogo → render do clone
Stop                  clone destruído; authoring intacto
```

## 4. Camadas de execução (Tick — P0-5, ADR-051)

O editor expõe a abstração **Tick** (o usuário não vê o ECS):

| Tick de sistema | Fase | Consumidor do editor |
|---|---|---|
| PhysicsTick | fixa | Collider/RigidBody/CharacterBody (componentes) |
| AnimationTick | fixa | Animator + camadas |
| ParticleTick | fixa | ParticleEmitter |
| ScriptTick (NI) | lógica | NiScriptComponent (attach por nome) |
| CameraTickSystem | câmera | CameraData (ativa/zoom; cai p/ editor se inativa) |

## 5. Onde cada validação mora (defesa em profundidade)

| Camada | Validação | Erro |
|---|---|---|
| RootedFileSystem | relativo, sem escape, não-vazio | `RootedFileSystem: path absoluto proibido na fronteira…` |
| EditorDocument | `isSafeRelativePath` em scene/script paths | `…path relativo obrigatório (sem ..)` |
| AssetBrowser | import com path temporário relativo + destino não-absoluto | `…destino absoluto é proibido` |
| ProjectFile::parse | config com assetRegistryPath/sceneRoots relativos | `InvalidArgument` (regra dura §2.6) |
| SceneSerializer | componentes conhecidos apenas | erro preciso com campo |

Cada camada continua validando o que a anterior garantiu — nenhuma
confia na anterior às cegas. A diferença pós-`3cade4a` é que agora a
CONVERSÃO acontece exatamente uma vez, na fronteira certa.
