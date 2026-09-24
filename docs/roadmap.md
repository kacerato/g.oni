# Roadmap

As fases 1–12 construíram o motor (núcleo, ECS, cena, serialização, RHI
Vulkan/GLES, input, áudio, física, animação, partículas, NI-Script, build).
Os relatórios delas estão em [archive/](archive/).

O trabalho atual é a **P5**: fechar o ciclo do produto (criar → jogar →
exportar) com uma interface nova. Plano, cortes e estado em
[p5-reformulacao.md](p5-reformulacao.md).

| Etapa | Escopo | Estado |
|---|---|---|
| 0 | Limpeza: docs/scripts arquivados, runtime-demo removido, marcações de fase fora dos comentários | feito |
| 1 | Protocolo único do editor (`snapshot`/`call`), JNI de 130 → 21 funções, modelos de entidade e de projeto | feito |
| 2 | Interface nova em Compose (Início, editor com 3 abas, modo jogo, editor de script) | feito, falta validar no aparelho |
| 3 | Exportar/importar `.goni` e jogar sem o editor | feito dentro do app; APK independente pendente |
| 4 | Polimento guiado pelo uso no aparelho | a fazer |
