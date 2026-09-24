# G.ONI

[![CI Linux](https://github.com/criandojogodenovo-sketch/g.oni/actions/workflows/ci-linux.yml/badge.svg)](https://github.com/criandojogodenovo-sketch/g.oni/actions/workflows/ci-linux.yml)
[![CI Android](https://github.com/criandojogodenovo-sketch/g.oni/actions/workflows/ci-android.yml/badge.svg)](https://github.com/criandojogodenovo-sketch/g.oni/actions/workflows/ci-android.yml)

Editor de jogos 2D que roda no celular. Você cria o jogo, testa com um
toque em **Jogar** e exporta, tudo no aparelho.

- **Motor** em C++20 (ECS, cena, física 2D, animação, partículas, áudio,
  renderização Vulkan/GLES) com a linguagem de script própria **NI-Script**.
- **Editor Android** em Kotlin + Jetpack Compose.

## Fluxo

1. **Início**: lista dos seus jogos. *Novo jogo* cria a partir de um modelo:
   *Plataforma 2D* já vem com chão, plataforma, personagem que anda e pula,
   e câmera que o segue. *Vazio* traz só a câmera.
2. **Editor**: a cena ocupa a tela. Embaixo ficam três abas:
   - **Cena**: a lista de objetos.
   - **Propriedades**: os componentes do objeto selecionado.
   - **Biblioteca**: imagens, sons, scripts, animações e materiais.

   O botão **+** adiciona objetos prontos (sprite, personagem, chão, caixa
   física, câmera, luz, partículas, som).
3. **Jogar**: roda a cena em tela cheia com controles de toque. **■** volta
   à edição sem perder nada.
4. **Exportar**: no menu do jogo, *Exportar (.goni)* gera um arquivo que
   qualquer G.ONI importa e joga.

Tudo é salvo sozinho ao sair do editor ou do app.

## Estrutura

```
engine/      módulos do motor (eng::core, ecs, scene, physics, niscript, rhi…)
editor/      núcleo do editor em C++ (documento, inspector, viewport, protocolo)
android/app  Activity, controlador e ponte JNI (libgoni.so)
android/ui   interface Compose (sem dependência do motor; testes de screenshot)
docs/        arquitetura, ADRs, NI-Script, protocolo do editor, plano P5
```

A interface conversa com o motor por um protocolo JSON único
(`snapshot` + `call`), descrito em
[docs/editor-protocol.md](docs/editor-protocol.md). Toda a lógica do editor
é C++ testado no Linux; o Kotlin só desenha o estado e envia operações.

## Build

Linux (motor + editor + testes):

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug --output-on-failure
```

Android (APK de debug):

```bash
cd android
./gradlew assembleDebug        # app/build/outputs/apk/debug/app-debug.apk
./gradlew :ui:recordPaparazziDebug   # regrava os screenshots da interface
```

Detalhes: [docs/build.md](docs/build.md) e
[docs/build-android.md](docs/build-android.md).

## Documentação

- [docs/p5-reformulacao.md](docs/p5-reformulacao.md): o plano atual (o que
  saiu, fluxo novo, próximos passos)
- [docs/editor-protocol.md](docs/editor-protocol.md): operações do editor
- [docs/ni-script/](docs/ni-script/): a linguagem de script
- [docs/architecture/](docs/architecture/) e [docs/adr/](docs/adr/):
  arquitetura do motor
- [docs/archive/](docs/archive/): auditorias e relatórios das fases antigas
