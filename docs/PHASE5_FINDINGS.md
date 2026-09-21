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
