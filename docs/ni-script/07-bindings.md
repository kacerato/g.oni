# NI-Script — 07: Bindings (runtime C++)

> FASE 11 · implementação: `engine/niscript/src/Bindings.cpp` +
> `editor/src/NiRuntime.cpp` · design §7

## Três camadas

```
engine/niscript (linguagem pura — core/math/ecs/reflect)
   │
   ├── NiNativeTable   &BL + nativos de HOST (fechada em COMPILE time)
   ├── NiHost          interface ABSTRATA (delta/ações/spawn/find/…)
   └── NiBindingTable  resolução de caminhos e.<alias>.<campo>…
                          ▲ registrada pelo CONSUMIDOR (editor/testes)
```

O núcleo NÃO conhece componentes de gameplay: a tabela de bindings é
montada por quem RODA os scripts — mesmo padrão do catálogo do
SceneSerializer (ADR-043) e do Inspector (auditoria D2: reuso do
reflection, zero hard-code).

## &BL — biblilioteca base (pura)

Visível apenas com `add &BL`. Funções PURAS (sem estado, sem I/O):

```
abs(x) ceil(x) floor(x) sqrt(x)* sin(x) cos(x)      → float
min(a,b) max(a,b) clamp(x,lo,hi)**                  → tipo dos argumentos
str(x)  — representação textual determinística de QUALQUER valor
         (compview e nil recusados)                  → string
len(s)  — comprimento em BYTES UTF-8                → int
vec2(x,y) vec3(x,y,z)                               → vec2/vec3
color(r,g,b) color(r,g,b,a)                          → color (ARIADICA 3..4)
transform(pos, rotGraus, scale)                      → transform
i(x)    — →int com checagem de range (NaN/inf/overflow = Fault)
fl(x)   — →float (o nome `f` COLIDE com a keyword `f` — decisão v1)
comp(e, "alias")                                    → CompView (interno)
```
\* sqrt de negativo é Fault `Range` (não NaN silencioso).
\*\* clamp com mínimo > máximo é Fault `BadArgument`.

## Nativos de HOST (sobre NiHost)

Sempre visíveis (não exigem `add &BL`):

```
delta()                → float    (deltaSeconds do tick)
action_down(s)         → bool
action_pressed(s)      → bool
action_released(s)     → bool
spawn("nome")          → entity   (falha de spawn = Fault, não nil)
despawn(e)             → bool
self()                 → entity   (entidade dona da instância)
find("nome")           → entity   (ausente = Fault, não nil)

move(dx,dy)            → bool     (P4.6 — semântica B5 abaixo)
move_and_slide(dx,dy)  → bool     (P4.6 — CharacterBody; false = Fault)
teleport(x,y)          → bool     (P4.7.0 B5 — SEMPRE cru)
camera.zoom(z)         → bool     (P4.7.0 B4 — primeira câmera ativa)
camera.position(x,y)   → bool     (P4.7.0 B4 — offsets da câmera)
camera.follow("nome")  → bool     (P4.7.0 B4 — "" solta o follow)
```

### Nativos de jogo do editor (NiRuntime::registerNatives)

Registrados pelo runtime do editor (o `script.compile` usa a mesma
tabela, então o editor de script reconhece todos):

```
random()               → float    [0, 1)
random(min, max)       → float    [min, max)
random_int(min, max)   → int      inteiro entre min e max, inclusive
global_set("k", n)     → bool     valor compartilhado entre scripts
global_get("k")        → float    0 se nunca definido
time()                 → float    segundos desde o Play
restart()              → bool     recomeça a cena no fim do frame
view_left() / view_right() / view_bottom() / view_top() → float
                                  bordas do que a câmera mostra (mundo)
play_sound("x.wav", v?) → bool    toca um som do projeto (volume 0–1)
count("nome")          → int      entidades vivas com esse nome
log(valor)             → bool     aparece no HUD do Play
```

`spawn("Nome")` clona um **molde** (entidade com o componente Molde e
todos os filhos) e devolve a cópia ativa, já com scripts. Os moldes ficam
fora do jogo; `despawn` vale no fim do frame. O `random` muda a cada Play;
os testes fixam a semente (`EditorDocument::setScriptSeed`) para serem
reproduzíveis.

`NiHost` é implementado pelo consumidor (editor: clone + runtimeInput_;
testes: harness próprio). Sem host configurado, os nativos de host falham
com `NativeError` (mensagens explícitas — nunca crash).

## Movimento de gameplay (P4.6 / P4.7.0 B5 — kinematic_sweep)

Três verbos, três semânticas — o autor distingue de cara:

| verbo | com kinematic_sweep ON (default) | com sweep OFF |
|---|---|---|
| `move(dx,dy)` | KINEMATIC com Collider: **VARRIDO** (TOI+slide — para e desliza na parede, nunca atravessa). Demais entidades: cru. | cru para todos (pré-P4.7) |
| `move_and_slide(dx,dy)` | varredura da esfera do **CharacterBody** (sem CharacterBody = Fault — nunca deslize silencioso) | idem (não muda com sweep) |
| `teleport(x,y)` | **SEMPRE cru** — atravessa por design (spawn/reposicionamento) | idem |

- A varredura do kinematic (B5) é a MESMA matemática do moveAndSlide:
  motion fatiado em substeps de ≤ meio raio (anti-túnel, teto 64);
  raio da esfera = `Collider` do próprio corpo (Sphere = radius;
  Box = círculo inscrito na meia-extensão mínima); o **mask do próprio
  corpo** decide contra quem desliza.
- A **ESCRITA de `position`** (`e.position.x = …`) resolve IGUAL ao
  `move` quando o corpo é KINEMATIC com Collider e sweep ON — script
  que move por position TAMBÉM colide. `teleport` é a válvula de
  escape (por design atravessa).
- O setting vive na CENA (`"physicsKinematicSweep"`, chave aditiva —
  arquivo antigo/ausente = ON: o script ingênuo COLIDE por padrão).

## Tabela de componentes (NiBindingTable)

Entradas `{alias → get/set}` resolvem caminhos `e.<alias>.<campo>…`:

- `NiBindingTable::get(entity, "position.x")` — primeiro segmento =
  alias, o resto = subcaminho;
- o ADAPTADOR REFLETIDO (`niAddReflectionBinding`) lê/escreve por
  **OFFSET** via `TypeInfo` — o mesmo mecanismo do Inspector/serialização:
  - `f32/f64 → float` (escrita f64→f32 checa NaN/inf/overflow);
  - `i8..i64/u8..u64 → int` (range checado; u64 > i63 = Fault);
  - `bool/string → bool/string`; `eng::math::Vec3/Vec2 → vec3/vec2`
    (inteiros E subcampos);
  - `eng::math::Quat → SÓ subcampos` x/y/z/w (quat inteiro não é valor
    do script — conversão euler↔quat é papel de binding custom);
  - enum → int POR VALOR (nome do enumerador: v2 — planejado);
  - struct desconhecida → recursão por caminho (subcampos).
- `basePath` desloca a raiz para DENTRO do componente — é assim que o
  açúcar funciona: alias `position` = Transform + basePath "position"
  ⇒ `e.position` é o vec3 e `e.position.x` o float.

### O que o EDITOR registra (editor/src/NiRuntime.cpp)

```
position  → Transform.position        (refletido, basePath + WRAP B5:
                                        a ESCRITA resolve varredura do
                                        kinematic — ver seção acima)
scale     → Transform.scale            (refletido, basePath)
rotation  → Transform.rotation         CUSTOM euler↔quat em GRAUS
transform → Transform inteiro          (refletido)
name      → Name.value                 (refletido, basePath)
<catálogo inteiro>: alias canônico ("eng::physics::RigidBody")
                    + apelido curto ("rigidbody") — refletidos via
                    ComponentEntry (get/getMutable type-erased)
```

Ou seja: `e.rigidbody.velocity.x = 1.0` e
`comp(e, "eng::physics::RigidBody").velocity.x` são o mesmo acesso.

## Semântica de acesso — leitura vs escrita

- LEITURA `e.<campo>`: entidade nula → `EntityNull`; obsoleta →
  `EntityStale`; componente ausente → `ComponentMissing`; campo
  desconhecido → `FieldUnknown`. TODOS reparáveis (§5.3).
- ESCRITA `e.<campo> = v`: mesmas checagens + conversão com range
  (nunca escrita parcial — o fault acontece ANTES do memcpy).

## Limitações conhecidas (honestas)

- campos de componente cujo NOME colide com keyword (`f`, `stop`, …)
  não são acessíveis pela sintaxe de ponto (a lexagem os captura como
  keywords) — renomeie o campo C++;
- o catálogo não conhece `std::vector<T>` (reflexão v1 — ADR-021);
  coleções no script são futuras;
- os bindings são por EXECUÇÃO no editor (recriados no play — o world do
  clone não sobrevive ao stop por ADR-044).

## E2E (teste da missão)

`ni E2E: fonte .nis muda componente ECS via reflexão` —
`.nis → compile → bytecode (inspecionado) → VM → binding → MUDANÇA no
componente ECS` com assert nos valores finais; integração editor:
`editor: PLAY roda scripts NI-Script do clone` (clone + start/update/
destroy + edição intacta).
