# Phase 5 findings — particle systems

**Goal of Phase 5:** extract particle-system *parameters* (emitters and their
authored variation, the modifiers that actually vary per effect) into the
`.gfmodel`, so a JS particle library (three.quarks or similar) can reproduce
them in the Angular viewer in a later phase. No simulation, no rendering —
this phase only measures, extracts and serialises.

**Result: 1272/2822 convertible files (45%) carry at least one particle
system, 6958 systems total, and every one resolves its attach node, its
emitter's shape-origin node, and its material — 100% on all three across the
whole corpus.** Only two emitter types exist in the corpus (`NiPSysBoxEmitter`,
`NiPSysMeshEmitter`), and a discovery made before any extractor code was
written reshaped the whole approach: **niflib generates no public getters at
all for particle block fields** (see §1). 0 regressions on mesh/material/
skeleton/animation data across all 2822 files.

---

## 1. The blocker found before writing any extractor: no getters

Every other phase's extractor reads a niflib block through generated
`Get*()`/`Set*()` accessors. Reading `NiPSysEmitter.h`/`NiPSysBoxEmitter.h`/
`NiPSysMeshEmitter.h`/every `NiPSysXModifier.h` shows only `GetType()` —
every scalar field (`speed`, `declination`, box `width`/`height`/`depth`,
gravity `strength`, rotation speed, grow/fade times…) is `protected` with no
accessor at all. This is a niflib generator gap specific to these classes
(reverse-engineered later than the ones with real getters, going by their
header comments — "Unknown", "A particle emitter?", "for ???"), not a project
bug: `NiGeometryData::GetVertexCount()`, `NiColorData::GetKeys()`, and
`NiPSysModifier::GetRefs()`/`GetPtrs()` (used for the modifier list and the
`Ptr<NiObject>` back-references, see §5) all work normally, because those
sit on classes niflib's generator *did* finish.

What niflib **did** generate for every one of these classes is `asString()`,
which prints every field as `"  Label:  value\n"` — the same text NifSkope's
own detail panel shows, since NifSkope is built on niflib. Three approaches
were possible: patch the submodule to add getters (rejected — Phase 1 decided
to keep niflib pristine, and a future niflib update would silently lose the
patch); a subclass-friend trick to read the protected fields directly
(rejected — relies on exact memory layout, more fragile and more surprising
to a future reader than the alternative); or parse `asString()`'s generated
text. Given to the user as an explicit choice mid-session, with a
recommendation for simplicity and reliability: **parse `asString()`**. It
needs zero niflib changes, the label strings are generated code (not
hand-maintained, so they only drift if niflib itself changes), and it
doubles as the exact text a NifSkope-based cross-check would compare against
(see §8).

`ParticleExtractor.cpp`'s `AsStringFields()` runs `asString(false)` once per
object and returns a `label -> value` map; `ExtractFloat`/`ExtractEnum`/
`ExtractBool`/`ExtractColor4`/`ExtractVec3` look up a field by its exact
printed label and parse it, appending a warning (never failing the file) if
a label is missing or does not parse — the seam that would surface a future
niflib version silently changing this text.

---

## 2. Corpus inventory (Etape 1), measured before writing the extractor

```
Files with >=1 NiParticleSystem : 1273 / 2825   (measured pre-export, incl. the
                                                  3 already-known permanent failures)
  of which skinned (have SkeletonData)  : 630
  of which skeleton-less                : 643
Total NiParticleSystem blocks    : 6966

By entity type (files with particles / systems):
  chair:    80 files,   675 systems
  char:      1 file,      4 systems
  effect:    2 files,     4 systems
  item:    713 files,  2335 systems
  monster:  52 files,   230 systems
  npc:      72 files,   236 systems
  ride:    353 files,  3482 systems
```

`item/` and `ride/` dominate — weapon/equipment glows and mount effects, not
`effect/` (only 2 files) as the brief's own framing guessed. `ride/` alone
carries exactly half the corpus's particle systems.

**Emitter types actually used — measured, not assumed:**

```
861  x NiPSysBoxEmitter
6105 x NiPSysMeshEmitter
```

Every `NiParticleSystem` has **exactly one** emitter (0 with none, 0 with
more than one, measured across all 6966). `NiPSysSphereEmitter` and
`NiPSysCylinderEmitter` — both real niflib classes — occur **zero** times in
this corpus. Only Box and Mesh are implemented; a third type would be
skipped with a warning (`ParticleStats::emittersUnsupported`, 0 on this
corpus) rather than guessed at.

**Modifiers actually attached:**

```
6966 x NiPSysAgeDeathModifier      (100% -- universal, see below)
6966 x NiPSysBoundUpdateModifier   (100% -- universal)
6966 x NiPSysPositionModifier      (100% -- universal)
6966 x NiPSysSpawnModifier         (100% -- universal)
6065 x NiPSysColorModifier         (87%)
5435 x NiPSysRotationModifier      (78%)
4810 x NiPSysGrowFadeModifier      (69%)
1315 x NiPSysGravityModifier       (19%)
  17 x NiPSysColliderManager       (0.2%, NOT extracted -- see below)
   1 x NiPSysMeshUpdateModifier    (0.01%, NOT extracted -- see below)
```

AgeDeath/BoundUpdate/Position/Spawn are attached to **every single system**
— engine bookkeeping the pipeline always adds, not authored per-effect
variation (`NiPSysPositionModifier`/`NiPSysBoundUpdateModifier` have no
fields to extract at all beyond `Name`/`Order`/`Target`/`Active`; Spawn's own
fields were checked and are template defaults — `Percentage Spawned: 1`,
`Min/Max Num to Spawn: 1` — on every sampled instance). These four are kept
in the output as type-only entries (§4) so a consumer sees the full authored
stack, but this phase does not extract per-effect fields for them since
there is no per-effect variation to extract.

`NiPSysColliderManager` (17 occurrences) and `NiPSysMeshUpdateModifier` (1
occurrence) are genuinely rare and were **not implemented** — recorded as
"encountered but not extracted" rather than silently dropped: both are kept
as type-only entries and each occurrence produces a warning
(`ParticleStats::modifiersUnsupported`, 18 on the full corpus — exactly
17+1), so a future corpus update making either common would be visible
immediately rather than silently mis-extracted.

**Attach-node animation — measured against the real question, not the
narrower one first asked.** Checking only "does a live embedded controller
target the particle system's own node" found 0 matches corpus-wide — but
this misses the real case, where a system rides an *ancestor* bone that a
companion `.kf` animates:

```
Files with particles that have a companion .kf         : 601 / 1273
Systems parented under a skinned skeleton bone (any ancestor) : 525 / 6966
Systems parented under a node the companion .kf actually
  names as a controller target (genuinely rides an animated bone) : 2098 / 6966 (30%)
```

**30% of particle systems in the corpus are attached, directly or via an
ancestor, to a node something actually animates.** This ruled out a
fixed-world-position design outright: the attach node must resolve through
the same name-based hierarchy Phase 4 built for animation targets, not a
baked transform.

**Skeleton-less files need a node hierarchy independent of animation.**
Measured: 643 of 1273 particle-bearing files (50.5%) have no skinned mesh at
all. Before this phase, `SceneData::nodes` was only built and kept when a
skeleton-less file's *animation* needed it (Phase 4's WA85 fix) — a static
emitter on a skeleton-less file would have had nothing to resolve against.
This is a real architectural change this phase makes to `MeshExtractor.cpp`
(§4), not just new extraction code.

Measurement tools: `tools/diag/measure_particles.cpp` (corpus inventory,
all numbers above) and `tools/diag/dump_particle_fields.cpp` (raw
`asString()` dump for one file, used to validate the parsing approach and
read real field values before writing `ParticleExtractor`). Neither is wired
into CMake — built ad hoc with the same `cl.exe` invocation prior phases'
diagnostics used (see PHASE4_FINDINGS §13.7).

---

## 3. Format

`ParticleSystemData` (`SceneModel.hpp`) holds one entry per `NiParticleSystem`:
name, attach node (index + which array, bone or plain node), the system's own
local transform, `maxParticles`, a material index (via the same
`MaterialExtractor` a mesh uses — a particle system carries an ordinary
`NiTexturingProperty`/`NiMaterialProperty`/`NiAlphaProperty` list, confirmed
by dumping `chair/C038.nif`'s block tree), one `ParticleEmitterData`, and a
`vector<ParticleModifierData>`.

**Values stay in the NIF's own units and enums** — radians, NIF's raw
`ForceType`/`VelocityType`/`EmitFrom` integers, not translated to a specific
JS particle library's convention — the same principle already applied to
Z-up axes and `NiAlphaProperty` blend modes in Phases 2–4. Translation is the
Angular loader's job.

`maxParticles` is the one field with a real typed getter
(`NiGeometryData::GetVertexCount()`, inherited because `NiPSysData` extends
`NiGeometryData` — the same base class ordinary mesh data blocks use) rather
than an `asString()`-parsed one; recorded in `ParticleSystemData`'s own
comment so a future reader does not assume every field here needed the
text-parsing workaround.

**`Ptr<NiObject>`-valued fields** (`NiPSysVolumeEmitter::emitterObject`,
`NiPSysGravityModifier::gravityObject`) print as a raw pointer address in
`asString()` — useless once parsed. These are resolved through `GetPtrs()`
instead, exactly the mechanism Phase 3/4 already use for skin/skeleton
back-references (confirmed by reading `NiPSysVolumeEmitter.cpp`/
`NiPSysGravityModifier.cpp`: both classes' generated `GetPtrs()` already
include this field).

`kGfModelFormatVersion` bumped 2 → 3. Particle data goes entirely into the
`.gfmodel` JSON, never the `.gfbin`: even the largest field (a color-over-life
curve) is at most a handful of keyframes, nowhere near the size that justifies
binary storage the way mesh/animation buffers do. Confirmed directly: every
`.gfbin` byte-for-byte identical before/after this phase (§7).

### 3.1 Format 4 (Phase 7 correctif): two additions to the emitter

Both of these close defects §6 of `PHASE7_FINDINGS.md` reported. Both are
additive — no existing field moves or changes meaning — and both are on
`ParticleEmitterData`.

**`meshEmitterMeshes`: the emission surface, by index.** `meshEmitterMeshNames`
identifies a surface by *name*, and a name does not identify a mesh: 4145 of the
corpus's 6111 mesh-emitter references (67.8%) name a shape another mesh shares
its name with, `chair/C004` having eight meshes all called `Editable Poly`. A
consumer resolving by name emitted from an arbitrary one of them.

The .nif itself is never ambiguous — `NiPSysMeshEmitter` references its surfaces
by block link — so `MeshExtractor`'s walk now records where each exported shape
landed (`std::map<NiAVObject*, MeshEmitterRef>`) and `ParticleExtractor`
resolves those links to `{index, array}`, `array` naming `meshes` or
`emitterMeshes`. **All 6111 references resolve exactly, none left unresolved**;
the names stay, parallel and unchanged, as diagnostics.

**`birthRate` / `birthRateKeys`: the authored emission density.** §6.1 named the
gap correctly but placed it on the wrong block: at this NIF version
`NiPSysEmitter` has no `Birth Rate` field at all — niflib's generated
`asString()` prints `Speed` through `Life Span Variation` and nothing else.
Gamebryo drives the rate from the `NiPSysEmitterCtlr` attached to the
`NiParticleSystem`, through that controller's `NiFloatInterpolator`, which is
reachable by real typed getters (no `asString()` parsing needed here, unlike
every other particle field).

Measured before writing the extraction (`tools/diag/measure_phase7.cpp`): all
6966 systems carry such a controller; 4768 (68.4%) have an interpolator to read
— 4686 a constant, 82 a keyed timeline — and 2198 have neither interpolator nor
data block. Those last keep `birthRate = -1` rather than being given an invented
value, and a consumer falls back to its own estimate as before. Range on the
readable ones: 0 to 4500, mean 68.3 particles/second.

**Non-regression.** Full corpus re-exported and compared against the format-3
baseline: **all 2822 `.gfbin` byte-for-byte identical**, all 2822 `.gfmodel`
identical line for line outside `formatVersion`, `generator` and the three new
fields. 1272 files (exactly the particle-bearing count) gained them.

---

## 4. The attach-node architecture change

`MeshExtractor.cpp`'s node-hierarchy build (`BuildNodeHierarchy`, from Phase
4) already runs unconditionally whenever a file has no skeleton — only the
*decision to keep it* (`scene.nodes = std::move(nodes)`) was gated on
"did an animation track need it". This phase widens that gate: **`scene.nodes`
is now kept when EITHER an animation track resolved against it OR a particle
system's attach/emitter/gravity object did.** The build itself was already
free (Phase 4 built it speculatively every time), so this costs nothing
extra on a file that turns out not to need it, and the animated-mesh
reattachment pass downstream is unaffected (it only fires when
`scene.animations` actually grew, which particle resolution alone does not
change).

For a **skinned** file, no architecture change was needed: `SkeletonExtractor`'s
`CollectBones` already walks every `NiNode` in the skeleton root's subtree,
not just weighted bones (confirmed by reading `SkeletonExtractor.cpp` — the
walk only skips non-`NiNode` leaves, which a particle system already is,
being a `NiGeometry` leaf like a mesh). So a particle system's `NiNode`
ancestors were already present in `SkeletonData::bones` before this phase;
only the resolver (`ParticleExtractor::ResolveObjectIndex`, walking up an
object's own ancestor chain by name) is new.

`ParticleExtractor::Extract` runs after `AnimationExtractor` in
`MeshExtractor.cpp`'s `ExtractScene`, using the same `skeleton`/`nodesPtr`
animation just resolved against — one walk, shared resolution mechanism,
consistent with how Phase 4 itself reused Phase 3's skeleton.

---

## 5. Corpus-wide extraction results

Full corpus re-exported with the change; numbers below are from that run
(`gfnif-export export input -o ...`, no flags):

```
Files with particles: 1272
Particle systems    : 6958
Attach node resolved: 6958/6958   (100.00%)
Material resolved   : 6958/6958 (6915 with texture found)
Emitters            : 861 Box, 6097 Mesh, 0 unsupported
Emitter object resolved: 6958/6958   (100.00%)
Modifiers           : 1310 Gravity (1310 with object resolved), 5429 Rotation,
                       4808 GrowFade, 6057 Color (0 missing data), 18 unrecognised
```

**1273/6966 (inventory) vs. 1272/6958 (export) — the gap is exactly one file,
fully explained.** `npc/model/N600.nif` has 8 particle systems but was
already a documented, permanent export failure since Phase 3 ("no visible
geometry — 4 shape(s) present but all marked hidden in the source file"):
`ExtractScene` returns failure before any particle extraction runs whenever
`scene.meshes` ends up empty, regardless of what else the file contains. Not
a new gap this phase introduces — confirmed by diffing the inventory tool's
per-file list against the export's file-by-file `particleSystems` count and
finding this single, already-known file as the only discrepancy.

**100% resolution on every one of the three lookups this phase does**
(attach node, emitter object, material) is a stronger result than any prior
phase's headline number (Phase 4's animation track resolution topped out at
100% only after two follow-up fixes). This is not a coincidence: unlike a
`.kf` track (which can name a bone from a *different* skeleton variant, per
PHASE4_FINDINGS §6's `BoneNN`/`Bip01` orphans), a particle system's own
ancestor chain is always *inside its own file* — there is no cross-file name
mismatch possible the way a reused animation clip creates one.

**28% of systems have a distinct emitter-shape-origin node from their own
parent** (1953/6958, measured on the exported corpus): the `<name>-Emitter`
sibling `NiNode` pattern seen in `chair/C038.nif`'s block dump is real and
common, confirming `ParticleEmitterData::emitterObjectNodeIndex` is not
redundant with `ParticleSystemData::attachNodeIndex`.

**Color-over-life keys are normalised 0..1 across the whole corpus, exactly
as assumed**: 18497 keys checked, min time 0, max time 1, 0 outside that
range.

500-file structural validation (random sample, same style of check as prior
phases' post-export validators): attach-node/emitter-object/gravity-object/
material indices all in range against their target array's actual size, 0
issues found across 1239 sampled particle systems.

---

## 6. Non-regression

Full corpus compared file-for-file against the pre-Phase-5 export
(`gfnif-export` built from the last Phase 4 commit, same 2822-file run):

```
Meshes/skeletons/animations : byte-identical on all 2821 comparable files
                               (2822nd file has a pre-existing, unrelated
                               non-UTF8 name byte in monster/M102.nif that
                               predates this phase -- confirmed present in
                               the Phase 4 baseline export too)
Materials                   : strictly append-only on every file (base
                               materials list is an exact prefix of the new
                               one on every file checked) -- mesh
                               materialIndex values are therefore unaffected
.gfbin binary buffers        : byte-identical on all 2822 files
EXPORT SUMMARY               : 2822/2825 (unchanged from Phase 4)
```

The materials-array growth is expected, not a defect: `MaterialExtractor`
is now also called for each particle system's own property list (the same
class Phase 2 already uses for meshes), and a particle system routinely
carries a distinct texture/blend combination from any mesh in the same file,
so new entries are appended. No existing material's index moves.

---

## 7. Viewer: particle markers

`tools/viewer/index.html` gained a `particle markers` toggle (on by default)
and `buildParticleMarkers()`: one small sphere + a `CSS2DObject` name label
per exported `ParticleSystemData`, parented directly under whichever bone or
scene node its `attachNodeIndex` resolved to. No new animation code is
needed for a marker to follow an animated bone — it inherits THREE's own
matrix propagation exactly the way `buildAnimationClips`'s bone/node targets
already do, so this is a genuine end-to-end exercise of the attach-node
resolution, not a separate code path that could disagree with it.

A system whose `attachNodeIndex` did not resolve (none in the current
corpus, §5) is skipped with an on-screen warning rather than silently
omitted, matching this project's "warn, don't hide" discipline elsewhere in
the viewer.

**No headless browser is available in this environment** (same limitation
PHASE4_FINDINGS §11/§14 recorded), so the marker rendering and clip-follow
behaviour below are unverified from this side — see §9 for what to check.

---

## 8. NifSkope cross-check: what to verify

I cannot run NifSkope myself. A small, targeted sample to compare against
its Block Details panel:

1. **`chair/model/C038.nif`, block `PA_star` (a `NiParticleSystem`)** — open
   its `NiPSysMeshEmitter` child block in NifSkope and compare `Speed`,
   `Declination Variation`, `Planar Angle Variation`, `Initial Radius`,
   `Life Span`, `Emission Type` against the exported JSON's
   `particleSystems[].emitter` for the same-named system in
   `chair/model/C038.gfmodel`. Expected: `speed: 0.9`, `declinationVariation:
   ≈0.0873` (5°), `planarAngleVariation: ≈0.0873`, `initialRadius: 0.3`,
   `lifeSpan: 0.6`, `meshEmissionType: 3` (EMIT_FROM_FACE_SURFACE).
2. **Same file, `NiPSysGravityModifier` under `PA_star05`** — compare
   `Strength`, `Force Type`, `Gravity Axis` against the exported
   `gravityStrength`/`gravityForceType`/`gravityAxis`. Expected:
   `gravityStrength: 0.9`, `gravityForceType: 0` (Planar), `gravityAxis:
   [0,0,1]`.
3. **Any file with a `NiPSysBoxEmitter`** (e.g. `chair/model/C013.nif`) —
   compare `Width`/`Height`/`Depth` against `boxWidth`/`boxHeight`/`boxDepth`
   in the export (this sample happens to have all three at 0 — a genuine
   authored value, not a parsing failure, since `Speed`/`Initial Radius`
   etc. on the same block are non-zero and match).
4. **A `NiPSysColorModifier`'s `NiColorData`** on any system — NifSkope shows
   this as a curve/key list in its own data block; compare key count and the
   first/last key's RGBA against `colorKeys[0]`/`colorKeys[last]` in the
   export.

If any of these disagree with NifSkope, the most likely cause is the
`asString()` label text not matching what this session read from
`niflib`'s generated `.cpp` sources exactly (§1) — worth re-checking the
label string first before suspecting the underlying value.

---

## 9. What to check visually in the viewer

1. **`chair/model/C038.gfmodel`** — load with `particle markers` on. Expect
   34 small pink spheres with name labels (`PA_star`, `PA_lit`,
   `SuperSpray_last`, …) scattered across the model at plausible emission
   points (weapon glows, effect planes). Toggle the checkbox off/on to
   confirm markers appear/disappear without needing a reload.
2. **Any `ride/` model with a companion `.kf`** (e.g. `ride/model/R834`) —
   play a clip and confirm any marker parented under an animated bone
   visibly moves with it, exactly like the skeleton overlay already does.
   This is the load-bearing check for §2's "30% of systems ride an animated
   bone" measurement — if a marker does NOT follow its bone, the resolution
   is structurally correct (§5's numeric validation passed) but something
   in the viewer's parenting is wrong.
3. **A skeleton-less file with particles** (e.g. `effect/model/S13103` or
   `item/model/G00008`) — confirm markers still appear despite the model
   having no skeleton toggle available; this exercises the `scene.nodes`
   gating change (§4) specifically.
4. **Switch clips** on a file with both animation and particles and confirm
   no marker is left stuck mid-air after a clip switch — markers are plain
   children of a bone/node object, so they should inherit the existing
   bind-pose snapshot/restore fix from PHASE4_FINDINGS §12.1 automatically,
   but this has not been visually confirmed.

---

## 10. Robustness

Every partial-failure path is warn-and-skip, never fail-the-file:

* An emitter type other than `NiPSysBoxEmitter`/`NiPSysMeshEmitter` is
  skipped with a warning; the system's other modifiers are still not
  extracted either (a system needs its emitter to mean anything), and the
  whole particle system is dropped, but the rest of the file's meshes/
  skeleton/animation/other particle systems are unaffected. 0 occurrences
  on this corpus (§5).
* A modifier type outside the six handled ones (Gravity/Rotation/GrowFade/
  Color, plus AgeDeath/BoundUpdate/Position/Spawn as type-only) is kept as a
  type-only entry with a warning rather than dropped silently — 18
  occurrences on this corpus, all `NiPSysColliderManager`/
  `NiPSysMeshUpdateModifier` (§2).
* An `asString()` field that does not parse (a future niflib version
  changing a label, or an unexpected enum text) warns and falls back to a
  documented default rather than crashing or silently writing garbage.
* An attach node, emitter object, or gravity object that cannot be resolved
  is left at `-1` (never guessed), with a warning — 0 occurrences on this
  corpus (§5), but the path is exercised by the format itself (every
  `*NodeIndex` field is documented as "-1 when unresolved").

---

## 11. What I need you to verify visually

Per §9: load `chair/model/C038.gfmodel` and `ride/model/R834.gfmodel` (or
any other `ride/` file with a companion `.kf`) in `tools/viewer/index.html`,
confirm particle markers appear at plausible positions, and confirm a
marker parented under an animated bone moves with it during clip playback.
If you have NifSkope available, the four comparisons in §8 would give an
independent confirmation of the extracted values beyond this session's own
structural checks.

---

## 12. Correctif — emitter meshes filtered out by the Phase 3 visibility gate

**Confirmed on the full corpus.** A `NiPSysMeshEmitter` emits from the
surface of one or more meshes (`meshEmitterMeshNames`), referenced by name.
Those meshes are almost always marked not-visible in the source file (they
are an emission volume, not something meant to render), so Phase 3's
unconditional hidden-geometry filter (`MeshExtractor.cpp`, `GetVisibility()
== false`, added for M491's opaque panel problem, §13 of PHASE3_FINDINGS)
was silently dropping them. A particle system referencing that name then
resolved to nothing, and a viewer/loader fell back to the emitter's own
attach-node origin — the "markers lined up at bone origins, off the model"
symptom reported for a `ride/` mount file.

**Measured, full corpus (2825 .nif files scanned, `tools/diag/
measure_emitter_meshes.cpp`):** 5589 `meshEmitterMeshNames` references
across 1113 files. **1865 of them (33.4%) point at a shape the source file
marks hidden.** Name resolution itself is not the problem: every single
reference resolves to a real `NiTriBasedGeom` in its own file (0
unresolved) — niflib's own ref, not a fuzzy name search. Per-folder: `ride`
32.3% hidden, `item` 35.3%, `chair` 22.8%, `npc` 72.6%, `char`/`effect`
100% (small samples, 4 and 1 refs respectively).

One file (`monster/M156.nif`) crashed the diagnostic — traced to niflib's
known crash-not-throw behavior on an unsupported block type
(`NiPhysXScene`, already handled everywhere else in this codebase via
`FindUnsupportedBlockType` before `ReadNifList` is ever called,
`MeshExtractor.cpp` §"niflib crashes rather than throwing cleanly"). The
diagnostic tool was missing that guard; fixed by reusing the same header
pre-check. Not a defect in the exporter itself — M156 was already correctly
reported as a failed file before this session.

**Fix — Option A, a separate `emitterMeshes` list (SceneModel.hpp,
GfxFormatWriter.cpp):**

* `ParticleExtractor::CollectMeshEmitterNames` (new, static) scans every
  `NiPSysMeshEmitter` in the file's block list up front — before
  `WalkNode`'s mesh pass runs — and returns the set of geometry names any
  emitter references.
* `MeshExtractor.cpp`'s hidden-geometry filter takes one exception: a
  hidden shape whose name is in that set is kept, but routed to
  `SceneData::emitterMeshes` instead of `SceneData::meshes` — never
  `hiddenGeometriesDropped`. Every other hidden shape is dropped exactly as
  before (§13 of PHASE3_FINDINGS, unchanged).
* `GfxFormatWriter.cpp` writes `emitterMeshes` as a second top-level array,
  same per-mesh shape as `meshes` (attributes/indices in the same `.gfbin`,
  same accessor format), so a consumer resolves a
  `ParticleSystemData.emitter.meshEmitterMeshNames` entry by name against
  `emitterMeshes`, not `meshes` — no existing render path can show these by
  accident.

**Option B (flag on the existing mesh list) was rejected**: it would put
the burden of not rendering these shapes on every consumer, and the reason
Phase 3's filter exists at all is that a mesh not meant to render had
leaked into the render path once already (M491). A separate array makes
that structurally impossible instead of relying on discipline.

**Resolution rate after the fix, full corpus: 2023/6111 distinct
emitter-mesh names resolved (0 unresolved), reported per-file in the
"particles" summary as `Mesh emitter surfaces: N/M resolved`.** (6111, not
5589 — this counts distinct names per file, so a name several emitters in
the same file reference is counted once; the un-deduplicated reference
count is the 5589 above.) The remaining ~4000 names are the ones already
visible before the fix (no exception needed) plus a genuine few dropped by
another filter (helper-gizmo name/texture heuristic, no data block, no
triangles) — reported as a warning naming the file and the mesh, never a
failure. 0 such warnings were seen on this run of the full corpus: every
`meshEmitterMeshNames` reference that resolves to a real geometry block
made it into the export, either via `meshes` or `emitterMeshes`.

**Non-regression, M491 (the original visibility-filter case, PHASE3_FINDINGS
§12/§821):** documented baseline is 34 total shapes, 4 hidden, 30 exported.
This session's export after the fix: still exactly 30 meshes in `meshes`.
One of M491's five particle systems references a hidden shape by name; it
is now the sole entry in `emitterMeshes`, still absent from `meshes` — the
opaque-panel fix is untouched.

**Secondary items from the correctif brief, measured, no fix needed:**

* **`colorKeys` black RGB**: 16 757/18 497 color keys (90.6%) corpus-wide
  have `[0, 0, 0, alpha]` — not universal, so this is authored variation
  (an opacity-only profile is common but not the only pattern), not a
  parsing bug. Left as-is per this phase's "values stay in the NIF's own
  units, no interpretation" principle (§3); worth an eye in Phase 7 if a
  system with a black `colorKeys` entry renders invisible under additive
  blending.
* **Particle texture resolution**: 6915/6958 particle systems (99.4%) have
  a `materialIndex` resolving to a material with `textureFound: true` —
  matches the exporter's own corpus summary. The 43 unresolved match the
  pre-existing unresolved-texture list (missing `.dds`→`.png` on disk),
  not a particle-specific gap.
* **`emitterObjectNodeIndex == attachNodeIndex`**: true for 5006/6958
  systems (71.9%), **not universal** — the brief's suspicion that one field
  might be redundant does not hold on the full corpus; the remaining 28.1%
  genuinely have a distinct emitter-shape-origin node (a sibling
  "`<name>-Emitter`" NiNode, per §"Emitter Object" in ParticleExtractor.cpp)
  from their attach parent. Both fields stay.

**What to verify visually:** reload the `ride/` mount `.gfmodel` that
originally showed the line-of-markers symptom in `tools/viewer/index.html`.
The markers themselves were already correct (bone origins are the right
place for an attach point with no emission surface); what changes is that
`emitterMeshes` now carries real geometry for the systems that need it, so
a Phase-7 particle renderer has something to emit from instead of nothing.
No viewer-visible change is expected in *this* phase's marker positions —
confirming that would mean this correctif regressed something.

Measurement tool: `tools/diag/measure_emitter_meshes.cpp` (not wired into
CMake, built ad hoc, same `cl.exe` invocation as §2's tools — see
PHASE4_FINDINGS §13.7).

---

## 13. Correctif — viewer can now visualise `emitterMeshes`, closing the "wait for Phase 7" gap

**Why:** §12's fix routed hidden emission-surface geometry into
`SceneData::emitterMeshes`, but nothing in the viewer could show it — the
particle markers (§7) render bone origins, which were already correct
before §12's fix and stay identical regardless of whether it worked. There
was no way to see, before Phase 7's real particle engine exists, whether an
emitter mesh actually sits where the game shows an effect (eyes, harness,
paws) rather than off in space. §12 itself flagged this as exactly the kind
of deferred verification that cost real time in Phase 4.

**Added to `tools/viewer/index.html`:** a dedicated "emitter meshes" toggle
(off by default, so it never gets in the way of ordinary model
inspection), separate from the existing "particle markers" toggle.
Enabling it renders every `model.emitterMeshes[]` entry as a
**wireframe, semi-transparent, distinctly-tinted** (`0x33d0ff`, cyan)
`MeshBasicMaterial`, plus a name label, using `CSS2DObject` the same way
particle-marker labels already work.

**Implementation reuses the real mesh-building path, not a separate one.**
The per-mesh construction inside `buildScene()` (geometry attributes,
skin binding, node/skeleton parenting) was factored into `buildOneMesh(m,
materialFor)`, called once per `model.meshes` entry (unchanged behaviour,
`materialFor = null` keeps the existing NiAlphaProperty-derived
`MeshStandardMaterial`) and once per `model.emitterMeshes` entry when the
toggle is on (`materialFor` swapped for the tinted wireframe look). Because
`MeshData` is the exact same struct/JSON shape for `meshes` and
`emitterMeshes` (SceneModel.hpp, confirmed identical accessor layout in
GfxFormatWriter.cpp's shared `writeMeshJson`), an emitter mesh gets
**identical parenting/skinning/nodeIndex handling as a real rendered
mesh** — same skeleton bone if skinned, same animated scene-node
reattachment if not — with no new transform code. This is deliberate: it
makes the toggle a genuine end-to-end check (does the emission surface
sit where the game shows the effect, and does it follow the bone the way
real geometry does), not a separate code path that could silently disagree
with how the exporter actually parents this geometry.

**What to verify visually (per the correctif brief):** re-exporting
`ride/R814.nif` and `chair/model/C038.nif` with the current build shows
both now have **0** `emitterMeshes` entries — their resolved mesh-emitter
surfaces (`Editable Poly`, `Editable Poly@#2` on R814; 6 on C038) turned
out to already be visible geometry, so they resolved into the ordinary
`meshes` array and never needed the hidden-shape exception. Neither file is
a working example for this specific toggle, despite being the named
validation targets in the correctif brief.

**`monster/model/M491.nif` (this section's own §8-§11 reference file) is a
confirmed working example**, re-exported this session:
`emitterMeshes` has 4 entries, all named `Editable Poly`, all
`isSkinned: true` — exercising the skinned/bone-parented path specifically,
not just the simpler unskinned-node case. Load `M491.gfmodel`, enable
"emitter meshes", and confirm the cyan wireframe surfaces appear on the
model (not detached in space) and, since this file also has a companion
animation, that they move with their bone during clip playback exactly
like the particle markers already do. A `ride/` mount with a non-empty
`emitterMeshes` list would additionally validate the eyes/harness/paws
framing from the original brief, but was not found among the files
re-exported this session — worth checking a wider sample in a future
session if that specific visual claim needs confirming.

---

## 14. Phase 5 closure

**Written for a reader with none of this phase's conversation history** —
everything needed to pick up from here should be in this section or the
ones it points to; check `git log` for what has actually landed (briefs
and even prior findings sections describe intent at the time, this section
describes the state Phase 5 was closed in).

**Status: Phase 5 is done.** The particle-system data exported into
`.gfmodel` is measured, corpus-validated, and correct — that is the bar
that closes this phase: what is *exported* must be right, verified
independently of any one consumer's rendering choices (the same standard
this project applied when it walked back `ReconstructBindPoseFromSkin` in
Phase 4 — see PHASE4_FINDINGS §14.2 — after realising a harness that
embeds a rendering/consumer opinion cannot settle an export-correctness
question). Two items remain genuinely open (§14.4 below) but both belong
to Phase 7 (the Angular/Three.js loader), not to this phase's own scope.

### 14.1 What this phase does and does not do

**Extracts and serialises particle-system *parameters* into `.gfmodel`.
Renders nothing.** No particle is simulated or drawn anywhere in this
project's own code. `tools/viewer/index.html`'s "particle markers" (§7)
and "emitter meshes" (§13) toggles are diagnostic-only: a marker sphere at
an attach point, and a wireframe of an emission surface, neither of which
is what a particle effect actually looks like. The real rendering — an
actual particle simulation driven by these exported parameters, using a
JS particle library such as three.quarks — is Phase 7's job, in the
Angular consumer app, not this exporter.

### 14.2 Emitter/modifier coverage: what is handled, what is not, and why

**Emitter types:** only `NiPSysBoxEmitter` and `NiPSysMeshEmitter` are
extracted — because those are the *only two emitter types that occur
anywhere in the corpus* (measured exhaustively, §2). `NiPSysSphereEmitter`/
`NiPSysCylinderEmitter` are real niflib classes but occur zero times; an
emitter type outside this set is skipped with a warning
(`ParticleStats::emittersUnsupported`), never guessed at. 0 occurrences of
that warning on the full corpus (§5).

**Modifiers:** `NiPSysGravityModifier`, `NiPSysRotationModifier`,
`NiPSysGrowFadeModifier`, `NiPSysColorModifier` are extracted with full
per-effect fields, because these are the modifiers that actually vary
between effects (§2). `NiPSysAgeDeathModifier`/`BoundUpdateModifier`/
`PositionModifier`/`SpawnModifier` are kept as type-only entries (no
per-effect fields extracted) because they are attached to **every single
system** in the corpus with template-default values — engine bookkeeping,
not authored variation (§2). `NiPSysColliderManager` (17 occurrences) and
`NiPSysMeshUpdateModifier` (1 occurrence) are kept as type-only entries
with a warning on each occurrence, genuinely not implemented because they
are too rare to justify it (§2) — a future corpus update making either
common would show up immediately as a rise in
`ParticleStats::modifiersUnsupported` (18 total today), not silently
mis-extract.

**Values are kept in the NIF's own units/enums throughout** (radians, raw
`ForceType`/`VelocityType`/`EmitFrom`/blend-mode integers) — the same
"export stays faithful to the source, the consumer's loader translates"
principle already applied to Z-up axes (Phase 2) and `NiAlphaProperty`
blend state (Phase 2 §10). Phase 7's loader is responsible for translating
every one of these into whatever a specific particle library expects.

### 14.3 The `emitterMeshes` correctif: what it is and why the visibility filter must NOT be reopened generally

A `NiPSysMeshEmitter` emits from the surface of a named mesh
(`meshEmitterMeshNames`). Those meshes are almost always marked
not-visible in the source file (§12) — they are an emission volume, not
something meant to render. Phase 3 added an unconditional filter that
drops every hidden shape (`GetVisibility() == false`), originally to fix
`M491`'s opaque-panel defect (a hidden decorative gizmo leaking into the
render path). That filter, applied unconditionally, was also dropping the
33.4% of `meshEmitterMeshNames` references that happen to point at a
hidden shape — so a particle system referencing that name resolved to
nothing.

**The fix is a narrow, structural exception, not a reopening of the
filter:** `ParticleExtractor::CollectMeshEmitterNames` scans every
`NiPSysMeshEmitter` up front for the set of geometry names any emitter
actually references. Only a hidden shape whose name is in *that specific
set* is kept — routed to the new `SceneData::emitterMeshes` array, never
back into `SceneData::meshes`. Every other hidden shape is dropped exactly
as before Phase 3 intended. **Do not loosen the general hidden-geometry
filter itself** — that would reintroduce the exact M491 defect this
project already spent real effort fixing (PHASE3_FINDINGS §12).
`emitterMeshes` is a deliberately separate array specifically so no
existing render path can show this geometry by accident (§12) —
consuming it means looking a name up in `emitterMeshes` specifically, only
when resolving a `meshEmitterMeshNames` entry, never rendering the array
wholesale.

### 14.4 Open items for Phase 7

**1. The fur/alpha-test rendering question (see
[docs/PHASE2_FINDINGS.md §11](PHASE2_FINDINGS.md) for the full
diagnosis).** An alpha-test-only material (no blending) cannot reproduce a
soft cutout edge with Three.js's binary `alphaTest` — confirmed on
`ride/R814`'s fur and, on inspection, on `monster/model/M491` too (same
exact alpha-test signature, smaller/less visible region). **This affects
2197 of 2829 files (77.7% of the corpus)** — a structural rendering
decision for Phase 7, not a one-file cosmetic issue. Three directions to
evaluate, none tried yet: rendering these materials as blended instead of
using `alphaTest` literally (most consistent with this project's own
translation-in-the-loader principle), `alphaToCoverage`, or a custom
shader if neither suffices. The exported alpha data itself
(`alphaBlendEnabled`/`alphaTestEnabled`/`alphaTestFunc`/
`alphaTestThreshold`) needs no change — this is purely a render-technique
choice.

**2. `colorKeys` black RGB in 90.6% of cases (16 757/18 497 keys,
§12).** Not a bug — an opacity-only color profile (black RGB, real alpha)
is common, measured as authored variation rather than universal, so left
as-is per this phase's "no interpretation" principle. **Worth an eye in
Phase 7 specifically**: a black `colorKeys` entry rendered under additive
blending (the most common particle blend mode in this corpus, §2 of
PHASE2_FINDINGS) would be invisible — check whether the intended blend
mode is additive before assuming a black-RGB particle should render
anything visible at all; this may be intentional (an opacity-only fade
effect) rather than a defect.

**3. Texture resolution is not universal: 99.4% (6915/6958 systems).**
The 43 unresolved match the pre-existing corpus-wide unresolved-texture
list (missing `.dds`→`.png` on disk, not particle-specific) — same
`textureFound: false` fallback behavior as any other material. No action
needed beyond what the exporter already does; noted so Phase 7 does not
treat a small number of texture-less particle systems as a new bug.

**4. `emitterObjectNodeIndex` and `attachNodeIndex` are genuinely
different fields 28.1% of the time (§12)** — the brief's original
suspicion that one might be redundant does not hold on the full corpus.
**Both fields are required**: `attachNodeIndex` is where the particle
*system* sits in the scene graph (parent it here to follow an animated
bone, §5), `emitterObjectNodeIndex` is the node the emitter's own shape
(box dimensions, or the mesh-emitter's `emitterMeshes` surface) is
expressed relative to — routinely a sibling `<name>-Emitter` node, not the
system's own parent. A consumer that only reads one of these will
misplace either the particle system itself or its emission shape on more
than a quarter of the corpus's systems.

### 14.5 Where to look

- Corpus measurements and the `asString()`-parsing approach: §1–§2.
- Format (`ParticleSystemData`/`ParticleEmitterData`/`ParticleModifierData`):
  §3, and `SceneModel.hpp` directly.
- The attach-node/node-hierarchy architecture change: §4.
- Extraction results and validation: §5–§6, §10.
- Viewer diagnostics (particle markers, emitter meshes): §7, §13.
- The `emitterMeshes` correctif in full: §12.
- The open fur/alpha-test question: [PHASE2_FINDINGS.md §11](PHASE2_FINDINGS.md).

**No headless browser available in this environment** (same recurring
limitation, PHASE4_FINDINGS §11/§14, PHASE5_FINDINGS §7) — this addition
is unverified from this side; the check above is what to run manually.
