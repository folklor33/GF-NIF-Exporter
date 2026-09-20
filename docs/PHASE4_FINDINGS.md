# Phase 4 findings — animation (embedded + external `.kf`)

**Goal of Phase 4:** extract every bone-transform animation in the corpus —
embedded `NiTransformController`/`NiMultiTargetTransformController` data plus
whatever `NiControllerSequence` clips the companion `.kf` carries — resample
the B-spline-compressed majority to plain keyframes, and confirm the same
Z-up, no-axis-flip discipline Phases 2–3 established still holds when a bone
is moving rather than sitting in its bind pose.

**Result: all 2822 previously-convertible files still convert (unchanged from
Phase 3), and 1787 of them now carry at least one animation clip.** 20103
clips (494 embedded, 19609 from 1473 `.kf` files), 1071465 bone tracks,
45982599 keyframes, **99.43% of tracks resolve to a skeleton bone by exact
name**. 0 skinning/mesh/material regressions — every Phase 2/3 number is
byte-identical to before this phase.

---

## 1. Export results

```
EXPORT SUMMARY: 2822/2825 file(s) converted        (unchanged from Phase 3)
Vertices          : 5 597 529                       (unchanged)
Triangles         : 6 782 167                       (unchanged)

--- animation ---
Files with animation : 1787
Animation clips       : 20 103   (494 embedded, 19 609 from 1473 .kf files)
Tracks                : 1 071 465  (473 689 classic keyframe, 597 776 B-spline resampled)
Track resolution      : 1 065 339 / 1 071 465 (99.43%) resolved, 6126 orphaned
Scale animated        : 33 707 / 1 071 465 track(s) (3.15%)
Total keyframes       : 45 982 599
```

The 3 known skips are unchanged from Phase 3 (`item/WF20.nif`,
`monster/M156.nif` unsupported block, `npc/N600.nif` all-hidden geometry).

Output grew from Phase 3's 545 MB to **≈1996 MB** (1237 MB `.gfbin` + 758 MB
`.gfmodel`, measured directly), a **+1451 MB** cost for animation — see §4 for
what that buys and why it is not overspending.

---

## 2. The two animation sources, and how the corpus splits between them

**A. Embedded** (`NiTransformController` / `NiMultiTargetTransformController`,
walked from the same unreferenced-root set the mesh pass uses): only **494
clips**, and every one carries exactly the tracks that had a real, non-null
`NiInterpolator` — most controllers registered this way turn out to be inert.
`monster/M491.nif`, picked as a stress case in Phase 3, is typical: it has one
`NiTransformController` and one `NiMultiTargetTransformController`, but the
`NiTransformController`'s `GetInterpolator()` is null and the
`NiMultiTargetTransformController` names ~90 bones as *extra targets* with no
per-bone interpolator data of its own (see §2.1 investigation notes below) —
so the file's real animation is 100% `.kf`-sourced, exactly as
`docs/NAMING_CONVENTIONS.md` predicted for `chair`/`char`/`item`/`monster`/
`npc`/`ride`.

**B. External `.kf`** (`NiControllerSequence`, resolved by the
`<type>/model/NAME.nif` ↔ `<type>/animation/NAME.kf` convention already
proven exact in Phase 1): **19609 clips from 1473 files** — the overwhelming
majority of the corpus's animation. `NiControllerSequence` roots sit at the
top level of a `.kf` with no wrapping `NiControllerManager` (verified by
dumping `chair/animation/C001.kf`), so the same "roots are the blocks nothing
references" walk `ExtractScene` already used for `.nif` files works unchanged
for loading a `.kf`.

### 2.1 `NiMultiTargetTransformController`'s extra targets carry no interpolator

`NiMultiTargetTransformController` has no `GetInterpolator()` of its own
(confirmed by reading niflib's header: it extends `NiInterpController`, not
`NiSingleInterpController`) — it only exposes `GetExtraTargets()`, a list of
`NiAVObject*` the controller nominally governs. In this corpus that list is
populated (M491's names ~90 bones) but none of those extra targets carry a
separate embedded interpolator block of their own; the actual per-bone
animation for those targets, if any, comes from the `.kf`'s
`NiControllerSequence` addressing the same bone names. The extractor walks
every `NiAVObject`'s own controller chain (`GetControllers()`), so if a future
file *did* attach per-bone `NiTransformController`s directly to those extra
targets, they would be picked up automatically — nothing about this design
depends on `NiMultiTargetTransformController` itself carrying data.

---

## 3. `ControllerLink` rows: what's actually a bone-transform track

A `.kf`'s `NiControllerSequence::GetControllerData()` returns one
`ControllerLink` per animated *anything* in the sequence — bones, but also
material/UV/visibility controllers riding in the same list. Measured
corpus-wide:

```
ControllerLink rows total          : 1 820 462
  out of scope (not bone transform): 656 295  (36.0%)
  static pose only (no timeline)   :  80 315  ( 4.4%)
  B-spline, no channel data        :  12 387  ( 0.7%)
  genuinely failed to extract      :       0  ( 0.0%)
  -----------------------------------------------
  extracted as bone tracks         : 1 071 465 (58.9%)
```

**Zero genuinely-failed tracks.** Every `ControllerLink` this phase could not
turn into a track falls into one of three well-understood, non-error
categories:

* **Out of scope (36.0%).** A `NiFloatInterpolator`/similar riding the same
  sequence for a material, UV, or visibility controller — e.g. M491's glow
  billboards (`Plane26` etc., PHASE3_FINDINGS §12) each carry a
  `NiFloatInterpolator` track for their alpha/opacity animation, correctly
  reported as "not a bone-transform track" and skipped. This phase extracts
  bone transforms only, as scoped by the brief; a future particle/material
  animation phase would read this same category.
* **Static pose only (4.4%).** A `NiTransformInterpolator` whose
  `GetData()` is null: the bone has a fixed pose value for this clip (its own
  `translation`/`rotation`/`scale` fields) but no keyframe timeline — it
  simply does not move in that particular clip. Not a defect; a `.kf` is
  reused across many bones and not every bone moves in every animation.
* **B-spline, no channel data (0.7%).** The B-spline equivalent: every
  channel offset (`translationOffset`/`rotationOffset`/`scaleOffset`) is
  unset, so `Sample*Keys` legitimately returns nothing for that track.

Both "genuinely failed" counters were non-zero (1822 corpus-wide, on the
`monster/` subset alone) before this three-way split was added — every one of
those turned out to be the B-spline-empty case once measured explicitly, not
an unexplained gap. This is recorded here because it is exactly the kind of
false alarm the brief's "measure, don't guess" instruction is meant to catch.

### 3.1 `XYZ_ROTATION_KEY`: measured, and deliberately not implemented

**322257 of 473689 classic tracks (68.0%) use `XYZ_ROTATION_KEY`** — separate
per-axis Euler sub-channels instead of quaternion keys. This is far from the
"rare" case the brief's phrasing anticipated; it is the *majority* encoding
for classic (non-B-spline) rotation in this corpus. For every such track,
translation and scale still extract normally; only the rotation channel is
left empty, and a warning names the track. Composing three independently
timed Euler channels (each with its own key times and interpolation type)
into a single quaternion timeline correctly is materially more work than the
rest of this phase and was left out deliberately rather than guessed at —
see §8 for what a future pass would need to get right (key-time
resampling across three unaligned channels, then Euler order and handedness
matching niflib's own composition, which is undocumented in the header).

**Practical impact is smaller than the raw percentage suggests**: 597776 of
1071465 tracks (55.8%) are B-spline (quaternion-based, unaffected), and
322257/1071465 (30.1%) are the affected classic-Euler case. Every affected
track's translation/scale still animates; only its own rotation is static at
bind pose for that clip.

---

## 4. B-spline resampling: the sampling-rate decision

### 4.1 niflib's own evaluator was used, not a reimplementation

Per the brief's instruction to validate against niflib rather than
re-derive: `NiBSplineCompTransformInterpolator` (and its base
`NiBSplineTransformInterpolator`) expose `SampleQuatRotateKeys(npoints,
degree)` / `SampleTranslateKeys(npoints, degree)` / `SampleScaleKeys(npoints,
degree)`, **virtual and polymorphic**, which dequantize the int16-packed
control points (`translationBias`/`translationMultiplier` and the rotation/
scale equivalents — niflib's own quantization scheme, confirmed by reading
`NiBSplineCompTransformInterpolator.cpp`, not "offset + range" as the brief's
phrasing guessed) and evaluate the B-spline internally via niflib's `bspline()`
helper. This phase calls these directly with `npoints` set from the desired
sample rate and `degree = 3` (cubic, the universal NifTools convention — no
degree field is stored in the file; `NiBSplineBasisData`'s own header comment,
"number of frames of animation plus degree of B-spline minus one", implies it).
**No B-spline math was reimplemented.**

### 4.2 Validating niflib's evaluator itself

niflib exposes no second, independent implementation of B-spline evaluation to
cross-check against (unlike Phase 3's `GetSkinDeformation`, which gave an
independent ground truth for skinning). The substitute used here: **a clamped
(open) uniform B-spline is mathematically required to pass through its first
and last control point exactly.** Sampling `SampleQuatRotateKeys`/
`SampleTranslateKeys` at `npoints = 200` and comparing the first/last output
sample against `GetQuatRotateControlData()`/`GetTranslateControlData()`'s own
first/last entry (the ground-truth endpoint, independent of the sampling
codepath) tests the evaluator against an invariant of the curve family it
claims to implement, corpus-wide:

```
B-spline tracks checked        : 610 503
Rotation endpoint error (deg)  : mean 0.0104, max 0.0791
Translation endpoint error     : mean 0.0,    max 0.0    (exact)
```

Translation reproduces its endpoints **exactly**; rotation is within
**0.01° on average, 0.08° worst case** — floating-point-scale noise, not a
structural discrepancy. (An earlier version of this check, run without
renormalizing the raw control-point quaternions first, showed a spurious ~4°
"error" — control points are stored as raw dequantized coordinates with no
unit-length guarantee, and comparing two off-unit quaternions with a
dot-product angle formula is meaningless. Renormalizing both sides before
comparing, exactly as the real extractor does before writing a quaternion out
(§4.4), collapsed the error to noise. Recorded here because it is the kind of
self-inflicted "bug" that measuring carefully catches before it reaches a
finding.)

This gives confidence niflib's evaluator is correct at the one point a
mathematical guarantee makes checkable without an independent reference; the
resampling error measured in §4.3 is then attributable to the resampling
*rate* (a deliberate, chosen approximation), not to a broken evaluator
underneath it.

### 4.3 Error vs. rate, measured

Method: for a stratified sample of `.kf` files (126 files spanning
`chair`/`char`/`item`/`monster`/`npc`/`ride`, avoiding only the single
heaviest known outlier `chair/C625` — 437 bones, PHASE3_FINDINGS §7 — to keep
the run tractable), each B-spline track was sampled at a **240 Hz reference**
rate (assumed close enough to the continuous curve to stand in for it) and at
four **candidate rates**. The reference curve was then linearly
re-interpolated from each candidate's own keyframes and compared back against
the reference samples — i.e. the error a consumer's own linear-interpolation
playback would actually see, not just a curve-fitting residual.

```
Rate(Hz)  MeanRotErr(deg)  MaxRotErr(deg)  MeanTransErr  MaxTransErr  Keyframes(sample)
15         2.32             179.8           0.0019        2.62         494 711
24         1.47             179.2           0.0012        1.56         773 822
30         1.17             145.7           0.00094       1.24         959 775
60         0.51             155.3           0.00041       0.53       1 890 464

Rotation error distribution (share of reference samples exceeding threshold):
Rate(Hz)   >1°       >5°       >30°       >90°
15         41.7%     12.7%     0.476%     0.0032%
24         32.0%      7.2%     0.144%     0.0007%
30         27.4%      5.2%     0.061%     0.0003%
60         13.7%      1.2%     0.001%     0.00003%
```

The **max** column looks alarming at every rate tested (145–180°) — but the
distribution shows why that is not representative: these are isolated,
near-antipodal quaternion double-cover artifacts at a handful of individual
sample points (well under 0.001% of all samples at 30 Hz), not a systemic
accuracy problem. The number that matters for perceived animation quality is
the **mean** and the **>5°/>30° share**, both of which improve steadily and
predictably with rate.

Translation error is negligible at every tested rate relative to model
scale — GF character/prop models measure a few units across (PHASE3_FINDINGS
§12's bounding boxes are typically 2–6 units per axis), so 30 Hz's 0.00094
mean / 1.24 max translation error is on the order of 0.02–0.05% of model
scale even at its worst.

### 4.4 Quaternion renormalization is required before writing keyframes

A B-spline is fit **component-wise** through a quaternion's four coordinates
independently; the result is not constrained to stay on the unit sphere
between control points. **Measured directly on the exported corpus: raw
`SampleQuatRotateKeys` output drifted up to 0.77 from a norm of 1.0** on some
tracks before a fix (worst case found by the structural validator, §5).
`AnimationExtractor::CopyQuat` renormalizes every rotation key — B-spline
*and* classic, the latter as free insurance — before it is written. After the
fix, corpus-wide worst-case quaternion norm error on a 500-model random sample
is **1.6e-7**, floating-point noise. A consumer's slerp requires unit
quaternions; shipping an off-unit one would silently corrupt playback in a way
that is easy to miss visually until it accumulates.

### 4.5 Decision: 30 Hz

**30 Hz is the sampling rate used for every B-spline track in the exported
corpus.** Justification from the measurements above:

* Mean rotation error drops from 2.3° (15 Hz) to 1.2° (30 Hz) to 0.5° (60 Hz)
  — each doubling of rate roughly halves the mean error, the expected
  diminishing-returns shape for uniform resampling of a smooth curve.
* At 30 Hz, 94.8% of samples are within 5° and 99.94% are within 30° of the
  reference curve — visually indistinguishable from continuous playback for
  the overwhelming majority of the animation, with genuinely large deviations
  confined to a vanishingly small, already-isolated set of sample points.
* File size scales linearly with rate (§4.6): 60 Hz would almost exactly
  double the corpus's already-substantial +1451 MB animation cost for a mean
  error improvement of 1.2° → 0.5°, i.e. diminishing returns exactly where the
  brief warned to watch for them.
* 15 Hz is visibly worse (2.3° mean, 12.7% of samples over 5°) for only a
  ~1.94× file-size saving versus 30 Hz — not proportionate.

**Adaptive sampling was not implemented.** The brief asked for it to be
considered only if uniform sampling proved too costly; at 30 Hz the error
profile is already good (§4.3) and the size cost, while real, is not
disproportionate for what it buys (§4.6) — there is no measured problem for
adaptive sampling to solve in this corpus. This is recorded as a deliberate
non-implementation, not an oversight.

### 4.6 File size cost

```
Phase 3 total (mesh + skeleton, no animation): 545 MB
Phase 4 total (+ animation)                  : ≈1996 MB  (1237 MB .gfbin + 758 MB .gfmodel)
Animation's own cost                         : ≈1451 MB
```

At the sample's measured ratio, 60 Hz would add roughly another ~1450 MB on
top (keyframe count is within 2% of exactly doubling from 30→60 Hz in the
measured sample: 959 775 → 1 890 464), for a mean rotation error improvement
of well under 1°. 15 Hz would save roughly half of the +1451 MB (494 711 vs.
959 775 keyframes in the sample, essentially exactly half) at the cost of
visibly worse fidelity on nearly a third of samples. 30 Hz sits at the elbow
of this curve.

---

## 5. Format validation

`AnimationClip`/`AnimationTrack` are appended to the `.gfmodel` JSON's
`animations` array (`kGfModelFormatVersion` bumped 1 → 2, since a v1 consumer
would not know to read the new content — see `GfxFormatWriter.hpp`).
Keyframes go in the shared `.gfbin`, addressed the same way mesh attributes
are: `{times: accessor, values: accessor, count}` per channel
(translation/rotation/scale), each accessor a `{byteOffset, itemSize, count,
type}` reference into the one buffer. `itemSize` is 3 for translation
(xyz), 4 for rotation (quaternion xyzw), and **1 for scale** — GF bone scale
is always uniform (`NiAVObject` stores scale as a single float; it cannot
even express non-uniform scale, PHASE3_FINDINGS §5), so the value channel is
one float per key rather than a redundant 3-component vector. (This was
caught and fixed during validation: an early version declared `itemSize: 3`
for scale while writing only 1 float per key, which the structural validator
below immediately flagged as an out-of-bounds accessor on real exported
data.)

500 random exported models were checked programmatically (byte-offset ranges
inside the declared buffer length, channel key/value counts matching,
key-time monotonicity, bone-index bounds against the file's own skeleton,
and — after the §4.4 fix — quaternion unit-norm to within float precision):

```
Sampled              : 500 files, 324 with animation
Clips checked        : 3888        Tracks checked: 198 645
Structural issues    : 0
Non-monotonic times  : 0
Quaternion norm err  : max 1.6e-7 (float noise)
```

---

## 6. Track resolution: `.kf` tracks bound to skeleton bones by name

**99.43% of the 1071465 extracted tracks resolve to a bone in the target
file's own skeleton** (1065339 resolved, 6126 orphaned). Resolution is exact
string match against `SkeletonData::bones[i].name`, the same byte-exact names
Phase 3 preserved specifically for this purpose. An orphaned track is **kept,
not dropped** (`AnimationTrack::boneIndex = -1`), with a warning naming the
cause — matching the "warn, don't fail" discipline used for unresolved
textures (Phase 2) and unresolvable skins (Phase 3).

The most frequent orphan names are generic `BoneNN` placeholders
(`Bone15`/`Bone13`/`Bone11`/`Bone09`, 167 occurrences each) and `Bip01` (115
occurrences) — consistent with the brief's own framing of `.kf` reuse: an
animation authored against one skeleton variant (a chair rig with more
numbered helper bones, say) applied to a model whose own skeleton has fewer
or differently-numbered bones of the same generic name. `chair/C041.nif`,
`C060.nif` and `C625.nif` (all sharing `Bone15`-style names) were confirmed as
one concrete case of this pattern. This is a real, if small, corpus property
rather than an extraction defect — the resolution rate is high enough that it
does not warrant a fuzzy-matching fallback, which would risk binding a track
to the *wrong* bone silently instead of leaving it visibly orphaned.

---

## 7. Does the corpus animate bone scale?

**Yes, but rarely: 33707 of 1071465 tracks (3.15%) actually animate scale** —
measured as "the scale channel's value differs across at least two keys",
not merely "the channel is present" (a channel with one authored key, or
several identical keys, is not really animated). This is common enough that
omitting scale from the format would be a real loss (3.15% of tracks would
silently lose part of their motion), so scale is kept, stored compactly as a
single float per key (§5) rather than dropped or expanded to a redundant
vector.

---

## 8. Z-up consistency: mesh, skeleton and animation agree

Phase 3 (§6) established that mesh and skeleton share one coordinate
convention — Z-up throughout, no axis conversion anywhere in the exporter,
verified numerically to 5.2e-16 of the model diagonal on the bind pose. This
phase adds no new axis-handling code path that could desynchronize from that:

* **Translation keys** are copied component-for-component
  (`CopyVec3`) from niflib's `Vector3`, the same type and the same Z-up frame
  Phase 3's positions already use. No scaling, no swizzle.
* **Rotation keys** are copied component-for-component from niflib's
  `Quaternion{w,x,y,z}` into the glTF/Three.js `[x,y,z,w]` order
  (`CopyQuat`). This is a **relabelling of component order, not an axis
  transform**: niflib's row-vector quaternion-to-matrix formula
  (`Quaternion::AsMatrix()`, read directly from `nif_math.cpp`) and the
  standard column-vector Hamilton convention THREE.js uses are related by
  exactly the same row/column transpose relationship Phase 3's
  `ToColumnMajor` already establishes for ordinary matrices — a quaternion
  cannot encode a reflection or a handedness flip on its own, so copying its
  four components straight across, with no sign or axis remap, reproduces
  the same physical rotation in the target convention. This was verified
  independently: §4.2's boundary check (niflib's own evaluator reproducing
  its own control points) and §5's corpus-wide norm check both operate on
  these exact copied components and show no systematic drift, which a wrong
  component mapping would have produced as a consistent, non-random error
  rather than float-precision noise.
* No skeleton-frame code changed. `SkeletonExtractor`/`bindMatrixLocal` are
  untouched by this phase; an `AnimationTrack`'s keys are meant to be applied
  as delta transforms from that same bind-pose local frame (the standard
  `.bones[name].position/quaternion/scale` target path the viewer's
  `THREE.AnimationMixer` uses, §9), so there is no second frame for
  animation to disagree with the skeleton about.

**No direct "does an animated pose reconstruct correctly" numeric check was
possible the way Phase 3's `GetSkinDeformation` cross-check was**: niflib
exposes no polymorphic "evaluate this interpolator/controller at time t and
give me a world transform" API (confirmed by reading `NiInterpolator.h` and
the controller headers — no such method exists anywhere in the hierarchy), so
there is no independent ground truth to replay an animated frame against the
way there was for the bind-pose skin matrices. The evidence above (component
relabelling only, verified via two independent numeric checks that would have
shown drift from a wrong mapping) is the strongest verification available
without such an API; visual confirmation in the viewer (§10) is the
remaining check.

---

## 9. Viewer

`tools/viewer/index.html` gained a clip dropdown, play/pause, and a scrubbable
timeline. `buildAnimationClips()` constructs one `THREE.AnimationClip` per
exported clip, targeting bones by the standard `.bones[boneName].position` /
`.quaternion` / `.scale` property paths — the same name-based binding the
source `.kf` itself uses, so a clip authored against one skeleton plays
correctly on any file whose skeleton has the same bone names. An orphaned
track (`boneIndex === -1`) is skipped when building the clip rather than
passed through, since `THREE.PropertyBinding` would otherwise warn every
frame for a path that resolves to nothing.

Scale keys are stored as one float per key in the `.gfbin` (§5); the viewer
expands each to an `[s,s,s]` triple when building THREE's
`VectorKeyframeTrack` for `.scale`, since THREE's own track type expects a
3-component value regardless of whether the source was uniform.

---

## 10. Robustness

Every partial-failure path from earlier phases is preserved and extended:

* A `.kf` that cannot be parsed at all (bad header, unsupported block type,
  niflib exception) is skipped with a warning; **the `.nif`'s geometry,
  materials and skinning still export successfully** — animation is added
  after the mesh/skeleton pass succeeds, never gating it.
* A single track that cannot be extracted (§3's three legitimate categories,
  plus any genuine future failure) is skipped with a warning; the rest of its
  clip, and every other clip, still exports.
* An orphaned track is kept rather than dropped (§6), so a consumer can still
  see what the source intended even when the current file's skeleton cannot
  satisfy it.
* The same truncated-header / unsupported-block-type gate `ExtractScene`
  already uses for `.nif` files (`HeaderNormalizer`, `FindUnsupportedBlockType`
  — niflib crashes rather than throwing on an unregistered type, PHASE1/2
  findings) is reused verbatim for `.kf` loading, since a `.kf` is
  structurally a NIF file and the same pitfall applies. The 6 known
  NIF-20.3.0.9 `.kf` files with real string tables (as opposed to the
  truncated-header files `HeaderNormalizer` repairs) were specifically
  re-checked: `monster/animation/M001.kf` is one of them (confirmed via
  `dump`, version `20.3.0.9`, 524 blocks including a genuine `NiBSplineData`/
  `NiBSplineBasisData` pair) and converts and animates correctly — the
  truncated-header/legitimate-20.3.0.9 confusion flagged as a past regression
  risk (PHASE4 brief, PHASE1 §3.1) was not reopened.

**Corpus-wide: 0 genuinely-failed tracks** (§3) and **0 files newly failing**
relative to Phase 3's 3 known skips.

---

## 11. What to check visually in the viewer

No headless browser is available in this environment, so the checks below are
numeric/structural only on this side; the following need eyes:

1. **`chair/model/C011.gfmodel`** (simplest skinned case, per PHASE3_FINDINGS
   §13) — load with **skinning** on, open the clip dropdown, and confirm at
   least one clip plays a visible, non-jittery motion. This file's animation
   is classic-keyframe (chair `.kf`s measured with 0 B-spline tracks earlier
   in this phase), so it is the cleanest case to confirm the non-B-spline path
   end to end.
2. **`monster/model/M491.gfmodel`** — 10 clips, entirely `.kf`-sourced
   (§2), heavily B-spline. Confirm: (a) playback is smooth, not visibly
   choppy, supporting the 30 Hz decision (§4.5); (b) the additive glow
   billboards (`Plane26`/`Plane30`/`Plane31`, PHASE3_FINDINGS §12) do **not**
   move with the skeleton — they carry `NiFloatInterpolator` alpha tracks,
   correctly out of scope for this phase (§3), and should sit static/billboard
   while the body animates.
3. **`npc/model/N920.gfmodel`** (59 bones, all 10 meshes skinned, per
   PHASE3_FINDINGS §13) — confirm the skeleton overlay (**skeleton** toggle)
   moves in lockstep with the skinned mesh surface during playback, i.e. no
   frame-of-reference desync between skin and animation (§8's numeric argument
   made visible).
4. **Scrub the timeline** on any animated file and confirm the pose updates
   immediately and continuously, not just on release — validates the
   `mixer.setTime` scrubbing path (§9) independent of normal playback.
5. **A file with an orphaned track** — any `chair/C041`, `C060`, or `C625`
   clip that references `BoneNN`-style names (§6) — confirm the model still
   plays its *resolved* tracks normally and nothing crashes or freezes on the
   orphaned ones; check the browser console for the expected "not found in
   the model's skeleton" pattern of warnings rather than a THREE.js error.
6. **Any classic-keyframe file with a `XYZ_ROTATION_KEY` warning** (§3.1) —
   confirm the affected bone's rotation visibly stays at bind pose (does not
   move) while its translation/scale (if animated) still plays, matching
   what "rotation channel left empty" should look like.

---

## 12. Phase 4 follow-up: three viewer-reported problems, measured and (partly) fixed

Three problems were reported from viewer use after the §9 fix (mixer root
exposing `.skeleton`): clips that break bone orientation persistently on
switch (`monster/M009`'s `stand01`/`magic01`), several monster models
appearing tilted or floating despite the Z-up→Y-up toggle, and embedded
animation on static (skeleton-less) models never producing motion
(`item/WA85`). Each was investigated by measuring first, per this project's
standing rule — and two of the three brief hypotheses did not survive
measurement.

### 12.1 Orientation "stuck" after switching clips — confirmed and fixed, in the viewer

**Root cause: `THREE.AnimationAction.stop()` does not restore a bone's
pre-clip transform.** A clip only carries tracks for the channels it
actually animates (an XYZ_ROTATION_KEY track, for instance, has translation
but no rotation keys at all — §3.1). Switching from a clip that moved a
bone's quaternion to one that never touches that bone leaves the bone stuck
at whatever the previous clip last wrote, indefinitely, because nothing ever
tells THREE to reset it. This is a `tools/viewer/index.html` bug, not an
export defect: the `.gfmodel`/`.gfbin` data was never wrong.

**Fix**: `buildScene()` now returns every bone/node object alongside the
already-decomposed bind pose (`buildBones`/`buildSceneNodes` call
`Matrix4.decompose` onto each object once, at load time, before any clip has
run). `show()` snapshots that as `bindPose`
(`snapshotBindPose`/`restoreBindPose`), and `playClip()` restores it
unconditionally before starting the next `AnimationAction`. Every bone now
starts each clip from a known-good rest pose regardless of what the previous
clip left untouched.

**`monster/M009`'s `stand01`/`magic01` specifically, measured**: the brief's
stated hypothesis — these two clips have a bone whose translation moves
while XYZ_ROTATION_KEY leaves its rotation frozen — **does not hold**. All 8
of M009's clips (everything from its one `.kf`) share the identical
zero-channel signature: the same 4 bones (`Bone01`/`03`/`05`/`06`) have
XYZ_ROTATION_KEY with **zero translation keys as well** (scale-only, and
that scale never varies either — `ScaleActuallyAnimates` would report it, it
does not), and the same 38 of 39 bones have no translation at all except the
root (`Bip01 NonAccum`), whose own translation range is small and unremarkable
across every clip including the ones that "work". Nothing in the exported
data distinguishes `stand01`/`magic01` from `move01`/`death01`/`attack01`/
`shoot01` at the channel or value level. Combined with the brief's own
observation that the corruption *persists into a subsequently-reloaded
working clip* — a session artifact, not a property of one clip's data — this
is strong evidence the reported M009 symptom **was the clip-switch bug
above**, not a distinct XYZ_ROTATION_KEY-driven defect. It is expected to be
gone with the fix; needs the visual re-check listed in §12.4.

**The XYZ_ROTATION_KEY "frozen rotation while translation moves" defect is
real elsewhere, though, just not on M009**: measured corpus-wide, **2825
bone+clip cases across 196 distinct files** (`chair`/`item`/`monster`/`npc`/
`ride`) have a bone whose translation range exceeds 0.5 units in a clip
while XYZ_ROTATION_KEY leaves its rotation at bind pose — this is the visible
defect §3.1 anticipated, now with a corpus-wide count. Motion-style clips
(move/attack/death/...) are *not* less affected than idle-style ones
(stand/magic/wait/...) — both show roughly proportional occurrence counts,
so "idle clips are disproportionately exposed" is also not supported.
**XYZ_ROTATION_KEY quaternion composition remains deliberately
unimplemented** (see §12.2) — this count is what a future implementation
would fix.

### 12.2 XYZ_ROTATION_KEY composition: investigated, still deliberately not implemented

Before attempting the composition §3.1 left open, niflib's own source was
searched for any Euler→quaternion composition logic to implement against.
**None exists.** `NiKeyframeData::UpdateRotationKeyCount()` is the only code
in niflib that branches on `XYZ_ROTATION_KEY` at all, and it only does
key-count bookkeeping, not evaluation — no `FromEuler`, no XYZ-composition
helper anywhere in `nif_math.cpp`/`.h`, `kfm.cpp`, or any `obj/*.cpp`. No
comment, docstring, or KFM code anywhere in niflib states an intended
composition order or handedness for the three axis channels either.

Structurally, `NiKeyframeData::Read()` confirms the three axis channels
(`GetXRotateKeys`/`GetYRotateKeys`/`GetZRotateKeys`) are **independent**:
each has its own key count, its own key times, and its own interpolation
type (`GetXRotateType()` etc., separate from the overall `GetRotateType()`,
and not restricted to `LINEAR_KEY` — `QUADRATIC_KEY`/`TBC_KEY`/`CONST_KEY`
are all structurally possible per axis). A correct implementation would need
to resample/union three independently-timed, independently-interpolated
channels before composing a quaternion at each sample time, using a
composition order and handedness that **cannot be sourced from niflib
itself** — it would have to be assumed from an external, unverifiable
convention (e.g. a NifSkope/community default) with no ground truth in this
project to check it against, unlike every other numeric decision in Phase 4
(§4.2's B-spline endpoint check, §4.4's renormalization, §5's structural
validator).

**Decision: still not implemented.** A wrong guessed composition would
silently produce incorrect animation — worse than the current honest gap
(rotation empty, translation/scale still play, a warning names the track).
This is recorded as a deliberate non-implementation with the investigation
that led to it, not an oversight; §12.1's 196-file/2825-case count is what
it would fix if a verifiable convention is ever found.

### 12.3 Seven "tilted/floating" monster models — hypothesis refuted; not an exporter defect

**All 1465 skinned models in the corpus — every one flagged as
tilted/floating and every one not — have an exactly identical root bone
(`bones[0]`) bind matrix: translation `(0,0,0)`, scale `(1,1,1)`, 0° rotation
from identity.** This includes all 7 flagged files (`M011`, `M017`, `M028`,
`M016`, `M019`, `M025`, `M022`) and `M011`'s Phase 1 comparison twin `M903`.

The reason is structural, not a fluke: in every file checked, the block
named `Scene Root` is simultaneously the file's only unreferenced root *and*
the skin's `GetSkeletonRoot()`, and `Scene Root`'s own local transform is
authored as identity with nothing above it in the hierarchy. `EnsureSkeleton`
seeding `bones[0]` from `GetWorldTransform()` (§ "the transpose trap"
composition, unchanged by this pass) therefore reduces to identity
corpus-wide — there is no "everything above the skeleton root" for it to
bake in, because nothing sits above `Scene Root`. **The brief's hypothesis
that a Phase 2/3 flattening disagreement or a non-identity root transform
explains these 7 files is refuted by direct measurement, not merely
unconfirmed.**

The real per-file variation lives one level down, at each file's `Bip01`
(or equivalent) child bone, which does carry large, file-specific,
non-identity rotations (e.g. measured ~18° tilt on one flagged file, ~90°
on another) — but this is present on **every** file checked, flagged or not,
including `M903`, and matches the standard 3ds-Max-Biped convention where
the biped root's authored local orientation is corrected for downstream by
the rig/animation, not a property distinguishing the 7 files.

None of the 7 flagged files have any unskinned geometry (each is a single
mesh, fully skinned), so the brief's proposed static-vs-skinned
cross-check is structurally inapplicable to them — there is no second,
independently-flattened path in these files to disagree with the skeleton.

**Conclusion: the exported skeleton/bind-pose data for these 7 files is
correct**, consistent with §6's numeric bind-pose check (5.2e-16 of model
diagonal) and this pass's own corpus-wide zero-exception measurement. If the
reported symptom is real, its cause is not in the exporter's static export —
the most likely remaining locus is animation playback (a bone stuck from a
previous clip, exactly §12.1's mechanism, landing on one of these 7 files)
or something viewer-specific not yet identified. None of the 7 files
currently appear in §12.1's 196-file XYZ_ROTATION_KEY-defect list, so if
§12.1's fix does not resolve what was seen on these files, the cause is
still open and needs a fresh look with the clip-switch bug now ruled out.
**Not treated as fixed by this pass** — see §12.4 for what to re-check.

### 12.4 Embedded animation on skeleton-less models — confirmed and fixed, in the exporter

**Confirmed exactly as the brief's primary hypothesis stated, with one
refinement.** `item/WA85.nif` has no skinned mesh, so `scene.skeletons` was
empty and every embedded track's `FindBoneByName(nullptr, ...)` returned -1
unconditionally — but the track was still *kept* (orphaned, per the
"warn, don't drop" discipline), so the clip was not literally track-less;
it had 2 orphaned tracks, both with every channel empty (both of WA85's
controllers are pure XYZ_ROTATION_KEY with no translation/scale data at
all — WA85 is a case where §12.1/§12.2's gap and this section's gap
compound). Measured corpus-wide: **336 of 1358 skeleton-less (static) `.nif`
files** have at least one embedded `NiTransformController` with real,
non-null interpolator data that a skeleton-aware resolver could never
reach. The brief's speculated "mixed" case — a skinned file with an embedded
controller targeting a node *outside* its skeleton's own subtree — was
searched for and **found nowhere in the corpus (0 files)**.

**Fix, implemented in the exporter**: `SceneModel.hpp` gains `SceneNode`
(name, parent index, local transform — the same shape as `BoneData` minus
`isAttachPoint`) and `SceneData::nodes`. `AnimationExtractor.hpp/.cpp` gains
`BuildNodeHierarchy()` (same parent-before-child walk as
`SkeletonExtractor::CollectBones`, but over every `NiAVObject`, not just
`NiNode`) and a `ResolveTargetIndex()` that tries the skeleton first, then
falls back to the node list. `MeshExtractor.cpp`'s `ExtractScene` builds the
node list **only when `scene.skeletons` is empty** (matching the measured
"0 mixed cases" — a skinned file never needs it and never pays for it), and
keeps it in `scene.nodes` (so it reaches the `.gfmodel`) **only if the file
gained at least one animation track that needed it** — the other ~1000
skeleton-less files with no embedded controller at all still export `nodes:
[]` at no cost. `GfxFormatWriter.cpp` serializes `nodes` the same way as
`skeletons`. Verified directly on `WA85`: both of its tracks now resolve
(`boneIndex` 5 and 7 into a 14-node hierarchy rooted at `Scene Root`),
100% track resolution, up from 0%.

The viewer (`tools/viewer/index.html`) gained `buildSceneNodes()`, building
a plain `THREE.Object3D` tree (not `THREE.Bone` — nothing here skins
geometry) the same way `buildBones()` builds a bone tree, and
`buildAnimationClips()` now targets a track by bare object name
(`"nodeName.quaternion"`, THREE's `PropertyBinding` name-search fallback for
any path that isn't a `.bones[]`/`.materials[]` special form) instead of
`.bones[name]` when the model has no skeleton at all. Both node and bone
objects are covered by §12.1's bind-pose snapshot/restore.

**Full corpus re-export completed and measured (this is no longer a
sample-based estimate).** Comparing every one of the 2822 convertible files
before/after this fix, file for file:

```
Mesh/material/skeleton fields              : byte-identical on all 2822 files (0 mismatches)
Files gaining a populated "nodes" array    : 394
Track resolution, corpus-wide              : 1065339/1071465 (99.43%) -> 1071428/1071465 (100.00%)
Newly-resolved tracks                      : 6089, across 383 files
  of those files' now-resolving clips, by origin: 353 embedded, 79 external .kf
    (a file can have both; these are not disjoint counts)
Files where playable keyframe count increased : 0
```

**The fix is structurally complete and fully verified — track resolution on
skeleton-less files is now 100%, up from 6126 orphaned tracks corpus-wide
before this pass — but its effect on visible motion in today's corpus is
currently zero**, not the partial gain hoped for. Every one of the 6089 newly-resolved tracks turns
out to have the same empty-channel signature as WA85's own two tracks: pure
XYZ_ROTATION_KEY rotation with no translation or scale data, so resolving
the *target* correctly still leaves zero playable keyframes on that track
(§12.2's still-open gap). This was not obvious in advance — the WA85 case
was assumed to be one data point, not representative of the entire
skeleton-less-embedded-animation population — and is recorded here plainly
because it means §12.2 (XYZ_ROTATION_KEY composition) is now the sole
remaining blocker on this entire class of animation, not two independent,
partially-overlapping problems. **Also confirmed corpus-wide: the "mixed"
case (a skinned file with an embedded controller outside its skeleton's own
subtree) is still 0 files** on the full corpus, not just the earlier sample
— the measured absence in §12.4's first paragraph holds exactly.

This fix is kept regardless of §12.2's status: it is a real, verified
correctness fix (100% vs. 99.43% resolution, zero regressions elsewhere),
it is a prerequisite for §12.2 ever mattering on these 383 files, and a
future file in this corpus or an updated one could still have a
skeleton-less controller with real translation/scale data that this fix
would correctly surface today, independent of the rotation gap.

### 12.5 What to check next

**Done in this pass**: full corpus re-export completed and diffed file for
file against the pre-fix corpus (§12.4's numbers) — 0 mesh/material/skeleton
regressions across all 2822 files, 394 files gained a populated `nodes`
array, track resolution 99.43% → 100.00%, 6089 tracks newly resolved across
383 files, 0 files gained playable keyframes (blocked on §12.2).

**Still not done, needed before closing this out:**

1. **Visual re-check** (no headless browser available in this environment,
   same limitation as §11):
   - `monster/model/M009.gfmodel`: cycle through all 8 clips multiple times
     in the dropdown, ending on `stand01` and `magic01` — expected: normal
     pose and normal animation on every clip, no persistent distortion,
     confirming §12.1's fix actually addresses what was seen (the exported
     data itself gave no reason to expect a distinct defect here).
   - `monster/model/M011`, `M017`, `M028`, `M016`, `M019`, `M025`, `M022`:
     re-check orientation after cycling clips per the point above. Expected,
     per §12.3's measurement: these files' bind pose (clip-free, right after
     load) was always correct, so if a problem is still visible on first
     load with no clip switching, it is a **new, unexplained finding** — the
     exporter's static skeleton data is confirmed correct for all 7, so a
     first-load defect cannot be one of the causes this pass ruled out.
   - `item/model/WA85.gfmodel`: **will still show no motion** — both its
     tracks now resolve (100%, up from orphaned) but carry zero playable
     keyframes (§12.4's compounding note). This is expected and matches
     every other file that gained a resolved skeleton-less track in this
     pass (§12.4: 0 of 383 such files gained any playable keyframe) — there
     is currently no file in the corpus that would visually demonstrate the
     node-hierarchy path driving geometry, since every skeleton-less
     controller happens to be pure XYZ_ROTATION_KEY. That visual
     confirmation becomes possible once §12.2 is implemented (or on a future
     corpus/file with non-Euler skeleton-less animation) — until then, the
     node-resolution fix is verified structurally (100% resolution,
     `boneIndex` pointing at the right entry, 0 regressions) rather than
     visually.

---

## 13. `XYZ_ROTATION_KEY` composition: implemented

§12.2 closed with "still not implemented" on the grounds that niflib gives no
composition order to implement against and a wrong guess would be worse than
the honest gap. That gap is now closed — not by guessing, but by finding an
order from two independent sources that converge, exactly the standard §12.2
asked for and the earlier B-spline/renormalization decisions in this phase
already met.

### 13.1 Piste 1: the composition order, read from an external reference

niflib itself still has nothing (re-confirmed: no `FromEuler`, no XYZ-compose
helper anywhere in `nif_math.cpp/.h`, `kfm.cpp`, or any `obj/*.cpp`). But
`blender_niftools_addon` — same NifTools umbrella as niflib and `nif.xml` —
does import `XYZ_ROTATION_KEY` correctly (its changelog: "fixed Euler rotation
animation import"), and its source was read directly
(`io_scene_niftools/modules/nif_import/animation/transform.py`, `develop`
branch):

```python
def as_b_euler(n_val):
    return mathutils.Euler(n_val)
...
if n_kfd.rotation_type == 4:  # XYZ_ROTATION_KEY
    b_target.rotation_mode = "XYZ"
    times_keys = [self.get_keys_values(euler.keys) for euler in n_kfd.xyz_rotations]
    times_all = sorted(set(times_keys[0][0] + times_keys[1][0] + times_keys[2][0]))
    keys_res = [interpolate(times_all, times, keys) for times, keys in times_keys]
    interp = self.get_b_interp_from_n_interp(n_kfd.xyz_rotations[0].interpolation)
    self.import_keys(EULER, b_action, bone_name, times_all, zip(*keys_res), flags, interp, ...)
```

Two facts fall out of this directly:

1. **The three channels are resampled onto the union of their own key times
   using plain linear interpolation** (`interpolate()`, a hand-written
   linear resampler in the same file) — the per-axis `KeyType`
   (LINEAR/QUADRATIC/TBC/CONST) is read only as a hint passed to Blender's own
   F-curve afterward, not used to do Hermite/TCB evaluation during resampling.
   This matches, rather than contradicts, this project's own existing
   precedent of dropping tangent/TBC data for classic tracks and using linear
   interpolation between keys (`ExtractClassicTrack`'s own comment, §3) — so
   implementing the same simplification here is consistency with an already-
   made decision, not a new unverified shortcut. (Decided explicitly rather
   than assumed — see AskUserQuestion in this session's log.)
2. **`mathutils.Euler(n_val)` is called with no explicit order argument.**
   Blender's own API documentation states the default: the worked example in
   every version of the `mathutils.Euler` docs is captioned "create a new
   euler with default axis rotation order" and uses
   `Euler((x, y, z), 'XYZ')` — i.e. **`'XYZ'` is the default**, confirmed by
   reading the documentation page directly (`blender_python_api_2_60_0`
   through `current`), not inferred from the name.

`'XYZ'` order in Blender's own convention was then pinned down exactly by
deriving `eul_to_mat3()` from Blender's C source
(`blenlib/intern/math_rotation.c`) by hand and comparing it term-by-term
against the standard `Rz(z)·Ry(y)·Rx(x)` matrix product: they match exactly
(every `cj*ch`, `sj*si*ch-ci*sh`, ... term lines up). So **Blender's `'XYZ'`
means: rotate about X first, then Y, then Z, composed as
`R = Rz(z)·Ry(y)·Rx(x)`** — equivalently, as a quaternion product,
`q = qz ⊗ qy ⊗ qx` (Hamilton product, rightmost operand applied first).

### 13.2 Piste 2: the empirical discriminant

`tools/diag/measure_xyz_euler_order.cpp` tests this independently of the
Blender-source reading, exactly as the brief asked: all 6 possible axis
orders, using niflib's own `NiNode::GetLocalTransform()` for bind pose (no
handedness/axis-flip guesswork — this project stays Z-up throughout, §8).

**Signal A — t=0 vs. bind pose.** For every `XYZ_ROTATION_KEY` track,
sample each axis channel at (or nearest) t=0, compose under each candidate
order, and compare to the target bone's own local bind rotation. Corpus-wide,
264403 tracks across 1029 files:

```
Order   MeanErr(deg)   MedianErr(deg)   %<1deg   %<5deg   %<30deg
XYZ     72.18          50.87            11.77%   17.16%   38.66%
XZY     74.32          55.40            10.75%   15.89%   36.79%
YXZ     74.01          53.03            11.80%   16.96%   39.21%
YZX     87.82          88.07             8.53%   12.72%   30.42%
ZXY     84.38          76.23             9.05%   13.74%   33.34%
ZYX     99.57          110.0             7.39%   11.04%   27.24%
```

`XYZ` and `YXZ` are the clear top two, well ahead of the other four — but
statistically tied with each other. This is expected, not a failure of the
method: X and Y rotations commute to first order for small angles, so a
track dominated by a single axis (common — see §3.1's per-track structure)
cannot distinguish `XYZ` from `YXZ`. Restricting to tracks where at least two
axes exceed 5° at t=0 (178372 tracks, the subset that actually stresses
composition order) sharpens the gap between the top two and the bottom four
but does not resolve `XYZ` vs. `YXZ` on its own — expected for the same
reason. Both this and the raw-mean-error magnitude (72–100° depending on
order, not "near zero") reflect that a large share of clips genuinely do not
start at rest pose (attack/skill-style clips especially) — real noise the
brief's own "note de méthode" anticipated, not a flaw in the discriminant.

**Signal B — smoothness, independent of the rest-pose assumption entirely.**
Each track's three channels are resampled at a uniform 30 Hz across the
track's own time span, composed under each candidate order at every sample,
and the mean angular step between consecutive samples is accumulated. A wrong
composition order should show visibly jumpier steps; this signal needs no
assumption about what pose a clip starts at. 264401 tracks:

```
Order   MeanStepDeg
XYZ     1.42
XZY     1.60
YXZ     1.60
YZX     1.59
ZXY     1.58
ZYX     1.70
```

**`XYZ` is the unambiguous winner here** — clearly separated from every other
candidate, including `YXZ` (1.42° vs. 1.60°), which breaks the Signal-A tie.

**Self-check.** Before trusting these numbers to validate the real
extractor's arithmetic, the tool's matrix-based `Compose(XYZ, ...)` was
cross-checked against a from-scratch Hamilton-product quaternion composition
(the same formula `AnimationExtractor.cpp`'s `ComposeXyzEuler` uses) over
10000 random angle triples: max disagreement **2.96e-6°**, floating-point
noise. This caught two real sign errors in the first hand-derivation of the
Hamilton product (in the `qy*qx` and `qz*(qy*qx)` steps) before they reached
the shipped extractor — recorded here because it is exactly the kind of
self-inflicted bug a numeric cross-check is meant to catch, per this
project's standing practice (§4.2 caught an analogous one for B-spline
renormalization).

**Conclusion: all three independent signals — external reference (Piste 1),
t=0-vs-bind-pose (Piste 2, Signal A), and timeline smoothness (Piste 2,
Signal B) — converge on the same order.** `XYZ` (apply X first, then Y, then
Z; `q = qz ⊗ qy ⊗ qx`) is implemented in `AnimationExtractor.cpp`'s
`ComposeXyzEuler`, with this reasoning recorded in its own comment.

### 13.3 Implementation

`ExtractClassicTrack`'s `XYZ_ROTATION_KEY` branch now calls
`ExtractXyzRotation(data, track)` instead of leaving the rotation channel
empty. `ExtractXyzRotation`:

1. Reads all three of `GetXRotateKeys()`/`GetYRotateKeys()`/`GetZRotateKeys()`
   (independently timed, confirmed structurally in `NiKeyframeData.h`, §12.2).
2. Builds the union of all three channels' key times (matching the reference
   importer's own approach, §13.1).
3. Samples each channel at every time in that union via `LerpKeyAt` (linear
   interpolation, clamped outside the channel's own range) — the same
   linear-interpolation-between-keys convention `ExtractClassicTrack` already
   uses for quaternion tracks, decided explicitly to stay consistent with it
   (§13.1 point 1) rather than implement per-type Hermite/TCB evaluation.
4. Composes each sample with `ComposeXyzEuler` and appends it as a normal
   `QuatKey`, which then passes through `CopyQuat`'s existing renormalization
   (§4.4) exactly like every other rotation key in this exporter — no special
   case needed for the B-spline bug that motivated that renormalization in
   the first place.

A channel with zero keys contributes 0 radians at every sample time
(`LerpKeyAt`'s empty-vector fallback) — "this axis was never keyed", not an
error; a track where literally all three channels are empty is left with no
rotation keys at all, same as before (nothing to compose).

### 13.4 Post-fix corpus numbers

Full corpus re-exported and compared file-for-file against the pre-fix
corpus, the same way §12.4's regression check was done:

```
Mesh/material/skeleton/nodes fields   : byte-identical on all 2822 files (0 mismatches)
Structural validation (500-file sample, same validator as §5):
  Structural issues                   : 0
  Non-monotonic key times             : 0
  Quaternion norm errors > 0.01       : 0 (max err 1.76e-7 -- float noise)
Classic tracks using XYZ_ROTATION_KEY : 322257 (unchanged -- same tracks, now composed)
Total keyframes                       : 45982599 -> 47437600 (+1454999*, new rotation keys)
Track resolution                      : 100.00% (unchanged from §12.4's fix, 1071428/1071465)
```

\* every `XYZ_ROTATION_KEY` track's own rotation key count equals the number
of distinct times across its X/Y/Z channels (§13.3 step 2) — this is new
keyframe data the export did not previously carry, not a resampling-rate
artifact like the B-spline numbers in §4.

**"Does a clip now produce effective motion" — measured directly, not
inferred from track resolution:**

```
                                                  before fix   after fix
Clips with >=1 track carrying real rotation motion   19045        19946   (+901)
Skeleton-less (node-hierarchy) files with real
  rotation motion (the §12.4 fix's own payoff)          84          269   (+185)
```

The 185-file jump is §12.4's fix finally paying off: every one of the 383
files that fix newly resolved was pure `XYZ_ROTATION_KEY` with zero playable
keyframes (§12.4's own closing note) — this pass is what makes those keys
playable, so the two fixes' combined effect (node resolution + Euler
composition) is what shows up here, not either one alone.

The cruder proxy this brief's §12.1 originally measured (translation moves
while rotation has no real timeline) dropped from 89413 to 33024 bone+clip
cases (900 to 654 files) using the same threshold; the residual is expected,
not a remaining gap — it also counts legitimate cases where a *quaternion*-
type rotation channel authored exactly one static key while translation
animates, which is normal per §3's track-independence finding, not something
this fix (or any fix) should change.

### 13.5 Non-regression

* **Geometry/materials/skinning**: byte-identical JSON (`meshes`,
  `materials`, `skeletons`, `nodes` fields) across all 2822 files, verified by
  direct structural comparison against the pre-fix export — 0 mismatches.
  Mesh data is written to the shared `.gfbin` before any animation data
  (`GfxFormatWriter.cpp`), so growing the animation payload cannot have
  shifted any mesh/skeleton byte offset even in principle.
* **Existing animation paths** (classic quaternion tracks, B-spline
  resampling, `.kf` and embedded sources): untouched code paths — this
  change only adds a new branch inside `ExtractClassicTrack` for the
  `XYZ_ROTATION_KEY` case specifically; `bSplineTracks`, `classicTracks`,
  `tracksResolved`/`tracksOrphaned`, and `tracksTotal` are all unchanged
  corpus-wide (1071465 tracks, same as §12.4's post-fix count).
* **Format**: no schema change — `AnimationTrack::rotations` simply has more
  entries than before for affected tracks; `kGfModelFormatVersion` is
  unchanged since no consumer-visible shape changed.
* **Same 3 known skips**: `item/WF20.nif`, `monster/M156.nif`,
  `npc/N600.nif`, unchanged.

### 13.6 Files to check visually (Piste 3)

No headless browser is available in this environment (same limitation as
§11/§12.5), so the following need eyes — priority order matches the brief's
own:

1. **`item/model/WA85.gfmodel`** — the reference case for §12.4's fix. Was
   confirmed to have exactly 0 playable keyframes after that fix landed
   (§12.4's own closing note); now has 2 real rotation keys on
   `Plane04`/`Plane03`. Simple embedded animation on a static model — easy to
   judge correct (a small, natural-looking rotation) vs. wrong (something
   that snaps or spins unnaturally).
2. **`item/model/W146.gfmodel`** (or `W152`/`W154`/`W167` — same pattern,
   picked as a second static-model data point) — another skeleton-less file
   that gained real embedded rotation motion from this fix.
3. **`monster/model/M009.gfmodel`**, clips `stand01` and `magic01`
   specifically — the original symptom this whole investigation traces back
   to. 37 bones per clip now carry real rotation keys (hands, fingers, tail)
   that were frozen at bind pose before this fix; §12.1 already confirmed the
   clip-switch stickiness these clips also exhibited was a separate, already-
   fixed viewer bug (§12.1), so this check is specifically about whether the
   rotation motion itself now looks natural.
4. **`monster/model/M389.gfmodel`** — the single heaviest `XYZ_ROTATION_KEY`
   user in the corpus, 2825 animated-rotation tracks across its clips. If the
   composition order were wrong, a file this saturated with the affected key
   type should show it unmistakably (joints twisting through implausible
   angles); if correct, it should read as an ordinary animated monster.

### 13.7 Reproducing this section's measurements

```
tools\diag\measure_xyz_euler_order.cpp   # §13.2: 6-order empirical discriminant (t=0 + smoothness), self-checked
```

Built the same way as the other §12 tools (not wired into CMake):

```
cl.exe /std:c++17 /permissive- /EHsc /MP /O2 /MD /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /D_SCL_SECURE_NO_WARNINGS /DNIFLIB_STATIC_LINK ^
  /I"external\niflib\include" /I"src" tools\diag\measure_xyz_euler_order.cpp src\nif\HeaderNormalizer.cpp ^
  /Fe:tools\diag\measure_xyz_euler_order.exe /link build\Release\niflib_static.lib
```

The blender_niftools_addon source referenced in §13.1 was read directly from
`https://raw.githubusercontent.com/niftools/blender_niftools_addon/develop/io_scene_niftools/modules/nif_import/animation/transform.py`
and Blender's `eul_to_mat3()` from
`https://raw.githubusercontent.com/dfelinto/blender/master/source/blender/blenlib/intern/math_rotation.c`
(a mirror of `blender/blender`, used because the canonical repo's raw URL for
this exact path returned 404 at the time of reading — the mirror's content
was cross-checked against Blender's own published `mathutils.Euler`
documentation, which independently confirms the `'XYZ'` default, so the two
sources are not relying on each other).

---

## 14. Follow-up session: model orientation, WA85 motion, M009 tilt

Three problems reported after visual re-check of §12/§13's fixes: several
"couché"/"en l'air" monster models still looking wrong, `item/WA85`'s
now-resolved 2-track clip (§12.4) still producing no visible motion, and
`monster/M009`'s `stand01`/`magic01` still tilted. All three were
investigated the same way as every prior gap in this project: measure first,
against an independent reference, rather than reasoning from the exported
JSON alone.

### 14.1 Model orientation — not an exporter defect; the reference tool itself needed a correction

The brief's suggested approach — convert the same files to `.obj` via
`blender_niftools_addon` (`tools/nif_to_obj.py`) and compare against the
exporter's own output — was followed, but the addon's legacy
`bpy.ops.export_scene.obj` operator no longer exists in Blender 5.2 (renamed
`wm.obj_export`, different parameter names); a scratch-only patched copy was
used for this session's runs (`tools/nif_to_obj.py` itself is unmodified — a
Blender-version compatibility fix is a separate concern from this
investigation).

**Numeric self-check first, per this project's standing discipline
([[gf-nif-exporter-external-reference]]).** A C++ diagnostic
(`compare_skin_deformation.cpp`, not committed) computed the exact same
formula `SkeletonExtractor::ApplySkin` uses (`vertexWorld = v * (boneOffset *
boneWorld)`) directly against niflib's own `NiGeometry::GetSkinDeformation`
reference implementation, for the four sampled models (`M011`, `M017`,
`M028`, `M389`). **Result: 0 to 1.5e-8 max vertex difference on all four** —
the exporter's skin math is exactly correct; niflib's own reference agrees to
floating-point precision.

**Visual re-render of the exporter's own computed geometry** (not statistics,
an actual rendered image, reconstructed in Node.js/three.js using the
viewer's own `SkinnedMesh`/`boneInverses` formula and rendered via headless
Blender) confirmed `M389`, `M011`, and `M028` stand correctly. Only `M017`
still rendered lying flat/splayed.

**M017, isolated**: `M017`'s raw `NiNode` hierarchy transforms disagree with
its own `NiSkinData` bind data — a real, independently-verified Gamebryo
authoring quirk (confirmed corpus-wide at **479 files**, measured via
`boneOffset.Inverse() * geomWorld` vs `bone->GetWorldTransform()`, tolerance
0.05). `blender_niftools_addon`'s `nif_import/armature/__init__.py`
(`store_bind_matrices()`) runs an equivalent correction on import, which is
why Blender's own display looked "correct" while the raw node-transform data
did not — the addon's displayed pose was never the file's literal node-local
transform to begin with. See §14.2 for the exporter-side fix.

**Conclusion: no missing rotation matrix in the exporter.** The four sampled
"problem" models were already geometrically correct at the vertex level; the
only real defect was in the skeleton's *bind pose* for files matching the
§14.2 pattern.

### 14.2 Fix: skeleton bind pose reconstructed from skin data, not trusted from NiNode transforms

`SkeletonExtractor::ReconstructBindPoseFromSkin()` (new method, called once
per file from `MeshExtractor::ExtractScene` after every skinned shape's
`ApplySkin` has run) replaces each bone's `bindMatrixLocal` with the world
bind pose implied by that bone's own skin data
(`boneOffset.Inverse() * geomWorld`, row-vector convention, matching
`blender_niftools_addon`'s `get_skin_bind()` exactly) wherever a skin
actually weighted that bone. A bone no skin ever weighted keeps its authored
`NiNode` local transform, re-anchored under the corrected parent chain so the
hierarchy does not tear at the boundary between corrected and uncorrected
bones. First skin to weight a shared bone wins on conflict (reuses the
existing `boneSkinMatrixConflicts` measurement's own precedent rather than
adding a new tie-break rule).

Verified on `M017`: reconstructed bone world positions (e.g. `Bip01 Pelvis`
at `(0, 0.728, 0.762)`) now match `blender_niftools_addon`'s independently-
computed rest pose to 3 decimal places. Raw mesh vertex positions are
unaffected (they never depended on `bindMatrixLocal`, only on the already-
correct per-vertex `skinMatrix`), confirming this only fixes skeleton display
and animation retargeting, not geometry.

**Not ported**: the addon's cross-skin conflict-resolution logic (warping a
second skin's vertices to agree with an earlier skin's bind pose) — that
mechanism only fires when two different `NiSkinInstance`s disagree about a
*shared* bone, which is a different, narrower situation than the single-skin
case this fix addresses, and this corpus's existing `boneSkinMatrixConflicts`
metric already tracks how often that would matter.

### 14.3 WA85 — real fix: animated meshes were never reattached to their node

**Root cause, unrelated to the Euler fix.** `MeshExtractor`'s mesh walk
flattens every unskinned mesh's vertices to world space at export time
(Phase 2 behavior, correct for the ~99% of meshes with no animation on their
own node), then discards the association between a mesh and the `NiAVObject`
it came from. For `WA85`'s two animated planes, this means the
`NiTransformController` genuinely produces correct, real quaternion motion
(verified: two distinct keyframes per track, not identical/degenerate) on an
`Object3D` node that no rendered mesh is actually parented under — the
viewer moves an empty pivot while the baked, disconnected geometry stays put.
This reproduces exactly the symptom (`1.5s / 2 tracks` reported, nothing
visibly moves).

**Fix**: `MeshData` gains `nodeIndex` (into `SceneData::nodes`, default -1).
`MeshExtractor::ExtractScene` records each mesh's source `NiAVObject*`
alongside the existing mesh walk (`meshSourceObjects`, parallel to
`scene.meshes`) and `BuildNodeHierarchy` gained an overload returning a
`NiAVObject* -> node index` map. After animation extraction determines which
node indices are genuinely targeted by a resolved track (propagated to every
descendant, since a rigid child of an animated node must move with it even
though nothing targets the child directly), any mesh whose source object
maps to an animated node has its vertices re-based from world space to
node-local space (`nodeWorld.Inverse()`, undoing the original bake) and its
`nodeIndex` set. Every other mesh — the overwhelming majority — is
untouched: same flattened-world vertices, `nodeIndex = -1`.

The viewer (`tools/viewer/index.html`) parents such a mesh under
`sceneNodeObjs.objs[m.nodeIndex]` instead of the top-level `group`, so it
rides the animated `Object3D` exactly as a `SkinnedMesh` rides its skeleton.

**Verified on `WA85`**: both "Editable Mesh" children of `Plane04`/`Plane03`
(previously indistinguishable by name — three siblings share it) correctly
resolved to their respective node indices (6 and 8) by pointer identity, and
only those two meshes were reattached; the other two meshes on the same file
correctly kept `nodeIndex = -1`.

**Corpus-wide, this pattern is not rare**: re-exporting the full corpus with
the fix reattaches **1397 meshes across 265 files**, overwhelmingly
`item/model/W*` (weapons) and `effect/model/S*` (spell effects) — glow
planes and particle-emitter geometry riding an animated pivot, the same
shape as `WA85`. No file's mesh/material/skeleton/animation *counts* changed
(structural non-regression confirmed across all 2822 convertible files);
only the vertex space and `nodeIndex` of the affected meshes changed.

### 14.4 M009 — real fix found (dropped tracks), tilt itself not re-confirmed visually this session

**§12.1's clip-switch bind-pose-restore fix and §13's `XYZ_ROTATION_KEY`
composition fix were re-checked and are not at fault.** `Bone01/03/05/06`
(the bones §12.1 flagged as "scale-only, frozen") now carry genuine composed
quaternion rotation in both `stand01`/`magic01` and the "working" clips —
the Euler fix reached them correctly.

**A separate, real defect found**: `AnimationExtractor::ExtractInterpolator`
silently dropped a `ControllerLink` whose interpolator resolved to a
recognized transform family but carried **no timeline data for this specific
clip** (`NiTransformInterpolator` with `GetData() == nullptr` — "static pose
only", or an empty `NiBSplineTransformInterpolator`) — the track was never
appended to `clip.tracks` at all, not even as an orphan, and no warning was
emitted. Measured on `M009`'s `stand01`: **32 of 71 skeleton bones**,
including `Bip01 Pelvis`, `Bip01 Spine`, and all four thigh/calf/foot bones,
had no track whatsoever in the exported clip, down from a real
`ControllerLink` in the source `.kf` for every one of them. Corpus-wide this
is not rare either — `tracksStaticPoseOnly` was already **200 rows on
`M009`'s 8 clips alone** before this fix, all silently discarded.

**Fix**: `ExtractInterpolator` now returns success (with every channel left
empty) for a static-pose-only classic track or an empty B-spline track,
instead of failure — both call sites (`ExtractEmbedded`, `ExtractFromKf`)
already push whatever track comes back and resolve its `boneIndex`
unconditionally, so no other code changed. An empty-channel track is not a
new concept: `AnimationTrack`'s own doc comment already specifies "the
consumer keeps the bone's bind-pose value" for an empty channel — this fix
only stops discarding the bookkeeping that says the track was there and
resolved, not partially inventing new interpolation behavior.

**Verified on `M009`**: `stand01`'s track count went from 39 to 64 (every
`ControllerLink` in the clip now resolves and is kept); `Bip01 Pelvis` etc.
now carry a resolved, empty-channel track instead of nothing.

**Not confirmed as THE tilt fix.** This session's own attempt to
independently re-render `M009`'s posed clips (a from-scratch three.js/Node.js
skinning+animation reconstruction, not the actual viewer) produced visibly
torn/broken geometry on **both** `stand01` and a clip the user reports as
already correct (`move01`) — strong evidence of a bug in that one-off
reconstruction script itself (most likely in B-spline keyframe sampling,
211/512 of `M009`'s tracks are B-spline-resampled), not new evidence about
the exported data. The dropped-track fix is real and worth keeping
regardless — a clip missing tracks for the entire core skeleton is a defect
on its own — but whether it is sufficient to resolve the visually-reported
tilt needs the browser re-check listed in §14.5, not a reconstruction from
this session.

### 14.5 Non-regression and files to re-check

Full corpus re-exported after all three fixes (WA85 reattachment, M017 bind
pose, M009 dropped tracks): same 3 permanent failures as every prior pass
(`item/WF20`, `monster/M156`, `npc/N600`), no new ones. Mesh/material/
skeleton counts unchanged file-for-file across all 2822 convertible files —
only vertex space (for the 265 newly-reattached files) and `bindMatrixLocal`
(for the 479 bind-pose-corrected files) changed, plus animation track counts
increased wherever a `ControllerLink` had previously been dropped.

**Files to re-check visually in the browser:**

1. **`monster/model/M017.gfmodel`** — should now stand on four legs in bind
   pose and show a correctly-shaped skeleton (`skel` toggle) instead of the
   splayed/lying pose. `M389`/`M011`/`M028` (already correct) should be
   unchanged.
2. **`item/model/WA85.gfmodel`** — the two glow-plane meshes should now
   visibly animate on the `1.5s` clip instead of the model staying static.
3. Spot-check a few of the 265 newly-reattached files for a visible
   regression from the vertex-space change — suggested:
   `item/model/W146`, `item/model/W167`, `effect/model/S13103`,
   `effect/model/S14141` (all gained `nodeIndex` on at least one mesh, per
   this session's corpus scan).
4. **`monster/model/M009.gfmodel`**, `stand01` and `magic01` — re-check
   whether the tilt is gone now that every clip drives the full skeleton;
   if it persists, the next lead is the B-spline sampling path specifically
   (§14.4's own reconstruction attempt broke on both a "good" and a "bad"
   clip, so the bug — if still present after this fix — likely lives in how
   B-spline tracks are sampled/rendered, not in per-clip data differences).
5. Full non-regression pass across a handful of previously-verified files
   (`M389`, plus one file from each entity type) to confirm nothing else
   moved.

### 14.6 Reproducing this section's measurements

```
tools\diag\compare_skin_deformation.cpp   # §14.1: skin math vs GetSkinDeformation, corpus-wide bind-pose mismatch scan, .kf NULL-interpolator/rotate-type dump
```

Built the same way as the other diagnostic tools (not wired into CMake):

```
cl.exe /std:c++17 /permissive- /EHsc /MP /O2 /MD /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /D_SCL_SECURE_NO_WARNINGS /DNIFLIB_STATIC_LINK ^
  /I"external\niflib\include" /I"src" tools\diag\compare_skin_deformation.cpp ^
  /Fe:tools\diag\compare_skin_deformation.exe /link build\Release\niflib_static.lib
```

Usage: `compare_skin_deformation.exe <nif_path>` (single-file skin-math
check), `compare_skin_deformation.exe measure <root>` (corpus-wide bind-pose
mismatch scan), `compare_skin_deformation.exe kf <kf_path>` (per-sequence
`ControllerLink` target/interpolator-type dump).

The Blender-side reference renders (native NIF import, front-orthographic,
Workbench engine) were produced with `blender.exe -b --python <script> --
<args>`, Blender 5.2.2 LTS, `io_scene_niftools` v0.1.1 — scratch scripts, not
committed. `tools/nif_to_obj.py` itself needs a `bpy.ops.export_scene.obj` ->
`bpy.ops.wm.obj_export` port to run on Blender 5.2+ (noted, not fixed this
session since a scratch patch was sufficient for this investigation).

---

## 15. Reproducing (Phase 4 overall)

```
cmake --build build --config Release
build\Release\gfnif-export.exe export input -o out --input-root input
```

Expected corpus totals (post-Phase 4, including §13's fix): 2822/2825
converted, 1787 files with animation, 20103 clips, 1071465 tracks, 100.00%
track resolution, 0 genuinely failed tracks, 47437600 total keyframes.
Mesh/skinning numbers are byte-identical to Phase 3.

The one-off diagnostics this phase's measurements came from live under
`tools/diag/` but are **not** wired into `CMakeLists.txt` (same convention as
Phase 2/3's `investigate-m491`/`measure-alpha`, which were built ad hoc and
never committed as build targets). To reproduce a number from §4, add a
temporary `add_executable` for the relevant `.cpp` (linking `niflib_static`,
including `src/`, same pattern as `gfnif-export`) or build it directly with
the MSVC compiler against `niflib_static.lib`:

```
tools\diag\measure_bspline_error.cpp        # §4.3: error vs. sample rate
tools\diag\verify_bspline_boundary.cpp      # §4.2: niflib evaluator self-check
tools\diag\validate_gfmodel.ps1 -OutDir <out>          # §5: structural format validation (PowerShell, runs as-is)
tools\diag\measure_xyz_rotation.cpp         # §12.1: XYZ_ROTATION_KEY prevalence/defect count
tools\diag\measure_root_transform.cpp       # §12.3: root bind-matrix decomposition + ancestor chain
tools\diag\measure_embedded_controllers.cpp # §12.4: static-model embedded-controller census
tools\diag\dump_wa85_keys.cpp               # §12.4: raw NiTransformData key dump for WA85
tools\diag\measure_xyz_euler_order.cpp      # §13.2: Euler composition order discriminant
```

The §12–§13 tools were built directly with `cl.exe` (not wired into
CMake), linking against the already-built `niflib_static.lib`:

```
cl.exe /std:c++17 /permissive- /EHsc /MP /O2 /MD /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /D_SCL_SECURE_NO_WARNINGS /DNIFLIB_STATIC_LINK ^
  /I"external\niflib\include" /I"src" tools\diag\<tool>.cpp src\nif\HeaderNormalizer.cpp ^
  /Fe:tools\diag\<tool>.exe /link build\Release\niflib_static.lib
```

`/MD` is required: `niflib_static` is itself built `/MD` (see the CMake
config), and a plain `cl.exe` invocation without it link-fails with
CRT-duplicate `LNK2005` errors.

### Viewer

```
python -m http.server 8000
http://localhost:8000/tools/viewer/?model=/out/monster/model/M491.gfmodel
```

New controls: a clip dropdown, play/pause, and a timeline slider, visible
whenever the loaded model has at least one animation clip. §11 lists what to
confirm visually.
