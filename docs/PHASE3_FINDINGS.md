# Phase 3 findings — skeleton & skinning

**Goal of Phase 3:** extract the full `NiNode` bone hierarchy and per-vertex
skinning for every skinned `.nif` in the corpus, export one shared skeleton per
file, and confirm visually that a skinned mesh sits in an undeformed bind pose
with its skeleton superimposed.

**Result: all 2823 convertible files export, 1465 of them with a skeleton.**
6610 skinned meshes, 3 665 755 skinned vertices, 90 384 bones. The two known
skips (`M156.nif`, `WF20.nif`) are unchanged.

> **Update (§11):** a name-based filter for 3ds Max editor helpers left in the
> source files (bone gizmos, biped boxes) removes 8389 meshes / 305 249
> vertices of non-visual geometry from the static path.
>
> **Update (§12):** a second, unrelated cause of the same "opaque parasitic
> surface" symptom was found after §11 and the alpha/transparency fix
> (PHASE2_FINDINGS §10): 10 156 shapes across 900 files are marked **hidden**
> by the source file's own `NiAVObject` visibility flag and were being
> exported anyway. Filtering on that flag removes them and, as a side effect,
> also explains most of the `InvBind conflicts` noise (corpus-wide: 8227 →
> 6427). One additional file (`npc/N600.nif`) now reports a clean "no visible
> geometry" rather than converting a model that has none — see §12.
>
> Both filters are measured, additive corrections to the numbers below; read
> §11 and §12 for what changed and why. Everything else in this document was
> measured before either and is still valid on its own terms.

The headline correctness result: **every skinned mesh whose weights come from
`NiSkinData` reproduces niflib's own `NiGeometry::GetSkinDeformation` to within
1e-6**, and the exported bind pose reconstructs to **5.2e-16 of the model
diagonal** — floating-point noise.

---

## 1. Export results

```
EXPORT SUMMARY: 2823/2825 file(s) converted
Vertices          : 6 188 840      (identical to Phase 2; see §11 for the
Triangles         : 7 278 167       post-hoc helper-geometry filter's effect)

--- skinning ---
Files with skeleton : 1465
Skeletons           : 1465 (90 384 bones, max 437 in one file)
Skinned meshes      : 6610
Skinned vertices    : 3 665 755
Mixed skinned/static: 672 file(s)   (601 after §11 -- see there)
Skins failed        : 0
Unweighted & drawn  : 3 vertices (of 3.67M)
```

Output is 545 MB across 5646 files. Runtime is unchanged at ~12 s. §11's
filter removes 305 249 of the 6 188 840 vertices above as non-visual editor
geometry; skinning figures (bones, skinned meshes/vertices) are unaffected,
since no helper mesh in the corpus is ever skinned.

Skinning is spread across 7 of the 8 entity types — `chair` 111, `char` 1,
`elf` 16, `item` 95, `monster` 537, `npc` 286, `ride` 419. Only `effect/` has
no skinned file, consistent with it being particle/texture animation.

---

## 2. Influences per vertex (brief question 1)

Measured across all 3 665 755 skinned vertices:

| Influences | Vertices | Share |
|-----------:|---------:|------:|
| 1 | 2 716 947 | 74.117% |
| 2 | 812 238 | 22.157% |
| 3 | 126 800 | 3.459% |
| 4 | 7 494 | 0.204% |
| 5 | 1 652 | 0.045% |
| 6 | 519 | 0.014% |
| 7 | 84 | 0.002% |
| 8–13 | 18 | 0.000% |

**The maximum observed is 13.** 96.3% of vertices use one or two bones, and
**only 2273 vertices (0.0620%) exceed the 4-influence limit** and are
truncated.

The policy is the one the brief specifies: sort by descending weight, keep the
4 largest, renormalise to sum 1. The cost is negligible and bounded:

* total weight discarded across the corpus: **264.7** (out of 3.67M vertices
  each summing to 1, i.e. **0.007%** of all weight)
* largest single discarded weight: **0.2**

A weight of 0.2 is the worst case anywhere in the corpus, on a vertex that
still keeps 4 influences covering ≥0.8 — the renormalisation redistributes it
proportionally among bones that are, by construction, already the dominant
ones. Renormalisation is applied unconditionally rather than only after
truncation, since it also repairs drift already present in the source and is a
no-op when weights already sum to 1.

Sorting uses `std::stable_sort` so equal weights keep a reproducible bone
order and re-exports stay byte-identical.

---

## 3. `NiSkinData` vs `NiSkinPartition` (brief question 2)

`NiSkinPartition` is present on **all 6610** skinned meshes — none absent. It
is a genuinely independent encoding of the same skinning, so it makes a strong
cross-check. Every influence it asserts was compared against what we extracted:
**5 092 720 influences checked.**

```
influences compared : 5 092 720
bone missing in data: 0            <- the check that must hold
bone truncated away : 534          (expected: we cap at 4)
weight differs      : 41 100       (expected: see below)
partition < ours    : 288 (max shortfall 0.5354)
```

**There is no case in the corpus where the partition names a bone influencing
a vertex and `NiSkinData` does not.** That is the result that matters: it
proves the bone mapping, the skeleton indexing and the weight transposition are
read correctly, across five million influences.

### Weights are *expected* to differ, and do

41 100 influences differ in weight by more than 1e-3. This is **not** a defect
and was verified to be the partition's design:

A partition caps the bones per submesh and renormalises whatever survives, so
its weights are systematically **higher** than the uncapped `NiSkinData` ones.
Sampling the divergences shows exactly that shape — `ours 0.667 → part 1.0`,
`ours 0.333 → part 0.5`, `ours 0.25 → part 0.333`. Of 20 426 sampled
divergences, **20 420 had the partition weight ≥ ours**, and the ratios are the
clean renormalisation factors that a dropped-influence-plus-rescale produces.

`NiSkinData` is therefore used as the source of truth, exactly as the brief
requires — it is the un-capped original, and the partition has already applied
the same 4-slot limit we apply ourselves. The 288 cases (0.006%) where the
partition weight is *lower* are the only ones the subset-and-renormalise story
does not explain; they are left as-is and recorded here.

### One real divergence: files where `NiSkinData` has no weights at all

`NiSkinData` carries a `hasVertexWeights` flag. **In 7 meshes across 5 files it
is 0**: the block stores the per-bone transforms but no weights whatsoever, and
the weights exist *only* in `NiSkinPartition`.

This was found because those meshes reported 100% of vertices with zero
influences. Every affected file is one of the 86 **truncated-header** files
from Phase 1 §3.1 — a distinct authoring vintage. (The header repair itself is
sound: 57 of the 62 truncated-header files that are skinned verify perfectly.)

The extractor falls back to `NiSkinPartition` **per vertex**, filling only
vertices `NiSkinData` left empty, so genuine `NiSkinData` weights always win.
The effect is decisive:

| | before fallback | after |
|---|---:|---:|
| drawn vertices with no influence | 8 673 | **3** |
| partition influences unaccounted for | 15 178 | **0** |

The 3 remaining vertices (in `M202`, `R521`) are weighted by neither source.
They are pinned to the skeleton root with weight 1 so they stay at their bind
position; left at zero weight they would collapse onto the origin and tear the
mesh open. Unweighted vertices that no triangle references are ignored
entirely — nothing renders them.

---

## 4. Mixed skinned and unskinned meshes (brief question 3)

The corpus does this routinely: **672 files** contain both skinned and
unskinned geometry, concentrated in `ride/` and `chair/` as the brief
predicted (e.g. `ride/R880` — 25 meshes, 8 skinned, 17 static).

`skeletonIndex = -1` holds up cleanly. Checked programmatically over a 700-file
sample: **174 mixed files, 4602 static sub-meshes, 0** with a stray
`skeletonIndex`, `skinBindings` entry or skin attribute. Static sub-meshes keep
the Phase 2 world-space flattening and are drawn unchanged; skinned ones keep
raw local positions and are placed by their bones.

---

## 5. The vertex representation (required decision)

**Decision: one `Vertex` type, always carrying 4 influence slots, with
`MeshData::isSkinned` saying whether they hold real data. The writer omits the
two skin attributes from the `.gfbin` entirely for an unskinned mesh.**

The brief offered this against a separate `std::vector<SkinnedVertex>` plus a
flag. The single type wins on the criterion asked for — what simplifies
`GfxFormatWriter`:

* The writer walks **one** vertex array and emits attributes from it. With two
  arrays every attribute loop, offset calculation and accessor emission would
  need a skinned and an unskinned spelling, roughly doubling the function's
  branching for no gain.
* `MeshExtractor` likewise keeps one `BuildVertices` path; skinning is layered
  on afterwards by `SkeletonExtractor::ApplySkin` rather than forking geometry
  construction.
* A failed skin degrades to a static export by simply rebuilding positions in
  world space — no array to migrate between.

The cost is 24 bytes per vertex of in-memory padding on unskinned meshes
(`uint16[4]` + `float[4]`). That is memory only: **nothing reaches the
`.gfbin`**, because the writer emits `skinIndex`/`skinWeight` only when
`isSkinned`. A consumer sees no difference from the two-array design, and the
JSON simply lacks those two accessors on a static mesh.

`StaticVertex` is kept as an alias so Phase 2 call sites still read naturally.

---

## 6. Coordinate frames, and why the bind pose is exact

**Z-up is preserved, for mesh and skeleton alike. No axis conversion happens
anywhere in the exporter** — `ToColumnMajor` changes matrix *storage
convention* only (niflib's row-vector layout to the column-major one glTF and
Three.js expect) and never touches handedness or axis order. The viewer rotates
for display, as in Phase 2.

This was the subtlest part of the phase and cost the most debugging, so the
conventions are worth recording precisely.

### The composition that is correct

niflib's own `NiGeometry::GetSkinDeformation` is the ground truth. It feeds the
**raw, untransformed** `NiGeometryData` vertices through

```
vertexWorld = v * (boneOffset * boneWorld)        // niflib, row-vector
```

Notably it uses **neither** `NiSkinData`'s overall `skinTransform` **nor** the
shape's own world matrix. Both were tried; each makes things worse:

| composition | worst bind-pose drift |
|---|---:|
| `boneOffset * boneWorld` (niflib's own) | **exact** |
| `+ overall transform` | 70× worse on some files |
| `+ shape world matrix` | 23× model diagonal |

On the `R773`/`R774` family the overall transform is precisely the *inverse* of
the shape's world matrix, so including either double-counts the node placement
— visible as a clean **+4.866 Z translation** with the rotation part coming out
a perfect identity.

**Skinned vertices are therefore exported raw, in the shape's local space**,
while unskinned geometry keeps the Phase 2 world flattening. This is what
PHASE2_FINDINGS §9.2 anticipated.

### The transpose trap

Converting to column-major transposes, and **transposing a product reverses its
factors**:

```
T(boneOffset * boneWorld) = T(boneWorld) * T(boneOffset)
```

Converting the two matrices *separately* and multiplying them therefore yields
the factors in the wrong order — which is precisely the bug that produced drifts
of 1.8 to 10 units. The row-vector product is now formed **first** and converted
**once**.

### `skinMatrix`, not an inverse bind matrix

Each `SkinBinding` stores the **complete** bind-pose skin matrix — the one
taking a raw vertex straight to its bind-pose world position — rather than a
classical inverse bind matrix. A consumer then never has to rebuild the bone's
world matrix from the hierarchy, nor match our multiplication convention, just
to draw the bind pose. The viewer recovers a standard `THREE.Skeleton`
`boneInverse` from it in one line when it needs one.

### The inverse bind matrix is per-skin, not per-bone

A bone's inverse bind matrix **cannot** live on the shared skeleton. Measured
on this corpus, two `NiSkinInstance`s in the same file routinely give the same
`NiNode` materially different `NiSkinData` transforms: **8227 such conflicts,
differing by up to 14.45**. `N920` alone has 8. Storing one per bone would force
picking a winner and misplacing every mesh that wanted the other.

They are therefore stored per mesh, in `MeshData::skinBindings`, and
`Vertex::boneIndex` indexes *that* list — glTF's `joints` +
`inverseBindMatrices` split, for the same reason glTF makes it.

### Proof the two frames agree

If mesh and skeleton disagreed about convention or axis frame, the skeleton's
bone world matrices would not cancel against the skin matrices. Replicating the
viewer's exact arithmetic —

```
boneInverse[i] = bone.matrixWorld⁻¹ · skinMatrix[i]
skinned(v)     = Σ wₖ · (bone.matrixWorld · boneInverse[jₖ]) · v
```

— over **314 skinned files and 777 775 vertices**, with bone world matrices
derived *independently* from the `bindMatrixLocal` chain:

```
worst bind-pose deformation = 5.2e-16 of the model diagonal
```

That is float rounding. The bind pose is undeformed, and mesh and skeleton
provably share one coordinate frame.

---

## 7. Skeleton construction

* Built from `NiSkinInstance::GetSkeletonRoot()`, walking **every** `NiNode` in
  the subtree — not just bones carrying weights. Intermediate nodes keep parent
  chains unbroken, and Phase 4 binds `.kf` tracks by node name to nodes that may
  carry no weights at all.
* **Bone names are stored exactly as the `NiNode` spells them** — no case
  folding, no trimming — as the brief requires for `.kf` binding.
* **One skeleton per file**, built on first use and shared by every skinned
  mesh in it. No file in the corpus named a second skeleton root (0 warnings).
* Bones are emitted **parent-before-child**, so a consumer computes world
  matrices in a single forward pass. Verified on every exported skeleton.
* The root bone is seeded with its full **world** transform, since niflib's
  `GetWorldTransform` (which the skinning is expressed against) includes
  anything above the skeleton root.
* Bones are keyed by **node pointer, not name**: names are not unique in this
  corpus. The DAG-sharing guard from Phase 2 applies — a node reachable from two
  parents is taken on its first path, since a bone needs exactly one parent.
* Max bones in one file: **437** (`chair/C625`). Max depth is unchanged at 28.

---

## 8. Output format changes

`"skeleton": null` is replaced by a `"skeletons"` array (plural, per the brief's
request to stay extensible), and meshes gain three keys.

```jsonc
{
  "meshes": [{
    "isSkinned": true,
    "skeletonIndex": 0,
    "skinBindings": [                     // glTF joints + inverseBindMatrices
      {"bone": 12, "skinMatrix": [ /* 16 floats, column-major */ ]}
    ],
    "attributes": {
      "position": { ... },
      "skinIndex":  {"byteOffset": …, "itemSize": 4, "count": …, "type": "uint16"},
      "skinWeight": {"byteOffset": …, "itemSize": 4, "count": …, "type": "float32"}
    }
  }],
  "skeletons": [{
    "rootName": "Scene Root",
    "boneCount": 59,
    "bones": [
      {"name": "Bip01", "parent": -1, "bindMatrixLocal": [ /* 16 floats */ ]}
    ]
  }],
  "animations": [],        // phase 4
  "particleSystems": []    // phase 5
}
```

* `skinIndex`/`skinWeight` are **absent entirely** on an unskinned mesh, so a
  consumer reads them only when `isSkinned` is true.
* `skinIndex` is `uint16` — half the space of `uint32`, and what THREE expects.
  Since it leaves the buffer 2-byte aligned on an odd vertex count, the writer
  pads to 4 bytes before the `float32` weights, which a `Float32Array` view
  requires. Index padding is now explicit too, so a future attribute of another
  width cannot silently break alignment.
* All matrices are **column-major**, directly consumable by
  `THREE.Matrix4.fromArray`.

### Format validation

500 random exported models were checked programmatically: declared binary
length, every accessor range inside the buffer, every accessor count equal to
`vertexCount`, attribute alignment, index bounds, `indexCount` a multiple of 3,
weights sorted descending and summing to 1 on every drawn vertex, bone and
binding indices in range, parents before children, and unskinned meshes free of
skin data. **0 issues across 500 models (273 skinned, 10 970 bones).**

---

## 9. Robustness

A skin that cannot be resolved **never fails the file and never drops the
geometry**: the mesh is rebuilt in world space and exported static, with a
warning naming the cause. Guarded cases: missing `NiSkinData`, unusable
skeleton root, `NiSkinInstance`/`NiSkinData` bone-count mismatch, a null bone
slot, a bone outside the skeleton root's subtree, and an out-of-range vertex
index in the weight list.

**Across the full corpus, 0 skins failed.** The only new warnings this phase
introduces are the 7 partition-fallback notices from §3. Total warnings rose
757 → 764, entirely from those.

---

## 10. Carried into Phase 4

1. **Bone names are the binding key.** They are preserved byte-exact, and
   `.kf` clips address nodes by these strings. Names are *not* unique within a
   file, so `.kf` binding must be prepared for ambiguity — the extractor keys by
   node pointer for this reason.
2. **`skinMatrix` is a bind-pose convenience.** Animation replaces bone-local
   transforms per frame, so Phase 4 must compute bone world matrices from the
   `bindMatrixLocal` chain and combine them with the inverse bind recovered as
   `boneWorldAtBind⁻¹ · skinMatrix`. §6 gives the exact arithmetic, and the
   viewer already does it.
3. **B-spline compressed animation** (`NiBSplineCompTransformInterpolator`,
   5654 occurrences in the Phase 1 sample) remains the main risk, unchanged.
4. **`NiTriStrips` is never skinned** — re-confirmed: all 6610 skinned meshes
   are `NiTriShape`.
5. The `GFNIF_VERIFYSKIN` hook (§13) is worth keeping as a regression check
   when the skinning math is touched again.

---

## 11. Filtering 3ds Max editor helpers (post-hoc correctness fix)

Visual validation in the viewer showed **blank grey surfaces** floating
around some models — geometry with no texture or detail. The node names
involved matched a known pattern: helper objects (bone gizmos, biped boxes,
collision primitives, attachment nubs) that 3ds Max leaves in a NIF export
and that Grand Fantasia's pipeline never stripped.

### Measurement before filtering (as instructed — this was done first)

The starting hypothesis was a name regex covering `plane`, `editable poly`,
`editable mesh`, `poly mesh`, `cylinder`, `biped object`, `bone`,
`right/left rope nub`, `shield nub`, `sphere`, `box`. **Measuring it against
the full corpus immediately showed the naive version is far too broad:**

```
MESHES total 41 783   matching 34 056 (81.5%)   verts 2 056 358 of 6 188 840
```

Breaking the match down by name and cross-referencing against whether the
mesh's texture actually resolved gave a completely different picture,
because **texturing, not the name, is what actually separates a gizmo from
real geometry**:

| name pattern | meshes | avg verts | % textured |
|---|---:|---:|---:|
| `editable poly` | 12 985 | 95.4 | **91%** |
| `plane` | 11 145 | 29.3 | **99%** |
| `editable mesh` | 1 074 | 109.6 | **92%** |
| `cylinder` | 176 | 88.8 | **94%** |
| `polymesh` | 144 | 299.7 | **93%** |
| `sphere` | 94 | 77.2 | **82%** |
| `bone` | 3 882 | 43.5 | **0%** |
| `biped object` | 3 120 | 33.0 | **0%** |
| `box` | 1 432 | 25.0 | **3%** |
| `*rope nub` / `shield nub` | 4 | 32.0 | **0%** |

`editable poly`/`plane`/`editable mesh`/`cylinder`/`polymesh`/`sphere` are
**91–99% textured** — real body/prop geometry that artists simply left under
Max's default object name. `bone`/`biped object`/`box`/the nubs are **0–3%
textured**, and inspecting examples confirms them as gizmos: `"Bone"` is a
32-vertex/14-triangle octahedron, `"Biped Object"` and `"Box"` are
24-vertex/12-triangle cubes, always alpha 1, always untextured, frequently
duplicated as `Bone@#0`, `Bone14`, etc.

**The 40 exceptions matter just as much as the pattern.** A handful of
`Box01`/`Box02`/`Box03` meshes *do* carry a resolved texture and real
geometry — one is 427 vertices. A name-only filter would have deleted them.

### One regression caught by re-measuring, and the correction

The first implementation additionally included `sphere`, `cylinder` and
`dummy` (`sphere`/`cylinder` were 82%/94% textured pre-filter — weaker
evidence than `bone`/`biped object`/`box`, but included anyway on the
strength of the brief's starting list). Running the exporter against the
full corpus caught the mistake immediately: **`elf/X1051.nif` dropped to zero
exported geometry.** Its entire visible mesh is a single `NiTriStrips` named
`"Sphere06"` whose texture reference happens not to resolve — indistinguishable
from a gizmo by the name+texture test alone. `sphere`/`cylinder`/`dummy` were
removed from the collision-helper list; only `bone`, `biped object` and `box`
remain, matching the categories with near-0% texturing. Re-running the full
corpus afterwards: **0 files with zero geometry, all 2823 convertible files
still convert.**

### Final classification rule

`src/nif/NameClassifier.{hpp,cpp}` implements two categories, checked with an
anchored match (the whole name must be the word plus an optional numeric/`@#`
duplicate suffix — `Boxer` and `Bonefish` do **not** match):

* **`CollisionHelper`** — `bone`, `biped object`, `box`. Geometry dropped
  **only if also untextured** (`ShouldDropGeometry` requires both).
* **`AttachPoint`** — `right rope nub`, `left rope nub`, `shield nub`, `nub`.
  Same drop rule; kept in the skeleton (below) for future weapon/shield/rope
  attachment.

### Answering the brief's three questions

**1. Do these nodes carry real geometry, or are they empty anchor points?**
Both exist, and they are cleanly separate objects — **zero overlap** between a
helper-named `NiNode` (skeleton bone) and a helper-named mesh in the whole
corpus (638 distinct helper bone names checked). The `NiTriShape` gizmos are
always their own, non-bone nodes.

**2. Category breakdown.** Category A (collision/edit remnants —
`bone`/`biped object`/`box`, untextured): **8389 meshes, 305 249 vertices,
across 867 files.** Category B (attach points, untextured): 4 meshes, 128
vertices (2 `shield nub`, 1 each `left`/`right rope nub`).

**3. Do attach-point-named bones carry visible geometry, or are they also
empty?** Also empty — the 4 untextured nub meshes above are the same handful
of gizmos as category A, just named for their attachment role rather than
their primitive shape.

### What is kept vs. dropped, precisely

* **`MeshExtractor`** drops the `NiTriShape`/`NiTriStrips` geometry outright
  when `ShouldDropGeometry(name, hasResolvedTexture)` is true — filtered at
  the source, never reaching `MeshData`, `.gfbin`, or the viewer.
* **`SkeletonExtractor`** keeps **every** `NiNode`, helper-named or not.
  Measured directly: **1565 of the 15 679 helper-named bones are the parent of
  a bone with an ordinary name.** Pruning them would break 1565 parent chains
  for a category the brief explicitly warned against — bone objects
  legitimately used as rig structure. `BoneData::isAttachPoint` flags the 56
  `*nub`-named bones as advisory metadata; nothing is removed from
  `SkeletonData` on account of a helper name.

### Non-regression: corpus totals before/after

```
                    before filter   after filter    change
Files converted     2823/2825       2823/2825       unchanged
Vertices            6 188 840       5 883 591       -305 249 (-4.9%)
Triangles           7 278 167       7 131 151       -147 016 (-2.0%)
Meshes dropped as helper                            8389
Files w/ 0 geometry 0               0                (checked explicitly)
Mixed skinned/static 672            601             -71 (files whose only
                                                       static mesh was a helper)
```

A 4.9% vertex drop concentrated in 24–95-vertex meshes, spread across 867
files, is the expected shape for stripping small decorative gizmos — not the
"massive drop" that would indicate an overreaching filter. Skinning is
untouched: skinned mesh/vertex counts (6610 / 3 665 755) and the
`GFNIF_VERIFYSKIN` ground-truth result (6603 verified, same 5 above 1e-3, same
causes as §3/§13) are byte-identical to the pre-filter run, since no helper
mesh in the corpus is ever skinned.

### Non-regression: geometry sample check

* **All 40 textured `Box*` meshes survive** the filter (checked by name +
  `textureFound`), including the 427-vertex `item/WA73.gfmodel` one.
  **All 8389 targeted untextured `bone`/`biped object`/`box`/nub meshes are
  gone** — a post-filter corpus scan finds zero of them left.
* Re-ran the format validator (§8) and the bind-pose/skeleton-coherence
  checks (§6) on 500 random post-filter models: **0 structural issues**,
  worst bind-pose deformation **7.1e-16 of the model diagonal** — identical in
  character to the pre-filter results, confirming the geometry filter did not
  disturb skinning or format correctness.
* The four files used for visual validation in §13 (`chair/C011`,
  `npc/N920`, `ride/R880`, `ride/R330`) were re-checked: none of their meshes
  matched the drop rule (none are named `bone`/`biped object`/`box`/a nub), so
  their vertex counts are unchanged by this fix and remain valid reference
  files.

---

## 12. Hidden geometry — the second cause of "opaque parasitic surfaces"

**Ticket: after §11 (helper-name filter) and PHASE2_FINDINGS §10
(alpha/transparency), `monster/model/M491.nif` still showed residual flat
opaque surfaces in the viewer.** M491 was a deliberately chosen stress case:
34 meshes, 12 materials, 148 bones, 12 skinned, mixed skinned/static, and
**every one of its 9 texture references resolves** — ruling out the previous
two causes by construction.

### Step 1 — full inventory of M491's 34 meshes

Built a one-off diagnostic (`investigate-m491`, walked the file directly with
niflib) rather than reasoning from the exported `.gfmodel`, since the
questions below need data the export format does not carry (raw `flags`,
`NiStringExtraData`, all texture slots).

| # | name | topology | verts/tris | skin | parent | material alpha | `NiAlphaProperty` | tex slots | flags | bbox | extraData |
|--:|---|---|---:|:-:|---|--:|---|---|---|---|---|
| 1 | KNOCK01 | TriShape | 180/155 | yes | M491 | 1.0 | test, thr=125 | BASE=M49101 | 0x16 vis | 0.45×0.33×0.75 | — |
| 2 | KNOCK02 | TriShape | 424/432 | yes | M491 | 1.0 | test, thr=125 | BASE=M49101 | 0x16 vis | 1.66×2.38×2.25 | — |
| 3 | M491 | TriShape | 2935/3111 | yes | M491 | 1.0 | test, thr=125 | BASE=M49101 | 0x16 vis | 4.11×2.44×3.63 | — |
| 4–8, 10–13, 15–18, 20–23, 25 | Editable Poly[@#N] | TriStrips | 4/2 each | no | Plane01…16 | 0.2–0.5 | blend, dst=ONE | BASE=M32855 | 0x16 vis | 0.5×0.5×0 **flat** | — |
| 9, 14, 19, 24 | Editable Poly@#{4,9,14,19} | TriStrips | 4/2 each | no | Plane25/24/23/22 | 0.4 | blend, dst=ONE | BASE=M32855, DETAIL=M47752 | 0x1e vis | 1.17×0.83×0 **flat** | — |
| 26 | Plane30 | TriShape | 4/2 | yes | @11 | 0.1 | blend, dst=ONE | BASE=M32855 | 0x1e vis | 5.92×5.92×0 **flat** | UserPropBuffer="zMode10" |
| 27 | Plane26 | TriShape | 4/2 | yes | @11 | **0.0** | blend, dst=ONE | BASE=M08151 | 0x1e vis | 5.25×5.25×0 **flat** | UserPropBuffer="zMode10" |
| 28 | Plane31 | TriShape | 4/2 | yes | @11 | **0.0** | blend, dst=ONE | BASE=M08151 | 0x1e vis | 3.01×3.01×0 **flat** | UserPropBuffer="zMode10" |
| **29** | Editable Poly | TriShape | 72/36 | yes | @ | 1.0 | none | *none* | **0x17 HIDDEN** | 2.18×1.72×0.54 | NiStringED000="NiOptimizeKeep" |
| **30** | Editable Poly | TriShape | 9/8 | yes | @ | 1.0 | none | *none* | **0x17 HIDDEN** | 2.34×2.34×0.15 **flat** | NiStringED000="NiOptimizeKeep" |
| 31 | M01 | TriShape | 178/200 | yes | @ | 0.7 | blend, dst=ONE | BASE=M47752, DARK=M49052 | 0x1e vis | 3.23×1.17×1.63 | UserPropBuffer="zMode10" |
| **32** | Editable Poly | TriShape | 76/64 | yes | @ | 1.0 | none | *none* | **0x17 HIDDEN** | 3.55×2.47×1.62 | NiStringED000="NiOptimizeKeep" |
| **33** | Editable Poly | TriShape | 120/96 | yes | @ | 1.0 | none | *none* | **0x17 HIDDEN** | 2.58×2.58×2.17 | NiStringED000="NiOptimizeKeep" |
| 34 | M49151 | TriShape | 62/34 | yes | @ | 1.0 | test, thr=125 | BASE=M49151 | 0x16 vis | 2.79×0.11×2.94 | — |

(Rows 4–25's near-identical repeated data are collapsed for space; the full
34-row raw dump is what `investigate-m491` printed and is reproducible with
the command in §12's reproducing block.)

The table alone identifies the culprits: **meshes #29, #30, #32, #33** are the
odd ones out on every axis that matters — `flags = 0x17` (bit 0 set), no
material properties at all, no texture, tagged `NiOptimizeKeep`. Everything
else, including the 24 small flat "Editable Poly" quads and the `Plane*`/`M01`
glow layer, is legitimately visible, correctly textured, and already carries
real `NiAlphaProperty` data from the previous two fixes.

### Step 2 — the six leads

**1. Hidden bit in `NiAVObject.flags` — confirmed, the actual cause.**
`NiAVObject::GetVisibility()` (niflib, `flags & 1`) is false for exactly
meshes #29/30/32/33. Nothing else in the file is hidden. This is the
authoritative, file-declared signal the brief was hoping for: unlike the
`NameClassifier` heuristics (name + texture, because names alone were 81.5%
false positives — §11), a shape the source file itself marks invisible needs
no corroborating evidence.

**2. Textures with uniform alpha — checked, not the cause on M491.**
`M32855.png` (the additive glow quads' texture): alpha range **(0, 237)** —
real, non-uniform data. `M47752.png` (the DETAIL/dark layer): **(0, 220)**.
`M49101.png` (the body's alpha-test texture): **(0, 255)**. All three carry
genuine transparency; this lead is negative for M491 specifically, though it
remains a real, separate possible cause worth keeping in mind for other files
(21% of the corpus's PNGs were already measured to have uniform alpha in
PHASE2_FINDINGS §10).

**3. Multi-slot `NiTexturingProperty` — a real gap, but not this bug's cause.**
Confirmed the exporter only reads slot 0 (`BASE`): meshes #9/14/19/24 also
populate `DETAIL`, and #31 also populates `DARK`. Both are legitimate,
correctly-alpha-channeled textures the exporter currently discards. This
would show up as **reduced visual fidelity** (missing a detail/darkening
layer) on an already-correctly-transparent mesh, not as an opaque surface —
the `BASE` slot alone already carries a resolved, blended texture on all of
these. Left unfixed; flagged for a future pass, out of scope for "parasitic
opaque surfaces" specifically.

**4. `NiStringExtraData` / UPB — informative, not itself actionable.**
The four hidden meshes carry `NiStringED000 = "NiOptimizeKeep"`, a 3ds
Max-exporter marker meaning roughly "don't let the optimizer discard this" —
consistent with editor-only geometry kept around rather than deleted. Four
*visible* meshes (#26–28, #31) instead carry `UserPropBuffer = "zMode10"`, an
engine render-mode hint. Neither string is a portable, general-purpose
visibility signal on its own (only the flags bit is), but `NiOptimizeKeep`
usefully corroborates the hidden-flag finding on this file, and is worth
watching for if a future file needs the same call made without a hidden flag
to lean on.

**5. `InvBind conflicts` (31, max delta 2.642) — checked, and resolved as a
side effect.** All four hidden meshes are skinned (`skinned: yes`), so they
were contributing to that count. After filtering: **M491's conflicts drop
from 31 to 3**; corpus-wide, 8227 → 6427. This was never the cause of a
*visible* artifact (Phase 3's per-mesh skin-matrix design already prevents a
conflicting inverse bind from misplacing a mesh — see PHASE3_FINDINGS §6) but
confirms the hidden shapes were poorly-maintained data on more than one axis,
consistent with them being abandoned/superseded geometry.

**6. Flat/coplanar meshes — checked, not a discriminator.** 28 of the 34
meshes are geometrically flat (all but the 3 main body parts and the 2
alpha-tested detail pieces). This is expected, not suspicious: the 24 small
quads and the `Plane26/30/31` layer are billboards for a glow/particle
effect, which are supposed to be flat. Flatness correlates with "is a
billboard" here, not with "should be filtered" — two of the four actually
hidden meshes (#29, #32) are *not* flat at all. Confirmed as a dead end for
this ticket and not used in the fix.

### Step 3 — the fix

`NiAVObject::GetVisibility()` is checked at the very top of `WalkNode`'s
geometry handling in `MeshExtractor.cpp`, before material extraction or the
`NameClassifier` name check — cheaper, and there is no scenario where a
shape the file marks hidden should still be considered for either of those.
Unlike `NameClassifier::ShouldDropGeometry`, this needs no texture-presence
gate: the flag is an explicit statement from the source data, not a
heuristic, so there is no plausible false-positive class analogous to
`item/WA73.gfmodel`'s textured `Box02` (§11) to protect against.

```cpp
if (!obj->GetVisibility()) {
    ++result.hiddenGeometriesDropped;
    return;
}
```

### Measured before applying, corpus-wide

```
total shapes (NiTriShape/NiTriStrips) = 41 783
hidden (flags bit 0 set)              = 10 160  (24.3%)
  hidden AND carrying a texture       =    965  (9.5% of hidden)
  hidden AND carrying a NiAlphaProperty = 911
  hidden, large (>100 verts) AND textured = 461
hidden vertices                       = 590 447
files containing >= 1 hidden shape    =    900 / 2825  (31.9%)
```

**90.5% of hidden shapes are untextured** — the same profile as M491's four
culprits. The other 9.5% (965 shapes, 461 of them over 100 vertices) are
textured but still explicitly hidden: consistent with deliberately-kept
LOD/costume-variant geometry the Max pipeline toggles off rather than
deletes, which is exactly what the visibility flag is *for*. There is no
plausible reading of "the file says don't draw this" as a false positive the
way a bare name match was, so this was applied unconditionally rather than
gated the way §11's filter had to be.

**One file, `npc/model/N600.nif`, has every one of its 4 shapes hidden** —
confirmed by inspection, not inferred: all 4 carry `flags = 0x17`. This is
the file's own declaration that it has no visible content (most plausibly an
unused/placeholder or trigger-only NPC entity), not an over-filtering
accident like `elf/X1051.nif` was during §11. The exporter's "no geometry"
error was extended to say so explicitly rather than reading as a generic
failure:

```
npc/model/N600.nif: no visible geometry (4 shape(s) present but all marked
hidden in the source file)
```

It is the only such file in the corpus; every other file that lost meshes to
this filter still has visible geometry left, confirmed in the non-regression
check below.

### Non-regression: corpus totals

```
                       before (§11 state)   after (this fix)   change
Files converted        2823/2825            2822/2825          -1 (N600, see above)
Vertices                5 883 591            5 597 529          -286 062 (-4.9%)
Triangles                7 131 151            6 782 167          -348 984 (-4.9%)
Shapes dropped as hidden                                        10 156
Files w/ 0 meshes exported  0                    0               (checked explicitly)
Skinned meshes           6 610                5 887             -723
InvBind conflicts        8 227                6 427             -1 800
```

A further −4.9% on top of §11's −4.9% (helpers) is proportionate to a corpus
where a quarter of all shapes turn out to be explicitly hidden leftovers —
not the disproportionate, "lost real geometry" shape that flagged the
`sphere`/`cylinder` mistake in §11. No file besides the one legitimate case
above lost all its geometry: re-scanned every `.gfmodel` after the fix and
found zero with an empty `meshes` array.

### Non-regression: format/skinning validation

* **Structural validator** (500 random post-fix models): **0 issues**, 277
  skinned files, 10 726 bones compared.
* **Bind-pose viewer simulation** (same 500 models): worst deformation
  **7.5e-16 of the model diagonal** — unchanged in character from §6/§11's
  results.
* **`GFNIF_VERIFYSKIN` ground truth**: 5880 meshes verified (down from 6603,
  matching the 723 fewer skinned meshes), **same 5 known exceptions** as
  §3/§13 (`M202`, `M254`, `M407`, `R521`, one `Editable Poly`) — no new
  skinning discrepancy introduced.
* M491 itself: 34 → 30 meshes, `InvBind conflicts` 31 → 3, skinned meshes
  unchanged at 8 (the 4 dropped meshes were the *only* hidden ones; all
  originally-visible skinned meshes survive), bone count unchanged at 148.

### What to check in the browser (manual — no headless browser here)

1. Open `M491.gfmodel` with **skinning** on, **texture** on. The four flat
   grey/untextured "Editable Poly" panels that used to sit at odd angles
   across the model should simply be **gone** — not transparent, not
   invisible-but-present, gone from the scene graph entirely.
2. The additive glow quads (the ~24 small billboards plus the `Plane26/30/31`
   layer) should now render as soft glowing overlays with visible depth —
   confirm they do **not** occlude geometry behind them (that was the
   depth-write fix from PHASE2_FINDINGS §10, still relevant here since these
   are exactly the additive-blend meshes that fix targets).
3. `KNOCK01`/`KNOCK02`/`M491`/`M49151` (the alpha-tested body parts) should
   look identical to before this fix — they were never hidden and carry no
   change.
4. Cross-check on `ride/model/R880.gfmodel` (5 hidden shapes measured in this
   file, previously reachable in the Phase 3 validation set): confirm no
   previously-visible part of the model has disappeared — only genuinely
   hidden geometry should be gone.

---

## 13. Reproducing

```
cmake --build build --config Release
build\Release\gfnif-export.exe export input -o out --input-root input
```

Ground-truth check against niflib's own skinning (opt-in, prints one line per
skinned mesh):

```
set GFNIF_VERIFYSKIN=1
build\Release\gfnif-export.exe export input -o out --input-root input
```

Expected (post-§12): **5880 meshes verified, 5 above 1e-3** — 2 from legitimate
4-influence truncation (`M254`, `M407`; deltas 0.020 and 0.007) and 3 from the
root-pinned vertices of §3 (`M202`, `R521`, one `Editable Poly`). The 7
partition-sourced meshes are skipped, since niflib's reference reads
`NiSkinData`, which for exactly those files holds no weights. (Before §12's
hidden-geometry filter this read 6603 meshes verified, same 5 exceptions —
the 723-mesh difference is entirely hidden skinned shapes no longer exported,
not a change in correctness.)

### Viewer

```
python -m http.server 8000
http://localhost:8000/tools/viewer/?model=/out/npc/model/N920.gfmodel
```

New toggles: **skeleton** (a `THREE.SkeletonHelper` per skeleton root) and
**skinning** (build `THREE.SkinnedMesh` vs. plain `THREE.Mesh`). The status line
reports skeleton count, bone count, root name and the skinned/total mesh split.

Suggested files, spanning the complexity range:

| File | Bones | Meshes | Skinned | Note |
|---|---:|---:|---:|---|
| `chair/model/C011` | 8 | 1 | 1 | simplest skinned case |
| `npc/model/N920` | 59 | 10 | 10 | many sub-meshes, all skinned |
| `ride/model/R880` | 67 | 25 | 8 (5 hidden shapes filtered by §12) | mixed skinned + 17 static |
| `ride/model/R330` | 96 | 10 | 3 | partition-sourced weights (§3) |
| `monster/model/M491` | 148 | 30 | 8 (4 hidden shapes filtered by §12) | additive glow effect layer (§12) |

With **skinning** on and no animation applied, the mesh must look identical to
the Phase 2 static render (minus the helper gizmos removed by §11 and the
hidden geometry removed by §12), and the skeleton overlay must sit inside it.
§6 establishes the bind pose numerically at 5.2e-16 of the model diagonal; the
toggles let it be confirmed by eye. §12 lists specific things to check on
`M491` and `R880`.
