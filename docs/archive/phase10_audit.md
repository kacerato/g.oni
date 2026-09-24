> **CORREÇÃO (auditoria final 4–10):** os itens D4/D6 deste documento
> foram corrigidos na remediação — o viewport AGORA desenha partículas
> como quads (`buildParticleQuads`, drift D6) e o cross-fade AGORA é
> aplicado ao nó (C-13: estado de blend no componente Animator, o
> `AnimationSystem` compõe as poses); `snapToGround` implementado (C-18);
> o teste de raycast "origem-dentro" referia-se apenas a origem externa.
> Ver `docs/final_phase4_10_audit.md`.

# Auditoria + Design — FASE 10: Physics · Animation · Particles

- **Base:** `969ac2d` (HEAD FASE 9), tree limpa, 23/23 suites nos dois
  presets (revalidado).
- **Missão:** §7 — sistemas fundamentais de gameplay físico e visual,
  sobre ECS (§7 intro), SEM Vulkan/GLES dentro deles.

## Decisões (audit D1–D6 + design em um documento — escopo técnico)

- **D1 — Física própria, mínima e correta** (nada de Jolt/Bullet): o
  roadmap antigo citava Jolt; a missão atual pede um PRIMEIRO sistema
  (RigidBody/Collider/Trigger/CharacterBody/raycast/timestep fixo).
  Primitivas: esfera + AABB (rotação de box fica para fase futura —
  documentado). Integração semi-implícita de Euler; resolução por
  projeção posicional + impulso; mass=0 → estático.
- **D2 — Collision filtering:** `layer/mask` por Collider
  (`A.layer & B.mask && B.layer & A.mask`); triggers geram CONTATOS sem
  resolução; contatos expostos p/ gameplay (§7.2).
- **D3 — Timestep fixo (§7.6):** `PhysicsWorld::step(scene, fixedDt)`
  chamado via ACUMULADOR no tick do runtime (frame dt → N passos fixos
  de 1/60 + resto carregado). Determinismo testado (mesmo tempo total,
  fatiamentos diferentes → mesmo estado).
- **D4 — Animação sobre transforms:** clips com keyframes TRS (position
  lerp, rotation SLERP, scale lerp); Animator (componente) com
  play/pause/stop/loop/speed/seek; máquina de estados com transições
  por cross-fade linear. Skeletal: §7.11 — a hierarquia de nós É a
  preparação (pose = transforms); skinning/mesh fica como extensão
  futura DOCUMENTADA (sem mesh renderer ainda).
- **D5 — Partículas CPU v1 (§7.13):** spawn por acumulador de rate
  (determinístico), integração por velocidade+gravidade, vida/size/
  rotation, morte → remontagem da pool. GPU fica como evolução
  (compute/texturas não existem no RHI) — decisão registrada no ADR.
- **D6 — Integração com o editor (§8 FASE 10):** RigidBody/Collider/
  CharacterBody/Animator/ParticleEmitter são componentes REFLETIDOS e
  registrados no SceneSerializer (aparecem no inspector e persistem na
  cena); em PLAY o tick do EditorDocument avança física (timestep fixo),
  animação e partículas; o viewport desenha partículas como quads
  (mesmo pipeline pos+cor). APIs 100% C++ (§9 — prontas para NI-Script).

## Layout

```text
engine/physics/    eng::physics — RigidBody/Collider/CharacterBody,
                              PhysicsWorld (step/raycast/contatos)
engine/animation/  eng::animation — AnimationClip/Animator/States/
                              AnimationSystem (TRS por interpolação)
engine/particles/  eng::particles — ParticleEmitter/ParticlePool/
                              ParticleSystem (CPU, determinístico)
```
Dependências: physics/animation/particles → core/math/scene (ECS).
Nenhum conhece RHI/Android. O HOST renderiza (quads pos+cor).

## Testes (§7.14)

- Physics: criação de body/collider; gravidade integra; velocity/
  force/impulse; colisão esfera-esfera/AABB/esfera-AABB; trigger SEM
  resolução; layers/masks filtram; raycast (hit/point/normal/distância/
  miss/primeiro hit); timestep fixo determinístico; CharacterBody
  move-and-slide contra estático.
- Animation: clip/interpolação (lerp/slerp); play/pause/stop/loop/speed/
  seek; transição com cross-fade; estado inválido não crasha.
- Particles: emitter spawn por rate; lifetime/update; destruição;
  determinismo (mesma sequência de dt → mesmo estado); gravidade.

## Não-alvos (honestos)

Box OBB/contínua/joints/solver iterativo profissional; skinning;
GPU particles; materiais. Documentados como extensões.
