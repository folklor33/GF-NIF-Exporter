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
