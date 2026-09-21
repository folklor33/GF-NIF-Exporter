# Phase 7 findings — Angular / Three.js loader

**Goal of Phase 7:** write the consumer. A reusable Angular/TypeScript library that
loads `.gfmodel` + `.gfbin` (format v3), renders it with Three.js, and — the genuinely
new part — renders the particle systems. Plus a decision on the fur/alpha-test question
left open by Phase 2 §11.

**Result: the loader renders the corpus correctly, and for the first time in this
project the verification is visual and automated.** Across a 130-model sample spanning
all 8 entity types: **193 007 / 193 007 non-orphaned animation tracks actually bind**,
**685 / 685 particle systems render**, 0 texture load failures, 0 untranslated
modifiers, 0 load errors. The fur question is settled empirically (§4). Two **real
format defects** were found and are reported here without touching the exporter (§6);
one of them is more serious than the fur question ever was.

The library lives in the front-end repo, not here:
`GF-Database-Frontend/src/app/gf-model/`.

---

## 0. Scope decisions taken before any code

| Question | Answer |
|---|---|
| Integration | A self-contained module inside the existing app (`src/app/gf-model/`), not a separate `ng-packagr` library. |
| Migration | **Direct replacement.** The `.glb`/Blender path is gone from `three-d-viewer`. |
| Asset serving | Same versioned CDN as today: `models/<entity>/gfmodel/<X>.gfmodel` + `.gfbin`, textures in the existing `models/<entity>/texture/`. |
| Versions | Angular 17 kept; **three 0.169 → 0.186** and `@types/three` with it. |

Bumping three turned out to be nearly free: **three was imported by exactly one file in
the whole application** — `three-d-viewer.component.ts`, the file being rewritten. This
was checked before the bump, not after. (`three-obj-mtl-loader` in `package.json` is now
unreferenced; left in place, flagged here.)

three.quarks 0.17.1 requires `three >= 0.182`, which is what forced the version question.
Worth recording: **three.quarks 0.16.0 peers at `three >= 0.165` and would have worked on
0.169 unchanged**, so the bump was a choice, not a constraint.

---

## 1. Architecture

```
src/app/gf-model/
├── gf-model-format.ts        format v3 types, documenting the raw NIF semantics
├── gf-model-loader.service.ts fetch + parse + build; the only place Z-up→Y-up happens
├── gf-nif-render.ts          NIF alpha/blend state → Three.js material  (§4)
├── gf-particles.ts           NiParticleSystem → three.quarks             (§3)
├── gf-model-placement.ts     ground placement, camera forward, root-bone lock
└── gf-model-diagnostics.ts   what was lost silently                      (§5)
```

`three-d-viewer.component.ts` keeps its public API — `modelName`, `entityType` — so
`detail-monster`, `detail-npc` and `detail-item` are untouched. It is now only staging:
scene, camera, lights, controls, display toggles. Every NIF→Three translation is in the
library, which is what makes the harness (§7) able to exercise the real code.

### 1.1 The Z-up rule, made structural rather than remembered

The brief asks that the Z-up→Y-up conversion happen once, at display, and warns to watch
that it is not applied twice. Rather than rely on discipline, the loader returns **two**
objects: `content`, which is everything in Z-up, and `root`, a group whose only purpose
is to carry `rotation.x = -π/2` and hold `content`. Nothing else in the library touches
rotation. Ground placement and camera framing (`gf-model-placement.ts`) are documented as
world-space operations that must run on `root`, i.e. after the conversion — which is also
the only way they can be correct, since they reason about "down".

---

## 2. What was carried over from the previous viewer, deliberately

The replaced component had behaviours that were not obvious and are not re-derivable
from the format. They were kept and moved into `gf-model-placement.ts`:

- **Root/NonAccum bone rotation lock.** A `Bip01 NonAccum` bone carries root motion, but
  its *rotation* is meant for the original engine's movement-direction computation, not
  for display. Its translation must still apply.
- **Ground placement via `applyBoneTransform`.** GPU skinning means a `SkinnedMesh`'s
  bounding box stays on the bind pose and does not follow the displayed animated pose.
  Bone positions are not a substitute either — a bone is an internal pivot (the ankle),
  not the surface (the sole passes below it).
- **Forward angle from the ankle→toe vector**, with the fold-check that rejects a foot
  tucked under the body (a hovering flier), because its horizontal component no longer
  says anything about orientation.
- **Model-code candidates** (`N077 10` → `N077`, `W79401` → `W794`), tried in order.

One behaviour was **added**: `selectClip` now restores every bone to its bind pose before
starting a new clip. A clip only carries tracks for the bones it animates, and
`AnimationAction.stop()` does not restore properties it never touched, so switching from
a clip that moves a bone to one that ignores it left that bone stuck on its last pose.
The old `.glb` path had the same latent issue.

---

## 3. Particles

### 3.1 Engine choice: three.quarks, decided on one measured property

three.quarks was the candidate named in the brief and it is what was retained, but the
deciding factor is narrower than "its behaviours resemble Gamebryo modifiers":

**`MeshSurfaceEmitter` exists natively, and 6097 of the corpus's 6958 emitters (88%) are
`NiPSysMeshEmitter`** — they emit from a mesh *surface*, not a primitive. Reading its
implementation before committing: it samples triangles weighted by area and sets the
initial velocity along the **face normal**, which is exactly `meshInitialVelocityType: 0`,
the dominant case. An engine limited to point/sphere/box emitters would have meant writing
area-weighted surface sampling ourselves for 88% of the corpus. This was checked in the
package's source, not assumed from its documentation.

The rest of the mapping is term-for-term, which is the secondary reason to prefer it:

| NIF | three.quarks |
|---|---|
| `NiPSysMeshEmitter` | `MeshSurfaceEmitter` |
| `NiPSysBoxEmitter` (872) | custom `BoxVolumeShape` — there is no box shape in the library |
| `declination` / `planarAngle` + variations | custom `ConeDeflectedShape`, wrapping the base shape |
| `lifeSpan ± variation` | `startLife: IntervalValue` |
| `speed ± variation` | `startSpeed: IntervalValue` |
| `initialRadius ± variation` | `startSize: IntervalValue` |
| `NiPSysColorModifier` | `ColorOverLife(Gradient)` — **see §3.2** |
| `NiPSysGrowFadeModifier` | `SizeOverLife(PiecewiseBezier)` |
| `NiPSysRotationModifier` | `startRotation` + `RotationOverLife` |
| `NiPSysGravityModifier` | `ApplyForce` + `TurbulenceField` |
| `AgeDeath`/`BoundUpdate`/`Position`/`Spawn` | no-ops, documented as such |

The four no-op modifiers are explicitly listed rather than silently ignored, so the
diagnostic can distinguish "attached everywhere with template values" from "we forgot it".
`untranslatedModifiers` counted **0** across the sweep.

### 3.2 The black-RGB trap, and why the obvious reading is wrong

Phase 5 §14.4 flagged this as "worth an eye"; measured properly it is the difference
between the particle layer working and 62% of it being invisible.

The naive translation multiplies the texture by the `colorKeys` RGB. Measured on the
re-exported corpus:

```
colorKeys with black RGB                        : 16 892 / 18 632  (90.7%)
Systems in ADDITIVE blending (SRC_ALPHA → ONE)  :  6 426 / 6 958   (92.4%)
Systems that are BOTH additive AND all-black RGB:  4 339            (62.4%)
```

A black RGB under additive blending contributes exactly nothing. So the literal reading
makes **4339 systems — 62% of the corpus — completely invisible.**

The tempting counter-argument is that black is just how "faded out" is written, and that
alpha would be zero there anyway. **It is not:**

```
Black-RGB keys with alpha > 0                   :  6 482
All-black systems reaching a positive alpha peak:  4 733 / 4 733  (100%)
  ... of which reaching alpha 1.0               :  2 809
Systems whose keys mix black and non-black RGB  :  1 210
emitter.initialColor == [1,1,1,1]               :  6 958 / 6 958  (100%)
```

Every all-black system reaches a real opacity peak, and `initialColor` carries no colour
anywhere in the corpus. These are **opacity profiles**; the colour comes from the texture.

**Decision: a key whose RGB is black is treated as "no tint" (white), keeping its alpha
unchanged.** Per *key*, not per system, because 1210 systems mix the two. The 99 systems
with a black key strictly between two coloured ones can get a tint discontinuity from
this; that is 1.4% of systems against 62% that would otherwise be blank.

Every system this rule fires on is counted in
`diagnostics.particles.blackRgbColorProfiles` (620 of 685 in the sweep), so the correction
is visible rather than hidden.

### 3.3 Emitter geometry must be looked up in **both** arrays

The brief says the emission surfaces "are in `emitterMeshes`". Measured, that is true
**34% of the time**:

```
Mesh emitters                          : 6 097
  name resolves only in emitterMeshes  :   845
  name resolves only in meshes         : 4 014   <-- the majority
  resolves in both                     : 1 234
  resolves nowhere                     :     0
  emitter carries no mesh name at all  :     4
```

Phase 5 only routed to `emitterMeshes` those emission surfaces that were marked
*not-visible* in the NIF; the rest stayed in `meshes`. Looking only in `emitterMeshes`
would have dropped two thirds of the emitters. The loader searches both, **`emitterMeshes`
first** — a name present there denotes an emission surface by construction, whereas the
same name in `meshes` may denote any visible geometry.

`emitterMeshes` is still never rendered. Only its geometry is built, and only for
emission, exactly as Phase 5 §14.3 requires.

### 3.4 Emission geometry has to be re-expressed in the system's own space

Found visually, not by inspection: on `ride/R814` the effects floated *beside* the wolf
rather than on it.

Exported positions are either model-space (skinned mesh) or node-local (a mesh carried by
an animated node). The particle system lives under its attach node and emits in local
space (`worldSpace: false`), so feeding it raw positions offsets every particle by the
attach node's own transform. `bakeEmitterGeometry` applies
`inverse(system.matrixWorld) · sourceWorld` once, at build time. This forces a two-pass
build — parent everything first, then bake — because the system's world matrix does not
exist until it is parented.

### 3.5 Attach node vs emitter object node

Both are used, as Phase 5 §14.4 requires. `attachNodeIndex` parents the system, so it
rides the animated bone through Three.js's own matrix propagation with no per-frame code.
`emitterObjectNodeIndex` is the space the emission *shape* is expressed in — used for the
box emitter's dimensions. For a mesh emitter it is redundant with the mesh's own space,
which §3.4 already resolves.

### 3.6 What is *not* derivable, and is therefore a knob

Two values cannot be computed from the data and are isolated in `ParticleTuning` rather
than buried:

- **`emissionRateFactor`** — the birth rate is not in the format (§6.1). The loader uses
  `maxParticles / lifeSpan`, the rate that sustains exactly `maxParticles` live particles.
- **`radiusToSize`** — `initialRadius` is a Gamebryo radius, so the rendered quad is
  2×radius; three.quarks' `startSize` is the sprite size. Kept at 2, checked visually
  against 1 on `M491` and `R814`; 2 is right for `M491`'s orbiting crystals and the two
  are hard to separate on `R814`.

---

## 4. The fur question — resolved, with measurements

Phase 2 §11 left three untried directions. All three were implemented behind a
`FurStrategy` switch and compared on `ride/R814` at identical camera and pose, then
diffed pixel-by-pixel against the literal `alphaTest` render.

| Strategy | Pixels differing from `alphaTest` | Mean deviation | Verdict |
|---|---|---|---|
| `alphaToCoverage` | **0.42%** | 94.5 | **No useful effect** |
| `blend` (depthWrite off) | **10.81%** | 139.3 | **Breaks the whole model** |
| `blendWithDepthPrepass`, residual 4/255 | 2.54% | 59.5 | soft edge, but a new artifact |
| **`blendWithDepthPrepass`, residual 64/255** | **1.93%** | **32.0** | **retained** |

What the numbers mean, confirmed against the images:

- **`blend` alone is the worst option, not the simplest.** Phase 2 §11 guessed it was
  "the most direct fix". With `depthWrite: false` the sort collapses: R814's face
  markings wash out, the collar goes see-through, the inside of the wrist bands shows
  through. 10.81% of the image changes with a large deviation because it changes the
  *whole model*, not the fur edge. The fur self-overlaps heavily, which is exactly the
  case unsorted blending handles worst.
- **`alphaToCoverage` does essentially nothing here** — 0.42% of pixels. Caveat, stated
  plainly: the harness runs on SwiftShader, whose multisample support is not real, so
  **this row is not conclusive on hardware.** It is conclusive that it cannot be relied
  on as *the* answer, since it degrades to the literal cutout wherever MSAA is
  unavailable.
- **Blending while keeping depth writes is what works**, and the residual threshold is
  load-bearing. At 4/255, texels of alpha 0.02 were locking the depth buffer and produced
  a visible grey plate on R814's chest. At 64/255 that artifact is gone while the 100–150
  band — the one that produced the halo — still blends fully. Moving 4 → 64 changes 0.92%
  of the image, and the difference is entirely that artifact.

The retained render differs from the literal cutout on **1.93% of pixels with a mean
deviation of 32** — the correction is localised on the fringe, which is what a correct
fix should look like. This is the project's own principle applied: the NIF says "alpha
test", and the loader is not obliged to reproduce that mechanism literally.

**Performance note.** The blend path costs a depth sort that the cutout avoids, and it
applies to 77.7% of the corpus. It is bounded here by keeping `depthWrite: true`, so
these materials still occlude correctly and do not have to be sorted against each other
to look right — the sort only has to be approximately correct. All four strategies stay
selectable per load (`GfLoadOptions.furStrategy`), so this is reversible without a code
change if a real GPU shows otherwise.

---

## 5. Diagnostics

Three.js fails silently in exactly the three ways that matter here, so the loader counts
what actually happened rather than inferring it from the absence of an error:

- **Tracks.** A track whose path resolves to nothing is ignored without a word — the
  cause of Phase 4's first animation bug. `buildClips` redoes `PropertyBinding`'s own
  resolution (`parseTrackName` + `findNode`) against the real graph and counts hits, and
  separately counts tracks the *exporter* already marked orphaned (`boneIndex === -1`).
- **Textures.** `TextureLoader.load` returns a `Texture` immediately whose image arrives
  later; until then a `transparent` material samples zero alpha and the mesh **is drawn
  and perfectly invisible**. A render loop self-corrects, a one-shot render does not.
  The loader exposes `texturesReady` for that, and the texture counters are only final
  after it resolves. This was found by the harness on the first run — every model came
  back as an empty grid.
- **Particles.** Every system that is not rendered is recorded with its reason;
  ambiguous emitter-mesh picks (§6.2) and black-RGB corrections (§3.2) are counted too.

`showDiagnostics` on the component renders this as an overlay. `summarize()` gives the
one-line form used throughout this document.

---

## 6. Two format defects found — reported, exporter unchanged

Per the brief, these are reported before anything is changed, and **nothing in the
exporter was touched.**

### 6.1 `NiPSysEmitter::BirthRate` is not exported

`ParticleEmitterData` (`SceneModel.hpp:559`) stops at `lifeSpanVariation`. The birth rate
and its variation — the field that says how many particles per second the system emits —
are simply absent, on all 6958 systems.

**Consequence:** a consumer cannot reproduce the authored density. The loader falls back
to `maxParticles / lifeSpan` (§3.6), which is defensible — `maxParticles` comes from
`NiPSysData::GetVertexCount()`, which the authoring tool sizes on the authored birth
rate — but it is an inference, and it assumes every system runs permanently at capacity.

**Suggested fix, if taken:** extract `NiPSysEmitter`'s `Birth Rate` (and
`Birth Rate Variation`) the same `asString()` way the other emitter scalars are read.
Low risk: additive, no existing field moves.

### 6.2 `meshEmitterMeshNames` does not identify a mesh — this is the serious one

A `NiPSysMeshEmitter` names its emission surface by **name**. Names are not unique:

```
Mesh-emitter references                                 : 6 111
  ... whose name is carried by more than one mesh       : 4 145  (67.8%)
  ... where those meshes have genuinely different
      geometry (different vertex/index counts)          : 4 106  (67.2%)
```

`ride/R814` has two meshes both literally named `Editable Poly`, and five of its six
systems reference that exact name. `chair/C004` has **eight** distinct meshes named
`Editable Poly`. The format offers nothing to break the tie: there is no mesh index, and
`MeshData::nodeIndex` is `-1` for skinned meshes, so the mesh↔node link that
`emitterObjectNodeIndex` would need does not exist either.

Two disambiguation heuristics were tried before concluding:

- **Prefer `emitterMeshes` over `meshes`.** Well-founded — a name there denotes an
  emission surface by construction — and it is implemented (§3.3). It only helps the 1234
  references present in both.
- **Nearest bound bone to `emitterObjectNodeIndex`**, using each candidate's
  `skinBindings` and hierarchical bone distance. **Resolves only 1071 of 4145 (26%)**,
  with 1449 ties and 1625 cases with no usable bones. Measured, rejected as not reliable
  enough to be worth being confidently wrong.

So the loader's pick is deterministic (first entry of the priority array) and **counted**
in `diagnostics.particles.ambiguousEmitterMeshes` — 327 of 685 systems in the sweep, 48%.
It is visible rather than silent, but it is not verifiable.

**Suggested fix, if taken:** export a mesh *index* into `meshes`/`emitterMeshes` next to
the names, resolved at export time where the NIF pointer is still available
(`NiPSysMeshEmitter`'s own `GetPtrs()`, the same mechanism Phase 5 already used for
`emitterObject` and `gravityObject`). The names can stay for diagnostics. **This is worth
doing**: it affects two thirds of all mesh emitters, and unlike the fur question the
consumer genuinely cannot work around it.

---

## 7. Method: visual verification, finally automated

Every previous phase recorded the same limitation — "no headless browser available in
this environment" (PHASE4 §11/§14, PHASE5 §7/§14.5). **That is no longer true.** Edge is
installed, and Playwright drives it via `channel: 'msedge'` with no browser download:

```js
chromium.launch({ channel: 'msedge',
  args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'] })
```

`GF-Database-Frontend/tools/gf-harness/` renders any model and returns its full
diagnostic alongside the frame. It imports **the real library**; only the staging
(scene/camera/lights) is re-created, because the Angular component cannot be separated
from its application.

This paid for itself immediately. **Three real defects were found here and by nothing
else**, each of which the diagnostics reported as perfectly healthy:

1. Every mesh invisible, because textures had not loaded at render time (§5).
2. Particles floating beside the model instead of on it (§3.4).
3. The grey plate on R814's chest from the depth buffer (§4).

All three are cases where counters said 100% and the picture said otherwise — precisely
the failure mode the brief's methodology section warns about.

### 7.1 The sweep

130 models, sampled deterministically across all 8 entity types (the 10 largest per type
plus a regular stride through each):

```
Load errors                    : 0 / 130
Animation tracks               : 193 007 bound / 193 007 non-orphaned   (100.00%)
                                 12 orphaned at export (known, Phase 4)
Textures                       : 1 668 loaded / 1 738 referenced        (95.97%)
                                 58 missing at export (known corpus gap), 0 HTTP failures
Particle systems               : 685 rendered / 685                     (100.00%)
Untranslated modifiers         : 0
Unsupported emitter types      : 0
Ambiguous emitter-mesh picks   : 327 / 685  (§6.2)
Black-RGB colour profiles fixed: 620 / 685  (§3.2)
```

Every render was also scanned for coverage and luminance to catch models that "loaded
fine" but produced nothing. **Lowest coverage was 0.30%** (`effect/S13103`) and it is
legitimate — see §8.

### 7.2 Animation proven to move, not assumed

Phase 4's lesson is that absent movement is not evidence of anything. Each model was
rendered twice from the same clip at two times and the frames diffed:

| Model | Pixels changed t=0 → t=0.7 s |
|---|---|
| `monster/M009` | 120 417 (10.45%) |
| `monster/M389` | 109 469 (9.50%) |
| `monster/M069` | 85 946 (7.46%) |
| `item/WA85` (t=1.4 s) | 27 462 (2.38%) |

`WA85`'s small figure is the expected one — 2 keyframes over 1.5 s, real but very slow
motion, as the brief describes.

---

## 8. Known limitations

- **Camera framing ignores particles.** It is computed from `Box3.setFromObject`, i.e.
  geometry only. On a model whose visible content is essentially its effects
  (`effect/S13103`), the framing centres on near-nothing and the effect sits off-frame.
  Affects the `effect/` family, 2 files in the corpus. Fixable by including emitter
  positions in the bounds.
- **`alphaToCoverage` is not conclusively measured** — SwiftShader gives no real MSAA
  (§4). The strategy remains selectable and should be re-measured on a GPU before being
  dismissed outright.
- **Emitter-mesh ambiguity is unresolvable from the data** (§6.2). 48% of rendered
  systems in the sweep may be emitting from the wrong surface. They are counted.
- **Particle density is inferred, not read** (§6.1).
- **Unrendered-but-loaded texture count.** `textures.referenced` counts materials whose
  `textureFound` is false but whose `sourceTextureName` is empty in neither bucket; the
  95.97% figure is therefore a slight under-count, not a loss.
- **`three-obj-mtl-loader`** is now unreferenced in `package.json`.

---

## 9. Models to check visually, and what to expect

These are the cases where a regression would show. Load each in the harness or the app.

| Model | What it exercises | Expected |
|---|---|---|
| `monster/M009` | skeleton, Phase 4 dismemberment case | crab, textured, intact, feet on the grid |
| `monster/M069` | complex skeleton, wings, Phase 4 case | winged creature, wings coherent, **not dismembered** at any clip time |
| `monster/M389` | 45 clips | all clips selectable, all move, no bone stuck when switching clips |
| `monster/M491` | 30 meshes, 9→17 materials, transparency, 4 `emitterMeshes`, 5 systems | winged figure with halo; 5 magenta crystal systems **orbiting the body**, not offset |
| `ride/R814` | fur (§4), 6 systems, 86 clips | soft fur silhouette with **no hard fringe** on ruff and chest, **no grey plate** on the chest; effects **on the head**, not beside it |
| `item/WA85` | embedded animation on a static model, node hierarchy | slow but real motion over 1.5 s; 14 nodes, 2 tracks bound |
| `chair/C038` | 109 meshes, 98 materials, 34 systems | all 34 systems render; no black/invisible particles |
| `effect/S13103` | particle-only model | a coloured burst renders; framing is off (§8) |
| `npc/N720` | multi-material static building | pavilion, correct textures, upright, sitting on the grid |
| `monster/M017` | known cosmetic debt | **still upside down** — accepted, do not "fix" |
| `monster/M389` | known cosmetic debt | **still raised** — accepted |

The last two are listed to prevent someone "fixing" documented, accepted debt.

---

## 10. Technical debt deliberately left alone

- `ReconstructBindPoseFromSkin` stays removed (PHASE4 §17). Nothing in the loader
  reconstructs a bind pose; `bindMatrixLocal` and the per-mesh `skinBindings` are used
  exactly as exported.
- `M017` upside down, `M389` raised: untouched.
- The 28 malformed `.kf` files: untouched, no effect on the loader.
- Phase 3's hidden-geometry filter: untouched. `emitterMeshes` is consumed by name
  lookup only and never rendered (§3.3).

---

## 11. Open points

1. **§6.2, the emitter-mesh ambiguity, is the one worth acting on.** Two thirds of mesh
   emitters, unresolvable by the consumer, cheap to fix at export.
2. **§6.1, the birth rate**, is smaller but in the same place and could go in the same
   change.
3. `alphaToCoverage` deserves one measurement on real hardware (§8).
4. Camera framing should include particle systems (§8).
5. The harness currently re-bundles via a manual `esbuild` call; wiring it into an npm
   script would make it harder to run against a stale bundle.
