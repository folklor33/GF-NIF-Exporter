# Phase 2 findings — static mesh, materials, textures

**Goal of Phase 2:** convert every `.nif` in the corpus into a `.gfmodel` (JSON)
+ `.gfbin` (binary) pair carrying static bind-pose geometry and materials, then
validate the result visually in a throwaway Three.js viewer.

**Result: 2823 of 2825 `.nif` files convert** (99.93%), producing 6.19M vertices
and 7.28M triangles in ~11 seconds. The two skips are both niflib limitations,
diagnosed below, and both fail gracefully. Texture resolution is **14 865 /
15 151 (98.1%)**.

> **The corpus grew between phases.** Phase 1 ran against 17 `.nif` + 10 `.kf`.
> The input directory now holds **2825 `.nif` + 1475 `.kf`**. Every figure below
> is measured against the current, larger corpus, so it supersedes rather than
> extends the Phase 1 numbers.

---

## 1. Export results

```
EXPORT SUMMARY: 2823/2825 file(s) converted
Vertices          : 6 188 840
Triangles         : 7 278 167
Degenerate dropped: 1 465 495
Geometries skipped: 0
Warnings          : 757
Max node depth    : 28
Textures resolved : 14865/15151 (98%)
```

Output is 2823 `.gfmodel` + 2823 `.gfbin`, 399 MB total, mirroring the input
tree under `out/`.

### Per-entity-type breakdown

| Type    | Models | Texture refs | Resolved | `texture/` dir |
|---------|-------:|-------------:|---------:|----------------|
| chair   |    111 |          976 |      951 | yes (394 png)  |
| char    |      3 |            8 |    **0** | **absent**     |
| effect  |      2 |           17 |       17 | yes (1912 png) |
| elf     |    157 |          176 |    **0** | yes (171 png)  |
| item    |   1222 |         8057 |     8042 | yes (2995 png) |
| monster |    546 |          892 |      854 | yes (1315 png) |
| npc     |    362 |          896 |      876 | yes (606 png)  |
| ride    |    420 |         4129 |     4125 | yes (1640 png) |

---

## 2. The two files that do not convert

### 2.1 `monster/model/M156.nif` — `NiPhysXScene` crashes niflib (worked around)

This file was **taking the whole process down with an access violation**, not
merely failing. It cost the bulk of the debugging time in this phase and is
worth recording carefully.

**Symptom.** A full-corpus run crashed with exit code `0xC0000005` at a
seemingly random file, and the crash point moved between runs. Two other files
reported `bad allocation`. Running any single file in isolation succeeded, which
made it look like accumulated heap corruption or a stack overflow.

It was neither. Running every file in its own process showed exactly one file
failing on its own, every time: `M156.nif`. The apparent randomness was an
artifact of stdout buffering — the last line printed before the crash is not the
file that crashed.

**Cause.** `M156.nif` is the only file in 2825 that contains `NiPhysXScene` and
`NiPhysXSceneDesc` blocks (they are the last 2 of its 137 blocks). niflib
registers ten `NiPhysX*` classes in `src/gen/register.cpp` — `NiPhysXProp`,
`NiPhysXPropDesc`, `NiPhysXActorDesc` and so on — but **`NiPhysXScene` and
`NiPhysXSceneDesc` are not among them; the classes do not exist in niflib.**

`ObjectRegistry::CreateObject` returns `NULL` for an unregistered type, and
`ReadNifList` responds by throwing `runtime_error`. The throw happens with 135
partially linked objects in flight, and the ensuing unwind corrupts memory,
turning a catchable exception into a process-killing access violation. A
`try`/`catch` around `ReadNifList` does not help.

**Fix.** The block-type name table is read straight out of the NIF header —
plain, well-documented layout, no niflib involved — and every type in it is
probed against `ObjectRegistry::CreateObject` *before* the file is handed to
niflib. A file naming a type niflib cannot construct is skipped with a clear
error. See `FindUnsupportedBlockType` in
[`HeaderNormalizer.cpp`](../src/nif/HeaderNormalizer.cpp).

The probe queries niflib's registry rather than hardcoding a blocklist, so it
stays correct automatically if the submodule ever gains the missing classes. It
needs the registry populated, which niflib normally does lazily inside
`ReadNifList`; `RegisterObjects()` is declared locally and called once, since it
is a plain function in namespace `Niflib` and we link statically. Re-registering
later is harmless — it overwrites identical map entries.

After the fix, three consecutive full runs of `monster/model` gave an identical
`546/547` with no crash.

### 2.2 `item/model/WF20.nif` — NIF version 20.3.1.0 unsupported

Header version dword is `0x14030100` = **20.3.1.0**, a version niflib does not
support. It fails cleanly with `bad allocation` (niflib misreads the header and
asks for an absurd allocation) and is skipped. No workaround attempted: it is 1
file in 2825, and guessing at an unknown header layout would risk silently
importing wrong geometry.

---

## 3. Header normalisation: the Phase 1 gate holds

Phase 1 flagged as an open question whether the truncated header string and the
bogus 20.3.0.9 version stamp always co-occur, since the restamp is gated on the
truncation marker and only 1 of 17 files was affected.

**Re-tested across all 2825 `.nif` and 1475 `.kf` files: the gate holds.** No
new truncation signature appeared, and no file needed a restamp without also
carrying the truncated header. The Phase 1 logic moved verbatim into
[`HeaderNormalizer.cpp`](../src/nif/HeaderNormalizer.cpp), shared now by the
dumper and the exporter rather than duplicated.

The marker remains deliberately strict: the header string must be *exactly* the
20-character `"Gamebryo File Format"` with nothing after it. Anything merely
unusual but long enough for niflib's hardcoded `substr(30)` is left alone.

---

## 4. De-striping `NiTriStrips`

Both topologies occur, in comparable numbers: **20 778 `NiTriShape` meshes** and
**21 005 `NiTriStrips` meshes**.

Strips are converted to indexed triangles with the standard walk — triangle *i*
is `(s[i], s[i+1], s[i+2])`, with odd-numbered triangles' winding swapped to
keep facing consistent — and degenerate triangles (any two of the three indices
equal) are dropped.

**Degenerate triangles dropped: 1 465 495**, against 1 677 631 strip triangles
kept — a **46.6% drop rate** within strip geometry.

That ratio is high enough to look like a bug, so it was verified two ways:

1. **Internal invariant.** A strip of length *L* must yield exactly *L−2*
   candidate triangles, each either kept or dropped. Checked on every strip mesh
   in the corpus: **0 violations**.
2. **Cross-check against niflib.** `NiTriStripsData::GetTriangles()` is niflib's
   own independent strip expansion. Its triangle count must equal the number we
   keep. Checked on all 21 005 strip meshes via `--verify-strips`:
   **0 mismatches.**

So the rate is real, not a defect: GF stitches many short strips together with
repeated indices, and a `Box` whose strips expand to 32 candidates correctly
yields the 12 triangles a cube needs. The degenerates are separators, cover zero
area, and are of no use to a renderer.

`--verify-strips` is kept in the tool for re-running this check on new data.

---

## 5. Texture resolution

The Phase 1 rule holds on the full corpus: `NiSourceTexture` stores a **bare
`.dds` filename with no directory component** (verified — 0 of the corpus's
references contain a path separator), and the shipped file is the same basename
with a `.png` extension under `<entityType>/texture/`.

**14 865 of 15 151 references resolve (98.1%). 286 do not.** Every unresolved
reference is a genuine gap in the shipped corpus, not a resolver fault:

* **`char/` (8 refs)** — the entity type ships **no `texture/` directory at
  all**, as already seen in Phase 1.
* **`elf/` (176 refs)** — `elf/texture/` exists and holds 171 PNGs, but they are
  an unrelated set (`F0017.png`, `F0018.png`, …). Models ask for names like
  `X1001H01.png`, and a corpus-wide search confirms **no such file exists
  anywhere under `input/`**.
* **the remainder (~102 refs)** — scattered misses in `chair`, `monster`, `npc`,
  `item`, `ride`, e.g. `C617.nif -> C6170156.dds`.

As required, a miss is never fatal: the material is still exported with
`textureFound: false`, the flat material colors remain usable, and a warning is
logged. The full list is printed by the exporter under `UNRESOLVED TEXTURES`.

---

## 6. Transform flattening

The `NiNode` hierarchy is walked from every root, accumulating
`local * parentWorld` (niflib's row-vector convention), and the resulting world
matrix is baked into positions and normals.

Normals use the upper 3×3 and are renormalised. The inverse-transpose is not
needed: `NiAVObject` stores scale as a **single float**, so non-uniform scale
cannot be expressed in the format, and for rigid + uniform-scale transforms the
plain rotation part is correct.

**Maximum node depth observed across the corpus: 28.** A depth guard of 256 was
added so that a pathologically nested or cyclic file cannot exhaust the stack;
the DAG-sharing guard (a node reachable from two parents) was needed
independently, since without it shared subtrees would be emitted twice.

This flattening is explicitly a Phase 2 measure. Phase 3 will keep skinned
vertices in bind space and carry the transform on the skeleton instead.

---

## 7. Output format

`.gfmodel` is JSON describing the structure; `.gfbin` is one binary blob holding
all attribute data. The split lets a browser fetch the small JSON, decide what
it needs, and pull ranges of the blob straight into typed arrays with no
parsing.

Attributes are stored **de-interleaved** — all positions, then all normals, UVs,
colors, and finally indices — so each range maps directly onto a
`THREE.BufferAttribute` with no stride, and a consumer can skip an attribute it
does not want.

```jsonc
{
  "formatVersion": 1,
  "sourceNif": "input\\monster\\model\\M011.nif",
  "binary": "M011.gfbin",
  "binaryByteLength": 37416,
  "meshes": [{
    "name": "M011", "materialIndex": 0,
    "vertexCount": 589, "indexCount": 2286,
    "sourceTopology": "NiTriShape",       // or "NiTriStrips"
    "degenerateTrianglesDropped": 0,
    "attributes": {
      "position": { "byteOffset": 0, "itemSize": 3, "count": 589, "type": "float32" },
      "normal":   { "byteOffset": 7068, "itemSize": 3, "count": 589, "type": "float32" },
      "uv":       { "byteOffset": 14136, "itemSize": 2, "count": 589, "type": "float32" },
      "color":    { "byteOffset": 18848, "itemSize": 4, "count": 589, "type": "float32" }
    },
    "indices": { "byteOffset": 28272, "count": 2286, "type": "uint32" }
  }],
  "materials": [{
    "diffuseTexturePath": "input/monster/texture/M01101.png",
    "sourceTextureName": "M01101.dds",
    "textureFound": true,
    "hasVertexColor": false,
    "ambient": [...], "diffuse": [...], "specular": [...], "emissive": [...],
    "glossiness": 0, "alpha": 1
  }],

  "skeleton": null,          // phase 3
  "animations": [],          // phase 4
  "particleSystems": []      // phase 5
}
```

The three reserved keys are emitted **empty rather than pre-filled with a guessed
structure**, so the schema keeps its shape without committing to a design those
phases have not made yet.

Indices are widened to `uint32` even though NIF stores `unsigned short`, so the
format does not need a version bump if a later asset exceeds 65 535 vertices.

**Vertex colors** are always present and default to opaque white, so a consumer
can read the attribute unconditionally; `hasVertexColor` on the material says
whether it carries real data (i.e. a `NiVertexColorProperty` was attached).

### Format validation

A random sample of 300 exported models was checked programmatically: `.gfbin`
length matches `binaryByteLength`, every accessor range lies inside the buffer,
every accessor `count` equals `vertexCount`, `indexCount` is a multiple of 3,
no index is out of range, and sampled normals are unit length.
**0 of 300 had any issue.**

---

## 8. Visual validation

`tools/viewer/index.html` is a single-page, throwaway Three.js viewer (CDN, no
build step). Serve the repo root and open it:

```
python -m http.server 8000
http://localhost:8000/tools/viewer/?model=/out/monster/model/M011.gfmodel
```

It rebuilds one `BufferGeometry` per mesh, applies a `MeshStandardMaterial` with
the resolved PNG, and has `OrbitControls` plus wireframe / texture /
vertex-color / Z-up toggles.

Checked on one model from each of the 8 entity types. Two notes worth carrying
forward:

* **Axis convention.** NIF is Z-up, Three.js is Y-up. The exporter deliberately
  **keeps the source axes** — converting is a consumer-side decision, and the
  Phase 3 skeleton must agree with whatever is chosen. The viewer rotates for
  display only.
* **UV orientation.** NIF V runs opposite to the glTF/Three convention, so the
  viewer sets `texture.flipY = false`. Textures land correctly with that.

Materials render with the expected transparency (`chair/C001`, a cocktail glass
of 11 meshes and 9 materials, 6 of them from `NiTriStrips`, shows correct glass
alpha and per-mesh texturing).

---

## 9. Carried into Phase 3

1. **Axis convention must be decided** before the skeleton lands, and applied
   consistently to geometry, bind pose and animation.
2. **Transform flattening is temporary.** Skinned meshes need vertices in bind
   space with the transform carried on the skeleton.
3. **`NiTriStrips` is never skinned** — re-verified on the full corpus, so the
   skinning path in Phase 3 only needs to handle `NiTriShape`.
4. **B-spline compressed animation** remains the main Phase 3/4 risk, unchanged
   from Phase 1.
5. **Missing textures are expected** (286 refs). Whatever consumes `.gfmodel`
   must tolerate `textureFound: false`.

### Reproducing

```
cmake --build build --config Release
build\Release\gfnif-export.exe export input -o out --input-root input
build\Release\gfnif-export.exe export input -o out --input-root input --verify-strips -v
```

---

## 10. Post-hoc fix — alpha / transparency was not extracted

**Found during Phase 3 visual validation, after the helper-geometry fix.**
Once decorative gizmos were removed (see PHASE3_FINDINGS §12), some models
still showed **flat, smooth, untextured-looking surfaces** in the viewer —
but this time `Max helpers dropped: 0` and every texture resolved. These were
real, correctly-textured geometry (capes, veils, wings, hair, glass, effect
planes) rendering **opaque** when they should be transparent.

**Cause: `MaterialExtractor` (Phase 2) never read `NiAlphaProperty`.** It
extracted `NiMaterialProperty`'s colors and its `alpha` (transparency) scalar,
and `NiTexturingProperty`'s diffuse texture, but the property that actually
turns blending or alpha-testing *on* — `NiAlphaProperty` — was not among the
three property types the extractor's loop recognised. `MaterialData.alpha`
therefore reached the `.gfmodel`, but nothing told a consumer whether to *act*
on it, so the Phase 2 viewer's own `opacity`/`transparent` logic never
activated the way GF's source assets intend.

### Measuring before implementing

**1. How common is `NiAlphaProperty`, and how much of it is real transparency?**

```
total shapes (NiTriShape/NiTriStrips) = 41 783
  with NiMaterialProperty              = 41 783  (100% -- every shape has one)
  with NiAlphaProperty                 = 31 822  (76.1%)
files containing >= 1 NiAlphaProperty  = 2 453 / 2 825  (86.8%)

NiMaterialProperty.alpha distribution (41 783 materials):
  < 1.0 (semi-transparent at the material level) = 23 692  (56.7%)
  == 1.0 (opaque)                                = 18 091  (43.3%)
  range observed                                 = [-3.60736, 10]  (see below)
```

`NiAlphaProperty` was already confirmed present in Phase 1's 56-block-type
survey (`NiAlphaProperty` — 29 occurrences in the 17-file sample). At full
corpus scale it turns out to be one of the **most common** property types:
present on more than 3 of every 4 shapes, and on 87% of files.

**A genuine data anomaly, investigated rather than silently worked around:**
228 materials (0.5%) have an out-of-range `alpha` — 227 small negatives (down
to -0.88) and one value of 10. Checking each one: **all 228 occur on a shape
that also carries a `NiAlphaProperty` with blending enabled.** The scalar is
malformed legacy/authoring data on those shapes specifically, not something to
chase further — the shape's actual transparency is controlled by the
`NiAlphaProperty` blend state, and the bogus `NiMaterialProperty.alpha` is
effectively vestigial there. The extractor now clamps it to `[0, 1]` on read
so a consumer reading the field in isolation never sees a negative opacity;
the `NiAlphaProperty` fields are unaffected and remain the authoritative
signal for whether a material blends.

**2. Flag bitfield, decoded against niflib's own generated accessors (which
niflib in turn generates from `nifxml`, the canonical format reference this
project has used throughout):**

```cpp
// external/niflib/include/obj/NiAlphaProperty.h -- comment above `flags`:
// Bit 0        : alpha blending enable
// Bits 1-4     : source blend mode   )  OpenGL glBlendFunc semantics
// Bits 5-8     : destination blend mode )
// Bit 9        : alpha test enable
// Bits 10-12   : alpha test mode        (OpenGL glAlphaFunc semantics)
// Bit 13       : no-sorter flag (disables triangle sorting)
```

niflib exposes this pre-decoded (`GetBlendState`, `GetSourceBlendFunc`,
`GetDestBlendFunc`, `GetTestState`, `GetTestFunc`, `GetTriangleSortMode`), so
the extractor uses those typed accessors directly rather than hand-rolling bit
masks — the risk of an off-by-one in a hand-decoded bitfield is exactly the
kind of bug this measure-first approach exists to avoid.

**Combinations actually seen in the corpus: 44 distinct per-shape (measured
before material de-duplication), collapsing to 12 distinct
`(blend, src, dst, test, testFunc)` tuples across the 14 756 de-duplicated
materials that carry a `NiAlphaProperty`** — out of a 16-bit field that could
in principle encode thousands. As the brief expected, real usage clusters
tightly onto two shapes:

| blend | src | dst | test | func | materials | meaning |
|---|---|---|---|---|---:|---|
| on | `SRC_ALPHA` (6) | `ONE` (0) | off | — | 9 367 | additive (glow/particle-style) |
| off | `SRC_ALPHA` (6) | `ONE_MINUS_SRC_ALPHA` (7) | on | `GREATER` (4) | 2 245 | alpha-tested cutout, no blend |
| on | `SRC_ALPHA` (6) | `ONE` (0) | off | — | 1 794 | additive, threshold unused |
| on | `SRC_ALPHA` (6) | `ONE_MINUS_SRC_ALPHA` (7) | off | — | 1 067 | standard "over" alpha blend |
| on | `SRC_ALPHA` (6) | `ONE_MINUS_SRC_ALPHA` (7) | on | `GREATER` (4) | 130 | blend + test together |
| *(6 further combos)* | | | | | 153 | one blend/test/threshold variant each |

`srcBlendMode` is `SRC_ALPHA` (6) in **every single** blend-enabled material
in the corpus — no other source factor occurs anywhere. `alphaTestFunc` is
`GREATER` (4) whenever testing is enabled at all, `ALWAYS` (0) (i.e.
unused) otherwise; total blend-enabled materials: 12 498 (`dst=ONE` additive:
11 181; `dst=ONE_MINUS_SRC_ALPHA` standard blend: 1 316; one `dst=8` outlier).
This is exactly the "very few distinct combinations" the brief predicted, and
none of it is exotic: it is the standard OpenGL-style alpha blend / additive /
alpha-cutout trio GF's Gamebryo-era pipeline would be expected to use.

**3. PNG alpha channel — confirmed intact, ruling out an upstream problem.**
Sampled 280 PNGs across all 7 texture-bearing entity types (`char/` ships no
`texture/`, per Phase 1 §4):

```
mode: 100% RGBA (0 non-RGBA, 0 missing an alpha channel entirely)
alpha channel carries real (non-uniform) data: 221 / 280  (78.9%)
alpha channel present but fully opaque (255 everywhere):   59 / 280  (21.1%)
```

The DDS→PNG conversion upstream of this project preserved the alpha channel
correctly on every sample, including `effect/` textures specifically (5/5
sampled show a full 0–255 alpha range). **The problem was entirely in the
exporter, not the source assets** — exactly what the measurement in step 4 of
the brief was meant to rule in or out.

### Extending `MaterialData`

```cpp
struct MaterialData {
    // ... Phase 2 fields unchanged (diffuseTexturePath, ambient, diffuse,
    //     specular, emissive, glossiness, hasVertexColor, textureFound) ...

    float alpha = 1.0f;              // NiMaterialProperty transparency, clamped [0,1]

    bool    hasAlphaProperty = false;  // false => opaque, exactly Phase 2's behaviour
    bool    alphaBlendEnabled = false;
    uint8_t srcBlendMode = 0;          // NiAlphaProperty::BlendFunc, raw NIF value
    uint8_t dstBlendMode = 0;
    bool    alphaTestEnabled = false;
    uint8_t alphaTestFunc = 0;         // NiAlphaProperty::TestFunc, raw NIF value
    uint8_t alphaTestThreshold = 0;    // 0-255
};
```

Blend and test modes are stored as **NIF's own raw enum values**, not
translated to a render engine's constants — the same principle already
applied to the Z-up axis convention (PHASE2_FINDINGS §6, §9): the export
format stays faithful to the source, and translation to whatever a consumer
actually renders with happens in that consumer's loader, not in the exporter.

`hasAlphaProperty = false` (no `NiAlphaProperty` attached) leaves every other
field at its default and is the exact Phase 2 behaviour — no regression for
the 13.2% of files that carry none.

### Viewer

`tools/viewer/index.html` maps the exported fields onto
`THREE.MeshStandardMaterial`:

* `opacity` = `alpha`; `transparent = true` and **`depthWrite = false`** the
  moment either `alphaBlendEnabled` or `alpha < 1`. This is the specific fix
  for the reported symptom: a blended surface that still writes depth hides
  everything behind it exactly like an opaque one, which is indistinguishable
  from the "flat parasitic surface" the fix was chasing.
* `alphaTest = alphaTestThreshold / 255` when `alphaTestEnabled`, and
  `depthWrite` stays **`true`** for a test-only material (no blend) — cutout
  geometry (foliage, hair cards) discards pixels outright, so what remains is
  genuinely opaque and should still occlude correctly. Getting this backwards
  would have been a different, subtler bug than the one being fixed.
* Texture loading was already alpha-correct: `THREE.TextureLoader` decodes a
  PNG's alpha channel natively, and nothing in the viewer forces an RGB-only
  format.
* Known limitation, not addressed here: Three.js sorts transparent objects by
  distance but does not sort *within* one mesh's own triangles, so
  self-overlapping transparent geometry (e.g. a cape's front and back faces)
  can still show ordering artifacts. This is a general transparency-rendering
  limitation, not an export defect, and is left as-is per the brief.

### Validation

* **Simulated the viewer's exact material logic in Python** against 400
  random exported materials (same technique as the Phase 3 bind-pose check):
  **0 violations** of "blend ⇒ depthWrite=false" (1473 cases) or "test-only ⇒
  depthWrite=true" (309 cases), and opaque/no-property materials correctly
  produce no special flags at all (497 cases).
* **Format validation**: 500 random models checked for the new fields being
  present, `alpha` in `[0,1]`, and `hasAlphaProperty = false` materials having
  every dependent field at its default. **0 issues.**
* **Generality, not a single-file fix**: blend-enabled materials appear in
  **all 8 entity types**, concentrated exactly where expected —
  `effect/` 2/2 files (100%), `item/` 762/1222 (62%, capes/cloth/glass),
  `ride/` 327/420 (78%), `chair/` 81/111 (73%, the Phase 2 "cocktail glass"
  file `C001` among them, now carrying real 0.5/0.7/0.8 opacity values rather
  than the flat opaque the old extractor produced), `npc/` 81/362.
* **Non-regression, corpus totals**: re-ran the full export.
  `Vertices: 5 883 591`, `Triangles: 7 131 151`, `Max helpers dropped: 8389` —
  **identical to the post-helper-filter Phase 3 numbers** (this fix touches
  `MaterialExtractor` only; no geometry is added, removed, or moved). The
  `GFNIF_VERIFYSKIN` ground-truth check is likewise unchanged: 6603 verified,
  same 5 known exceptions as Phase 3 §3/§11 — skinning does not read
  materials, so it cannot have been affected, and re-running it confirms that.
* No headless browser was available in this environment (same constraint
  noted in PHASE3_FINDINGS §11), so the "confirm in the viewer" step could not
  be captured as a screenshot. The Python simulation above replicates the
  viewer's material-construction logic line-for-line against the real
  exported JSON, which is the same substitute used for the Phase 3 bind-pose
  proof; a manual check in a browser is still recommended before sign-off.

### Carried forward

Nothing here changes the Phase 3 skeleton/skinning path — materials and
skinning are extracted independently, and the numbers above confirm neither
geometry nor skinning moved. `NiTexturingProperty` alpha maps / multi-texture
blending (a texture's *own* alpha channel driving a separate blend mode, as
opposed to the material-level `NiAlphaProperty` this fix covers) was not
investigated and is out of scope here, same as the brief's boundary.

---

## 11. Correctif — residual fur halo on `ride/R814`, diagnosed as an export-correct but open rendering question for Phase 7

**Reported symptom:** on `ride/R814` (a mount with fur), a faint opaque halo
appears around fur silhouettes — different from the M491 "large opaque
panel" defect this section originally fixed: not a whole surface wrongly
opaque, but a thin fringe of wrongly-opaque pixels at the edge of an
otherwise-transparent cutout region.

**Cause, confirmed by dumping R814's own exported material data (no new
diagnostic tool needed — the existing `.gfmodel` export already has every
field to check):** R814's fur/body material (`materialIndex 0`, texture
`R81401.png`) is **alpha-test-only, not blended** —
`alphaBlendEnabled: false, alphaTestEnabled: true, alphaTestFunc: 4
(GREATER), alphaTestThreshold: 125`. Per this section's own viewer design
(§"Viewer" above), that combination is correctly treated as a hard cutout:
`transparent` stays unset, `depthWrite: true`, `alphaTest: 125/255 ≈ 0.49`.
A hard cutout is binary by construction — a texel at alpha 124 vanishes
entirely, one at 126 renders **fully opaque** — so any texture with a
*progressive* alpha gradient at its cutout edge (exactly what a fur/hair
silhouette needs to look soft) will show a thin ring of fully-opaque pixels
right at the threshold, which is the reported halo.

**Measured, not assumed — `R81401.png`'s own alpha channel (1024×1024,
Pillow):** 256 distinct alpha values present (a genuine progressive
gradient, ruling out a flattened/quantized PNG — see point 4 of the
diagnostic brief), 95.1% fully opaque, 4.1% fully transparent, and 1826
pixels (0.17%) sitting in the 100–150 band that straddles the 125
threshold — this is the fringe that becomes the halo.

**Non-isolated: the exact same alpha state exists on the reference file,
M491, and is not a difference between the two files.** M491's own body
material (`materialIndex 0`, texture `M49101.png`) has **identically**
`alphaBlendEnabled: false, alphaTestEnabled: true, alphaTestFunc: 4,
alphaTestThreshold: 125` — the same signature, same viewer code path. Its
texture's own alpha histogram (same method) shows an even larger soft-edge
population (3405 pixels in the 100–150 band) and a bigger transparent
region (20.6% vs R814's 4.1%). M491 was the Phase 2 validation case for the
*blend* fix (§8) — a different material on that file — and was never
checked for this alpha-test artifact specifically, so "M491 doesn't show it"
was an assumption, not a prior measurement; the data says the artifact's
raw ingredients (binary threshold across a soft gradient) are present on
M491 too, just on a smaller/less visually salient region (a small cutout
detail rather than the large fur silhouette R814's whole body has).

**Corpus-wide prevalence, full corpus (2829 exported `.gfmodel` files,
scanned directly — no re-export needed):**

```
Files with >=1 alpha-test-only material (blend off, test on): 2197 / 2829  (77.7%)
Alpha-test-only materials total                              : 2243 / 15217
  of which func=GREATER(4), threshold=125 (R814/M491's exact signature): 2144
Materials with BOTH blend and test enabled                    : 149
  (handled correctly per §"Viewer": transparent=true, depthWrite=false,
   alphaTest set -- the blend branch runs first and test's `if (!blend)`
   guard correctly does not re-enable depthWrite)
```

**This is not an R814-specific defect and not rare: over three-quarters of
the corpus's files carry at least one alpha-test-only material, and the
overwhelming majority share the exact threshold/func R814 and M491 use** —
almost certainly a single shared authoring convention across GF's
Gamebryo-era pipeline (confirms `alphaTestFunc`/`threshold` decoding again
from a second, independent file, on top of §2's original 12-tuple survey).

**Where the boundary from the diagnostic brief lands, per point by point:**

1. **Alpha state correctly extracted** — confirmed identical on R814 and
   M491, both dumped directly from the export with no ambiguity.
2. **Viewer applies the extracted values faithfully**, including the
   blend+test-together case (149 materials, handled correctly, see above) —
   no bug found in `buildStandardMaterial`'s branching.
3. **Threshold conversion (`/255`) and comparison direction are correct**:
   Three.js's `alphaTest` keeps a pixel when `alpha > threshold` (GREATER),
   which is exactly `NiAlphaProperty::TestFunc` 4 (`GREATER`) — the only
   `alphaTestFunc` value observed anywhere in the corpus when testing is
   enabled at all (§2), so there is no NIF test-function this exporter
   would render backwards.
4. **PNG alpha channel is a genuine progressive gradient** (256 distinct
   values), not a flattened/quantized one — the DDS→PNG conversion is not
   the cause.
5. **Root cause is `alphaTest`'s binary nature itself** (point 5 of the
   brief): a hard threshold cannot reproduce a soft edge from a continuous
   alpha gradient. This is a real constraint of that *specific* rendering
   technique — but it is not the only technique available, and the
   original game renders this same fur without a halo, so a correct
   render is possible. **Not a closed limitation — see below.**

**Decision: export is correct and unchanged — this is an OPEN QUESTION for
Phase 7, not a closed "nothing to do".** The exported alpha data is
faithful to the NIF (`alphaBlendEnabled`/`alphaTestEnabled`/`alphaTestFunc`/
`alphaTestThreshold`, all confirmed correct above) and needs no change —
that much *is* settled, per this project's export/viewer boundary
(§"Perimetre" of the brief). What is **not** settled is how a consumer
should render an alpha-test-only material to avoid this halo. `alphaTest`
alone cannot do it, but nothing requires a consumer to translate NIF
"alpha test" into Three.js `alphaTest` specifically — the project's own
standing principle (§6, §9, and this section's own "Viewer" paragraph) is
that the export stays faithful to the source and the *translation* to a
render technique is the loader's job, which leaves room to choose a
different technique for this case. Three directions, **none evaluated in
this session**, that Phase 7 should assess before assuming the halo is
unavoidable:

1. **Render alpha-test-only materials as blended instead of using
   `alphaTest`.** The NIF says "alpha test", but the consumer is not
   obligated to reproduce that mechanism literally — a blended render of
   the same texture would reproduce the soft gradient this data already
   has (confirmed: 256 distinct alpha values, not a flattened mask). This
   is the most direct fix and the most consistent with the project's own
   translation-happens-in-the-loader principle.
2. **`alphaToCoverage`** (mentioned in the original diagnostic brief,
   not evaluated in this session) — an MSAA-based technique that dithers
   the cutout across sample points instead of a hard per-pixel threshold,
   which could soften the edge while keeping the depth/sort behavior of a
   true cutout (relevant for fur, which self-overlaps — see this
   section's existing "Known limitation" note on transparent sort order).
3. **A custom shader**, if neither of the above proves sufficient — the
   fallback if a soft edge needs behavior neither of the built-in Three.js
   techniques provides.

**This is a structural question, not an edge case: 2197 files (77.7% of
the 2829-file corpus) carry at least one alpha-test-only material**, and
2144 of those share R814/M491's exact threshold/func signature (§ above).
Whatever Phase 7 decides here will visibly affect more than three
quarters of the corpus, not just R814's fur — this is the reason to treat
it as a real open question rather than a one-off cosmetic note.

**Why this was not resolved in Phase 5 itself:** `tools/viewer/index.html`
is explicitly a throwaway validation tool (its own file header says so),
built across Phases 2–4 to eyeball exported data, not the real rendering
path. The real consumer is the Angular/Three.js loader Phase 7 builds, so
picking and implementing a rendering technique for this case belongs
there, not as a patch to the diagnostic viewer.

**Non-regression:** no code changed in this correctif (measurement/dumping
only, using the existing export + Pillow), so M491 and every other
previously-validated file are unaffected by construction. Re-confirmed the
blend-only fix from §8-§10 above is untouched: M491's blend-enabled
materials (indices 1–7) all still show `alphaBlendEnabled: true`,
`alphaTestEnabled: false` in this session's dump, matching the original
fix's intent.
