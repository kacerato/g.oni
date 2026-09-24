# FASE 11 — NI-Script: design formal (pré-implementação)

> **Estado**: design aprovado ANTES da implementação (requisito da missão:
> "semântica de `repeat`/`repair`/`timeout` definida formalmente antes de
> codificar"). Este documento é a fonte canônica; divergência entre aqui e o
> código = bug do código.
>
> **Linguagem**: NI-Script (`.nis`) — linguagem própria do G.oni. NÃO embute
> Lua, NÃO usa Python, NÃO roda em camada de navegador. Compilação UMA VEZ
> para bytecode; o VM executa bytecode (sem JIT — ADR-049).

## 1. Pipeline

```
.nis (fonte UTF-8)
  → Lexer          (tokens + linha/coluna)
  → Parser         (AST + verificação sintática)
  → Sema           (símbolos, inferência de tipos, checagens estáticas)
  → Compiler       (bytecode + constantes + source map linha/coluna)
  → NI VM          (execução determinística com orçamento de instruções)
  → runtime C++    (bindings: host interface + adaptador de componentes)
```

Cada etapa devolve `Result` com diagnósticos `{linha, coluna, severidade,
mensagem}`. A compilação inteira é pura (sem estado global, sem I/O, sem
relógio) — mesmo input produz o MESMO bytecode (testado).

## 2. Sintaxe (gramática)

Forma de bloco: blocos são terminados por `stop` — SEM semântica de
indentação e SEM chaves obrigatórias. `stop` fecha exatamente o bloco aberto
mais recente que ainda não foi fechado (casamento por estrutura, não por
indentação).

```
script      := { topLevel }
topLevel    := addStmt | funcDef | handlerDef | varDecl
addStmt     := "add" "&" IDENT                          ; módulo
funcDef     := "f" IDENT "(" [params] ")" ":" block "stop"
handlerDef  := "up" IDENT ":" block "stop"              ; evento
varDecl     := "var" IDENT [":" type] ["=" expr]

block       := { stmt }
stmt        := varDecl | ifStmt | repeatStmt | repairStmt | timeoutStmt
             | linkStmt | emitStmt | giveStmt | assignStmt | exprStmt
ifStmt      := "if" expr ":" block "stop" [ "else" ":" block "stop" ]
repeatStmt  := "repeat" expr ":" block "stop"
repairStmt  := "repair" ":" block "stop"
timeoutStmt := "timeout" expr ":" block "stop"
linkStmt    := "link" "to" expr
emitStmt    := "emit" IDENT
giveStmt    := "give" [ expr ]
assignStmt  := lvalue "=" expr
lvalue      := IDENT | lvalue "." IDENT
exprStmt    := expr

expr        := orExpr
orExpr      := andExpr { "or" andExpr }
andExpr    := notExpr { "and" notExpr }
notExpr     := "not" notExpr | cmpExpr
cmpExpr     := addExpr [ ("=="|"!="|"<"|"<="|">"|">=") addExpr ]
addExpr     := mulExpr { ("+"|"-") mulExpr }
mulExpr     := unary { ("*"|"/"|"%") unary }
unary       := ("-"|"not") unary | postfix
postfix     := primary { "." IDENT }                     ; acesso a campo
primary     := INT | FLOAT | STRING | "true" | "false"
             | IDENT | IDENT "(" [args] ")"              ; chamada
             | "(" expr ")"

type        := "int" | "float" | "bool" | "string" | "vec2" | "vec3"
             | "color" | "entity" | "asset" | "transform"
```

Comentários: `#` até o fim da linha. Sem comentários de bloco (v1).

## 3. Valores e tipos

| Tipo        | Representação no VM        | Zero-value (`var x: T`) |
|-------------|----------------------------|--------------------------|
| `int`       | i64 com sinal              | 0                        |
| `float`     | f64 (IEEE-754)             | 0.0                      |
| `bool`      | 1 byte                     | false                    |
| `string`    | UTF-8 imutável             | ""                       |
| `vec2`      | {x,y} f64                  | (0,0)                    |
| `vec3`      | {x,y,z} f64                | (0,0,0)                  |
| `color`     | {r,g,b,a} f64, 0..1        | (0,0,0,1)                |
| `entity`    | u32 índice + u32 geração   | entidade nula (índice 0xFFFFFFFF) |
| `asset`     | u64 AssetId                | 0                        |
| `transform` | {pos vec3, rot vec3 (graus), scale vec3} | identidade |

`var` exige inicializador OU anotação de tipo (erro de compilação sem
nenhum dos dois). NÃO existe literal `nil` na linguagem: todo valor
declarado tem valor definido (zero-value ou inicializador). Internamente o
VM usa um estado "nil" apenas para locais não inicializadas — inacessível
ao script.

`entity` é um HANDLE geracional, nunca um ponteiro: um `entity` obsoleto
usado em qualquer operação é NO-OP SEGURO ou FAULT reparável (dependendo da
operação — §7.3), NUNCA UB (a segurança de geração vem do próprio ECS,
ADR-024; o VM apenas propaga a checagem).

### 3.1 Regras de inferência

- Literal `42` → `int`; `1.5` → `float`; `"…"` → `string`; `true/false` → `bool`.
- Binário aritmético: `int∘int → int`; se QUALQUER lado `float` → `float`.
  `-` só para numéricos.
- `/`: `int/int → int` (divisão TRUNCADA em direção a zero — documentado;
  divisão por zero = Fault); `float` sempre `float`.
- `%`: só `int` (sinal do dividendo, como C++); `%` por zero = Fault.
- Comparações: numéricos misturados permitidos → `bool`; `==`/`!=` também
  para `string`, `entity`, `asset`, `vec2/3`, `color`, `transform` (igualdade
  estrutural). `< <= > >=` só numéricos.
- `and`/`or`/`not`: só `bool` (curto-circuito: o RHS não é avaliado
  se o LHS decide).
- `+` para `string` = concatenação (`"a" + "b"`); `string + int/float` NÃO
  (erro estático) — use `str(x)` (nativo de &BL).
- Aritmética de `vec2/vec3`: `v+w`, `v-w`, `v*escalar`, `escalar*v`, `-v`.
  `color`/`transform`/`entity`/`asset`: SEM aritmética (erro estático).
- Acesso a campo `.`:
  - base `vec2` → `x,y`; base `vec3` → `x,y,z`; base `color` → `r,g,b,a`
    (checado EM COMPILE TIME quando o tipo da base é conhecido);
  - base `transform` → `position`, `rotation`, `scale` (vec3 cada; rotação
    em GRAUS Euler XYZ — conversão para Quat acontece na fronteira do
    binding, mesma convenção do editor);
  - base `entity` → caminho DINÂMICO resolvido pelos bindings (§7.3) —
    `e.position`, `e.position.x`, `e.name` … (a tabela de bindings define
    o que existe; resolução é por reflexão);
  - qualquer outro tipo base → erro de compilação.
- Tipo de `var` é FIXO na declaração (inferido ou anotado). Atribuição com
  tipo incompatível conhecido estaticamente → erro de compilação; a
  checagem dinâmica restante acontece nos bindings (Fault reparável).
- Anotação `: type` + inicializador: tipos devem CASAR (erro de compilação
  se o inicializador tem tipo estático diferente — `var x: float = 1` é
  erro; escreva `1.0`).

## 4. Estrutura do programa

- `f nome(params): … stop` — função. Parâmetros: `nome` ou `nome: type`.
  Chamada com número errado de argumentos = erro de compilação. `give expr`
  devolve; cair no `stop` final devolve nil implícito (consumir nil em
  expressão = Fault em runtime, reparável). `give` só é permitido DENTRO de
  `f` (erro de compilação em `up`/topo).
- `up EVENTO: … stop` — manipulador de evento do script. Eventos:
  - `up start` — disparado 1x quando a instância nasce (host);
  - `up update` — disparado 1x por tick (host);
  - `up destroy` — best-effort no encerramento (host);
  - `up <nome>` — customizado: disparado por `emit <nome>` (§5) no MESMO
    script, ou PROPAGADO por links.
  Evento duplicado no MESMO script = erro de compilação. A ordem de
  execução quando vários scripts ouvem o mesmo evento é a ordem de
  INSTÂNCIA dos scripts na cena (determinística — §7.4).
- `add &MODULO` — importa módulo. Disponível em v1: `&BL` (biblioteca base:
  matemática pura + construtores + `str`). `add &X` com X desconhecido =
  erro de compilação. Nativos de `&BL` só são visíveis com o import
  (mecanismo de namespace por módulo; módulos futuros: `&UI`, `&NET` —
  APENAS planejados, NÃO implementados). `add &BL` duplicado = idempotente.
- `var` no TOPO do script = estado GLOBAL da instância (persiste entre
  eventos, vive na instância — não compartilhado entre entidades).
- `link to <expr:entity>` — registra aresta direcionada DA entidade do
  script PARA a entidade alvo na tabela de links da instância. Semântica de
  CONJUNTO (idempotente; duplicata não cria segunda aresta nem reordena).
  Auto-link (target == própria entidade do script) = erro de compilação
  quando estático, Fault em runtime. Limite: 64 links por instância (Fault
  além). Ordem de iteração: ORDEM DE INSERÇÃO (vector — determinístico).
- `emit NOME` — dispara handlers `up NOME`: (1) do script corrente,
  imediatamente (execução síncrona, empilhamento de frame); (2) propagação
  BFS pelos links: handlers `up NOME` de scripts cuja entidade é alcançável
  a partir da entidade do script emissor, com conjunto de visitados
  (imune a ciclos) e ordem determinística (BFS por ordem de inserção das
  arestas). O mesmo script não recebe o evento duas vezes na mesma emissão
  (deduplicado por visitados). Reentrância: `emit` dentro de handler é
  permitido (empilha; o orçamento de instruções é GLOBAL à execução e
  protege recursão infinita). Profundidade de emissão limitada a 32
  (Fault reparável além — proteção de estourar a pilha).

## 5. Semântica formal do fluxo de controle (REQUISITO DA MISSÃO)

### 5.1 `if/else`

```
if C: B1 stop else: B2 stop
```
Avalia `C` (deve ser `bool` — checado estático quando possível, senão
Fault em runtime). Se `C` = true executa `B1`; senão executa `B2`. Escopo:
`var` dentro de bloco é LOCAL ao bloco (escopo léxico aninhado).

### 5.2 `repeat` — iteração CONTROLADA

```
repeat N: B stop
```
1. `N` é avaliado UMA ÚNICA VEZ, ANTES da primeira iteração. `N` deve ser
   `int`. Se `N` for literal fora de `[0, 65536]` → erro de COMPILAÇÃO.
   Em runtime fora de `[0, 65536]` → RepeatFault (reparável).
2. Se `N <= 0`: corpo não executa (bloco vazio válido).
3. Caso contrário o corpo executa EXATAMENTE `N` vezes. NÃO existe
   `break`/`continue` em v1 (futuro: `halt` — planejado, não implementado).
4. Loop infinito é IMPOSSÍVEL por construção: cada instrução do corpo
   consome orçamento global (§6.2); orçamento esgotado = TimeoutFault.
5. `var` declarada no corpo é REINICIALIZADA a cada iteração (escopo por
   iteração); variáveis externas mantêm o valor entre iterações (fechamento
   lexical padrão).

### 5.3 `repair` — recuperação de falha

```
repair: G stop
```
`repair` é uma REGIÃO GUARDADA contra Faults de runtime (nunca erros de
compilação):

1. Entrar na região salva o estado do VM: profundidade da pilha de
   valores, profundidade da pilha de chamadas e o DEADLINE de timeout
   vigente. O CONTADOR GLOBAL de instruções NUNCA é restaurado (é
   monotônico — §6.2: impediria que repair loops esgotassem o orçamento
   global, virando DoS).
2. Se QUALQUER instrução dentro de `G` (inclusive dentro de chamadas `f`,
   blocos aninhados e handlers disparados por `emit` a partir de `G`) gerar
   um Fault, então:
   a. a execução de `G` é abandonada IMEDIATAMENTA (nenhuma instrução
      adicional de `G` executa);
   b. o VM restaura o estado salvo no passo 1 (frames criados dentro da
      região são descartados — os frames são PODs, sem destrutores a
      acionar);
   c. a execução continua na PRIMEIRA instrução após o `stop` da região;
   d. o Fault fica registrado em `NiScriptState::lastFault` (consultável do
      C++ para diagnóstico/logging — NÃO introspectável pelo script em v1;
      binding de fault-info é futuro planejado e NÃO implementado).
3. Atomicidade: como toda atribuição avalia o RHS COMPLETO antes da
   escrita, e só existe UM alvo por atribuição, uma instrução que falha NÃO
   aplica escrita parcial (o Fault ocorre antes do STORE). Efeitos de
   instruções ANTERIORES à que falhou NÃO são desfeitos (repair não é
   transação — documentado explicitamente).
4. Se nenhum Fault ocorre, a região é apenas um bloco (escopo léxico
   próprio).
5. `repair` NÃO aninha implicitamente: um Fault numa região guardada é
   SEMPRE capturado pela região mais INTERNA em cujo escopo dinâmico a
   instrução está. Fault fora de qualquer região = a execução do EVENTO
   corrente inteira é abandonada, o Fault registrado, e o próximo evento
   executa normalmente (o script NÃO morre — isolamento de evento).

### 5.4 `timeout` — orçamento de instruções (NÃO relógio)

```
timeout N: B stop
```
1. `N` é avaliado uma vez na entrada, deve ser `int >= 1`. Literal `< 1` →
   erro de compilação; runtime `< 1` → TimeoutFault imediato (reparável).
2. `N` define um ORÇAMENTO DE INSTRUÇÕES para `B`: o VM registra
   `deadline = instruções_executadas_até_aqui + N`. Cada instrução
   dispatchada dentro de `B` (inclusive chamadas/emit a partir de `B`)
   incrementa o contador global; quando `contador > deadline` →
   TimeoutFault (reparável pela região `repair` mais interna que envolve o
   `timeout` — o próprio `timeout` NÃO captura seu Fault).
3. POR QUE instrução e não segundos: um timeout por tempo real exige
   thread/relógio/interrupção → quebra o determinismo e viola "sem threads
   escondidas, sem alocação por frame". Orçamento de instruções é
   determinístico, testável e não aloca nada. Timeout por TEMPO DE JOGO
   (relógio simulado em pontos de determinismo explícitos) é FUTURO
   PLANEJADO e NÃO implementado.
4. Blocos `timeout` aninham: o deadline mais APERTADO (menor) vigora.
   Sair do bloco restaura o deadline anterior (salvo na entrada).

## 6. VM (máquina virtual NI)

### 6.1 Modelo

Stack machine sobre bytecode plano. Estado por EXECUÇÃO (não global):
- pilha de valores (vector com reserva; cresce só quando necessário —
  sem realocação por instrução no caminho quente);
- pilha de frames (função, pc, base da pilha, deadline do timeout);
- contador global de instruções.

### 6.2 Orçamento global de instruções

TODA execução de evento nasce com orçamento `kDefaultBudget = 1.000.000`
instruções (configurável por chamada no C++). Esgotar = TimeoutFault
tratado como Fault fora de região (§5.3.5). Proteção determinística contra
loop infinito/DoS sem threads.

### 6.3 Determinismo — garantias declaradas

1. Nenhum relógio, endereço, thread, RNG ou ordem-de-hash acessível ao
   script (nativos de &BL são funções puras; bindings são funções do
   estado do mundo — mesma entrada, mesmo efeito).
2. Floats: f64 IEEE-754; builds SEM fast-math (verificado nos presets —
   `-ffast-math`/`-Ofast` não aparecem em lugar nenhum).
3. Iteração de links e ordem de scripts: ordem de INSERÇÃO (vectors), nunca
   mapas hash.
4. Mesma cena + mesmos scripts + mesma sequência de eventos ⇒ mesmos
   efeitos (TESTADO — ver suite).
5. GC: não existe; valores são ownership direto (std::string, PODs).
   Alocação acontece em concatenação de strings e criação de valores —
   nada de alocação POR INSTRUÇÃO.

### 6.4 Bytecode

Instruções são `struct { OpCode op; std::uint32_t a, b; }`. Constantes:
pool único de int/float/bool/string. Cada função carrega `sourceMap:
pc → (linha, coluna)` para Faults com localização precisa.

### 6.5 Hooks de depuração

`NiVm::setTraceHook(fn)` — callback chamado a cada `stride` instruções
(configurável; default desligado) com `{pc, linha, coluna, opcode}`. Hook
não pode mutar o VM (visão const). UI de depuração no editor: ADIADA
(honesto — ver docs/ni-script/08). Breakpoints: não implementados em v1
(mecanismo do hook já permite um tracer de linha; planejado, não entregue).

## 7. Bindings (runtime C++)

### 7.1 Camadas

```
NI-Script (linguagem pura: core+math)          ← engine/niscript
  └── NiHost (interface ABSTRATA — sem engine)  ← implementada pelo CONSUMIDOR
        editor/runtime: deltaSeconds, ações de input, spawn/despawn
  └── NiBindingTable (resolução de caminhos de entidade)
        registro vive no CONSUMIDOR (editor) — MESMO padrão do catálogo do
        SceneSerializer (ADR-043): engine/niscript não conhece componentes
        de gameplay; o editor registra Transform/Name/RigidBody/... via
        adaptador REFLETIDO (offsets + typeName do TypeRegistry).
```

### 7.2 Nativos de `&BL` (biblioteca base — pura, determinística)

`abs(x) ceil(x) floor(x) sqrt(x) sin(x) cos(x) min(a,b) max(a,b)
clamp(x,lo,hi) str(x) len(s) vec2(x,y) vec3(x,y,z) color(r,g,b)
color(r,g,b,a) transform(pos, rotGraus, scale) i(x) fl(x)` —
conversões `i`/`fl` com checagem de range (o nome `f` colide com a
keyword de definição `f` — decisão de design v1) (fora de range = Fault). Sem
I/O, sem estado.

### 7.3 Bindings de entidade (dinâmicos, por reflexão)

- `e.<campo>` com base `entity` compila para `FIELD_GET/SET` com o CAMINHO
  completo como constante de string ("position", "position.x", …). A
  resolução é feita pela `NiBindingTable` em runtime:
  - adapters por componente: o adapter devolve/aceita `NiValue` lendo e
    escrevendo por OFFSET com o `TypeInfo` do reflect — MESMO mecanismo do
    Inspector (auditoria D2: reuso do reflection, zero hard-code);
  - açúcar de nó: `position`/`rotation`/`scale`/`name` mapeados para os
    componentes Transform/Name (registrados pelo adapter do editor);
  - `comp(e, "Nome")` → valor INTERNO compview (entidade + prefixo);
    `.campo` sobre compview prefixa o caminho — `comp(e,"RigidBody").velocity.x`
    tem a mesma semântica, nomes resolvidos pela tabela;
  - entidade inexistente/obsoleta em LEITURA → Fault reparável (rigor);
    componente ausente em LEITURA → Fault reparável (não nil — nil não é
    exposto à linguagem).
- Host natives (via `NiHost`): `delta()` `action_down("nome")`
  `action_pressed("nome")` `action_released("nome")` `spawn("nome")`
  `despawn(e)` `self()` `find("nome")`.

### 7.4 Instâncias e eventos

`NiScriptState` = { programa compartilhado, entidade self, globais,
tabela de links, lastFault }. O HOST mantém a lista de instâncias na ordem
de varredura da cena (determinística). `update` roda instância por
instância; `emit` propaga por BFS (§4).

## 8. Segurança (modelo declarado)

1. Nativos existem SÓ nas tabelas registradas em compile-time: nome
   desconhecido = erro de compilação; índice fora da tabela = IMPOSSÍVEL
   (bytecode só referencia índices validados pelo compiler).
2. NENHUM acesso a sistema de arquivos, shell, rede, ponteiros brutos ou
   memória crua. Strings são imutáveis e nunca interpretadas (sem eval).
3. Handles de entidade são valores (índice+geração) — nunca ponteiros
   cruzam a fronteira do VM.
4. Orçamento de instruções OBRIGATÓRIO em toda execução (§6.2) — script
   não pode travar o frame loop.
5. O bytecode é produzido SÓ pelo compiler (não há loader de bytecode
   externo em v1 — serialização de bytecode é futuro planejado; a
   recompilação por play é barata e mantém uma única fonte de verdade).

## 9. Plano de testes (resumo — detalhe na suite)

- Lexer: tokens, linha/coluna, comentários, erros precisos.
- Parser: cada forma; `stop` sem bloco aberto; `else` sem `if`.
- Sema: inferência (tabela de casos), erros de tipo/redeclaração/arity,
  `give` fora de `f`, `link to` não-entity, `add &X` desconhecido.
- VM: aritmética/promoções, curto-circuito, chamadas, recursão (fatorial),
  escopos, `repeat` (limites 0/65536/65537), `repair` (divisão por zero,
  entidade obsoleta, dentro de chamada, dentro de emit), `timeout`
  (orçamento exato, aninhado, interação com repair).
- Bindings: get/set por reflexão com componente de TESTE registrado; geração
  (entidade destruída → Fault e não-UB); e2e completo
  `.nis → compile → bytecode → VM → binding → MUDANÇA de componente ECS`.
- Determinismo: mesmo script/mundo duas vezes → efeitos idênticos.
- Segurança: natives fechados; orçamento esgotado; sem-nil.
- Integração editor: play compila scripts do clone; `up update` move
  entidade por N ticks; stop descarta; edição intacta.

## 10. Fora de escopo (honesto, NÃO entregue em v1)

- UI de script no editor (painel com editor de texto/realce) — o campo
  `source` do componente é editável pelo Inspector existente (string).
- Breakpoints UI, inspetor de variáveis do VM, profiler.
- Serialização de bytecode (cache de compilação).
- Arrays/dicionários como valores da linguagem; `break/continue`;
  corrotinas/`wait`; timeout por tempo de jogo; fault-info acessível ao
  script; módulos além de `&BL`; JIT (nunca por design).
- Multi-VM paralelo (jobs) — VM é single-thread por design (ADR-049).
