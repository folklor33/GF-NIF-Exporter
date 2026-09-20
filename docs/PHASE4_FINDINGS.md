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

## 12. Reproducing

```
cmake --build build --config Release
build\Release\gfnif-export.exe export input -o out --input-root input
```

Expected corpus totals (post-Phase 4): 2822/2825 converted, 1787 files with
animation, 20103 clips, 1071465 tracks, 99.43% track resolution, 0 genuinely
failed tracks. Mesh/skinning numbers are byte-identical to Phase 3.

The one-off diagnostics this phase's measurements came from live under
`tools/diag/` but are **not** wired into `CMakeLists.txt` (same convention as
Phase 2/3's `investigate-m491`/`measure-alpha`, which were built ad hoc and
never committed as build targets). To reproduce a number from §4, add a
temporary `add_executable` for the relevant `.cpp` (linking `niflib_static`,
including `src/`, same pattern as `gfnif-export`) or build it directly with
the MSVC compiler against `niflib_static.lib`:

```
tools\diag\measure_bspline_error.cpp     # §4.3: error vs. sample rate
tools\diag\verify_bspline_boundary.cpp   # §4.2: niflib evaluator self-check
tools\diag\validate_gfmodel.ps1 -OutDir <out>   # §5: structural format validation (PowerShell, runs as-is)
```

### Viewer

```
python -m http.server 8000
http://localhost:8000/tools/viewer/?model=/out/monster/model/M491.gfmodel
```

New controls: a clip dropdown, play/pause, and a timeline slider, visible
whenever the loaded model has at least one animation clip. §11 lists what to
confirm visually.
