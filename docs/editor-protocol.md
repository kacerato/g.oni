# Protocolo do editor

A interface Android não chama o documento do editor diretamente. Ela usa
duas funções da `libgoni.so`, ambas em JSON:

- `snapshot(sinceKey)` devolve o estado atual, ou nada se a chave não
  mudou. É chamada uma vez por frame.
- `call(request)` executa uma operação e devolve
  `{"ok":true,"result":…}` ou `{"ok":false,"error":"mensagem"}`.

A implementação fica em `editor/src/EditorProtocol.cpp` e é testada no Linux
(`editor/tests/ProtocolTests.cpp`). A ponte JNI (`android/app/src/main/cpp/EditorJni.cpp`)
só repassa as strings. Os gestos do viewport (toque, gizmo, pan, zoom, toque
do jogo) não passam por JSON, porque rodam a cada evento de movimento.

Entidades são inteiros (`id`; `0` = nenhuma).

## Snapshot

```json
{
  "key": 123456,
  "project": {"name": "Voo", "folder": "Voo", "controls": "tap", "orientation": "portrait"},
  "scene": {"path": "main.json", "dirty": true},
  "mode": "edit",
  "paused": false,
  "previewing": false,
  "selection": 4294967299,
  "tool": 1,
  "snap": {"translate": false, "rotate": false},
  "canUndo": true,
  "canRedo": false,
  "hierarchy": [{"id": 1, "name": "Cano", "depth": 0, "kind": "empty", "template": true}]
}
```

`kind` é o ícone da entidade: `sprite`, `camera`, `character`, `light`,
`particles`, `audio`, `script`, `text` ou `empty`. `template` marca um
molde (componente Molde, ele ou um ancestral): fica fora do jogo e serve
de base para `spawn("Nome")`.

## Operações

| op | argumentos | resultado |
|---|---|---|
| `project.list` | | pastas dos projetos |
| `project.templates` | | `[{id, title, description}]` — catálogo de exemplos |
| `project.new` | `name`, `template` (id do catálogo) | pasta criada |
| `project.open` | `folder` | |
| `project.ensure` | | pasta aberta ou criada |
| `project.save` | | |
| `project.rename` | `name` | |
| `project.exportZip` | `path` (relativo ao workspace) | |
| `project.importZip` | `path`, `name` | pasta criada (sufixo " 2" se já existir) |
| `scene.list` / `scene.new` | | nomes / |
| `scene.save` | `path` (".json" opcional) | |
| `scene.load` | `path` | |
| `entity.create` | `template`, `name`?, `parent`? | `id` (fica selecionada) |
| `entity.delete` / `entity.duplicate` | `id` | / novo `id` |
| `entity.rename` | `id`, `name` | |
| `entity.reparent` | `id`, `parent` (0 = raiz) | |
| `entity.select` | `id` (0 = limpar) | |
| `transform.get` | `id` | `{p:[x,y,z], r:[x,y,z], s:[x,y,z]}` (rotação em graus) |
| `transform.set` | `id`, `p`?, `r`?, `s`? | |
| `inspector` | `id` | `{id, name, components:[{name,label,category,removable,fields:[{path,type,value,kind,options?}]}]}` |

`kind` do campo diz o editor a usar: `number`, `int`, `bool`, `enum`
(com `options`), `color` ("#RRGGBB" ou "#RRGGBBAA"), `text`, `bitfield`
(camadas de colisão), `layer` (grupo de atualização), `texture`, `audio`,
`material`, `script`, `code`. A rotação do Transform vem em graus.
`component.set` recusa valores que deixariam a cena sem abrir (por
exemplo, uma camada que não existe).

| op | argumentos | resultado |
|---|---|---|
| `component.catalog` | `id` | componentes que ainda cabem na entidade |
| `component.add` | `id`, `name` | nomes adicionados (inclui dependências) |
| `component.remove` | `id`, `name` | |
| `component.set` | `id`, `component`, `path`, `value` | |
| `tool.set` | `tool` (0 selecionar, 1 mover, 2 girar, 3 escalar) | |
| `snap.set` | `translate`?, `rotate`? | |
| `viewport.fit` / `viewport.zoom` | / `factor`, `x`, `y` | |
| `history.undo` / `history.redo` | | |
| `play.start` / `play.stop` | | |
| `play.pause` | `paused` | |
| `play.hud` | | `{scripts:{found,compiled,failed,ticks,faults,compileError,fault}, audio, perf}` |
| `asset.categories` / `asset.list` | / `category` | `[{name,id,registered,path}]` |
| `asset.import` | `temp` (relativo ao workspace), `category`, `name` | nome final |
| `asset.rename` / `asset.delete` / `asset.move` | `category`, `name`, `newName` / … / `from`, `name`, `to` | |
| `asset.imageInfo` | `name` | `{w,h,alpha}` ou null |
| `texture.apply` | `id`, `name` | adiciona Sprite se faltar |
| `script.list` / `script.read` / `script.write` | / `name` / `name`, `content` | |
| `script.create` | `name` | nome com `.nis` |
| `script.delete` | `name` | |
| `script.compile` | `source` | `{ok, diags:[{line,col,message}]}` |
| `script.assign` | `id`, `name` | liga o componente ao arquivo |
| `anim.*` | `list`, `read`, `write`, `create`, `delete`, `assign`, `addFrame`, `setMeta`, `addKey`, `keys`, `setKey`, `deleteKey`, `preview`, `previewStop` | |
| `material.*` | `list`, `read`, `write`, `create`, `delete` | |
| `audio.preview` / `audio.stop` / `audio.playing` | `name` / / | / / bool |
| `settings.get` | | grade, passo de física, camadas, `game` |
| `settings.game` | `background` [r,g,b] 0–1?, `orientation` (`portrait` \| `landscape` \| `auto`)?, `controls` (`platformer` \| `tap` \| `none`)? | |
| `settings.grid` / `settings.physicsDt` / `settings.kinematicSweep` | | |
| `layer.add` / `layer.set` | `name`, `timeScale`?, `update`/`physics`/`render`? | |
| `collision.add` / `collision.rename` | `name` / `bit`, `name` | bit novo / |
| `host.info` / `host.backend` | / `name` | `{backend, perf, audio}` |

## Modelos

Entidades (`entity.create`): `empty`, `sprite`, `camera`, `ground` (chão
estático), `physics` (caixa dinâmica), `character` (jogador com
`jogador.nis`), `text`, `particles`, `light`, `audio`. Cada modelo é um
único passo de desfazer.

Projetos (`project.new`, listados por `project.templates`):

| id | o que vem pronto | tela / controles |
|---|---|---|
| `platformer` | chão, plataformas, jogador que anda e pula, câmera que segue | deitada / botões |
| `flappy` | pássaro com gravidade, molde "Cano" gerado sem parar, placar, recomeço ao bater | em pé / toque |
| `boxes` | cada toque solta uma caixa física que cai e empilha, contador | em pé / toque |
| `empty` | só a câmera | livre / botões |

Os três exemplos jogáveis são testados de ponta a ponta no Linux
(`[protocol]`): o Voo é jogado por um piloto automático até marcar pontos.

## Ajustes do jogo

Valem para todas as cenas e ficam em `project.goni.json` (chave `game`):

- `background`: cor de fundo no Play;
- `orientation`: a Activity trava a tela no Play (`portrait`, `landscape`)
  ou deixa livre (`auto`); no editor a tela é sempre livre;
- `controls`: o que a interface desenha por cima do jogo — `platformer`
  (◀ ▶ e pulo), `tap` (uma dica que some; a tela toda vira a ação `tap`)
  ou `none`.

## Controles do jogo

Sem configuração própria do projeto, o Play usa estas ações:

| ação | toque (fração da tela) | teclado |
|---|---|---|
| `left` | x 0–0,2, y 0,6–1 | ←, A |
| `right` | x 0,2–0,4, y 0,6–1 | →, D |
| `jump` | x 0,75–1, y 0,6–1 | Espaço, ↑ |
| `tap` | a tela toda | Espaço, Enter |
| `up` / `down` | | W, ↑ / S, ↓ |

A interface desenha as zonas de toque conforme o ajuste `controls`.
