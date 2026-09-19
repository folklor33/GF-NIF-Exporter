# Phase 1 findings — niflib validation against the Grand Fantasia corpus

**Goal of Phase 1:** stand up the build (CMake + vcpkg manifest + niflib
submodule), and prove that niflib can parse the real GF data before any exporter
code is written on top of it.

**Result: all 27 `.nif`/`.kf` files in the test corpus parse successfully**
(`gfnif-export input --summary` exits 0), but only after two in-memory header
fix-ups described in §3. Both are small, well understood, and live in our code —
the niflib submodule is unmodified.

---

## 1. Corpus overview

`input/` holds 17 `.nif`, 10 `.kf` and 9033 `.png` across 8 entity types
(`chair`, `char`, `effect`, `elf`, `item`, `monster`, `npc`, `ride`). Layout and
the model↔animation pairing rule are documented separately in
[NAMING_CONVENTIONS.md](NAMING_CONVENTIONS.md).

### Per-file breakdown

| File | Version | Blocks | Skin | TriShape | TriStrips | ParticleSys |
|------|---------|-------:|-----:|---------:|----------:|------------:|
| chair/C011.nif    | 20.2.0.8 |  23 |  1 |  1 |  0 | 0 |
| chair/C027.nif    | 20.2.0.8 | 229 |  2 |  2 |  6 | 8 |
| char/B2A02.nif    | 20.2.0.8 |  10 |  0 |  0 |  1 | 0 |
| char/B2A15.nif    | 20.2.0.8 |  16 |  0 |  0 |  3 | 0 |
| char/X6AQ9.nif    | 20.2.0.8 | 249 |  7 | 14 |  0 | 4 |
| effect/S13103.nif | 20.2.0.8 | 232 |  0 |  0 |  9 | 2 |
| effect/S14141.nif | 20.2.0.8 | 325 |  0 |  0 | 16 | 2 |
| elf/X1006.nif     | 20.2.0.8 |   8 |  0 |  0 |  1 | 0 |
| elf/X1016.nif     | 20.2.0.8 | 389 |  0 |  0 | 72 | 0 |
| item/W667.nif     | 20.2.0.8 | 319 | 21 | 21 | 13 | 3 |
| item/WA85.nif     | 20.2.0.8 |  72 |  0 |  5 |  0 | 1 |
| monster/M011.nif  | 20.2.0.8 |  56 |  1 |  1 |  0 | 0 |
| monster/M903.nif  | 20.2.0.8\* | 145 |  1 |  1 | 11 | 2 |
| npc/N009.nif      | 20.2.0.8 |  69 |  1 |  1 |  1 | 0 |
| npc/N920.nif      | 20.2.0.8 | 126 | 10 | 10 |  0 | 0 |
| ride/R834.nif     | 20.2.0.8 | 145 |  2 |  2 |  0 | 4 |
| ride/R880.nif     | 20.2.0.8 | 321 |  8 | 25 |  0 | 5 |

\* `M903.nif` is stamped 20.3.0.9 on disk; see §3.2.

### `.kf` breakdown

| File | Version | Clips | B-spline interp. | Transform interp. |
|------|---------|------:|-----------------:|------------------:|
| chair/C011.kf   | 20.3.0.9 |  1 |    0 |   3 |
| chair/C027.kf   | 20.2.0.8 |  2 |    0 |   6 |
| monster/M011.kf | 20.3.0.9 |  9 |  231 |  66 |
| monster/M903.kf | 20.3.0.9 | 23 |  649 | 455 |
| npc/N009.kf     | 20.3.0.9 |  2 |   44 |  32 |
| npc/N920.kf     | 20.2.0.8 |  2 |   51 |  51 |
| ride/R834.kf    | 20.3.0.9 | 86 | 2513 | 325 |
| ride/R880.kf    | 20.3.0.9 | 87 | 2152 | 719 |
| item/W667.kf    | 20.2.0.8 |  4 |    9 | 103 |
| char/X6AQ9.kf   | 20.2.0.8 |  4 |    5 | 123 |

"Clips" = `NiControllerSequence` count, which equals `NiTextKeyExtraData` count
in every file.

---

## 2. Block types encountered (56 distinct)

All block types the brief asked about were found and parsed. Counts are corpus
totals across all 27 files.

### Scene graph & geometry
| Count | Type |
|------:|------|
| 490 | `NiNode` |
|  39 | `NiBillboardNode` |
|  83 | `NiTriShape` |
|  83 | `NiTriShapeData` |
| 133 | `NiTriStrips` |
| 133 | `NiTriStripsData` |
|   1 | `NiCollisionData` |

### Skinning
| Count | Type |
|------:|------|
| 54 | `NiSkinInstance` |
| 54 | `NiSkinData` |
| 54 | `NiSkinPartition` |

### Materials & textures
| Count | Type |
|------:|------|
| 114 | `NiMaterialProperty` |
|  85 | `NiTexturingProperty` |
|  69 | `NiSourceTexture` |
|  29 | `NiAlphaProperty` |
|  26 | `NiZBufferProperty` |
|  25 | `NiVertexColorProperty` |
|   5 | `NiStencilProperty` |

### Animation
| Count | Type |
|------:|------|
| 220 | `NiControllerSequence` |
| 221 | `NiTextKeyExtraData` |
| 1966 | `NiTransformInterpolator` |
| 1535 | `NiTransformData` |
|  93 | `NiTransformController` |
|  10 | `NiMultiTargetTransformController` |
| 5654 | `NiBSplineCompTransformInterpolator` |
|  27 | `NiBSplineCompFloatInterpolator` |
| 214 | `NiBSplineData` |
| 214 | `NiBSplineBasisData` |
| 866 | `NiFloatInterpolator` |
| 804 | `NiFloatData` |
| 516 | `NiBoolInterpolator` |
|  34 | `NiBoolData` |
|  10 | `NiPoint3Interpolator` |
|  10 | `NiPosData` |
|  27 | `NiColorData` |
|  25 | `NiAlphaController` |
|  10 | `NiMaterialColorController` |
|  54 | `NiTextureTransformController` |
|   1 | `NiGeomMorpherController` |
|   1 | `NiMorphData` |

### Particles
| Count | Type |
|------:|------|
| 31 | `NiParticleSystem` |
| 31 | `NiPSysData` |
| 31 | `NiPSysUpdateCtlr` |
| 31 | `NiPSysEmitterCtlr` |
| 31 | `NiPSysAgeDeathModifier` |
| 31 | `NiPSysBoundUpdateModifier` |
| 31 | `NiPSysPositionModifier` |
| 31 | `NiPSysSpawnModifier` |
| 27 | `NiPSysMeshEmitter` |
| 27 | `NiPSysColorModifier` |
| 24 | `NiPSysRotationModifier` |
| 19 | `NiPSysGrowFadeModifier` |
|  8 | `NiPSysEmitterSpeedCtlr` |
|  6 | `NiPSysEmitterLifeSpanCtlr` |
|  4 | `NiPSysBoxEmitter` |
|  3 | `NiPSysGravityModifier` |

### Misc
| Count | Type |
|------:|------|
| 260 | `NiStringExtraData` |

**Not present in this corpus:** no `bhk*` Havok collision blocks (beyond a
single `NiCollisionData`), no `BS*` Bethesda extensions, no `NiLODNode`,
no `NiPixelData` (textures are always external files).

---

## 3. Anomalies and divergences from the standard NIF spec

### 3.1 Truncated header string (blocking, fixed)

`monster/model/M903.nif` begins with the bare header string

```
Gamebryo File Format\n
```

instead of the `Gamebryo File Format, Version 20.2.0.8\n` form every other file
uses. This **crashes niflib**:
`NifStream(HeaderString&, ...)` in `src/NIF_IO.cpp:395-405` matches the
20-character `"Gamebryo File Format"` prefix, hard-codes `ver_start = 30`, then
calls `header.substr(30)` on a 20-character string. `std::string::substr` throws
`std::out_of_range("invalid string position")`, which surfaced as the original
parse failure.

The version dword after the string is intact, so the fix in
[`NifDumper.cpp`](../src/nif/NifDumper.cpp) reads it and splices a well-formed
`", Version x.x.x.x"` suffix into an in-memory copy of the file before handing
it to niflib. **The file on disk is never modified.**

### 3.2 Version stamped 20.3.0.9 with a 20.2.0.8 header layout (blocking, fixed)

The same file additionally declares version **20.3.0.9** (`0x14030009`) while
its header is physically laid out as 20.2.0.8. Per `nifxml`, two extra header
fields appear above 20.2.0.7:

* a per-block size array — `version >= 0x14020007` (`Header.cpp:104`)
* a string table (`numStrings`, `maxStringLength`) — `version >= 0x14010003`
  (`Header.cpp:110`)

Neither is present in `M903.nif`. Byte-level comparison against `M011.nif`
confirms the two headers are structurally identical after the version dword:
both continue straight into `numBlockTypes` and the block-type name table.

The fix restamps the in-memory copy to 20.2.0.8 so niflib selects the layout the
bytes actually use.

**This restamp is deliberately gated on the truncated-header marker from §3.1.**
The corpus also contains **genuine** 20.3.0.9 `.kf` files (C011, M011, M903,
N009, R834, R880) that *do* carry the string table and parse correctly as-is.
An earlier, ungated version of this fix-up corrupted all six of them. Only files
carrying the truncated header are touched.

> **Open question for Phase 2:** only 1 of 17 `.nif` files is affected. If the
> full production dataset contains many such files, it is worth confirming that
> the truncated header always co-occurs with the bogus 20.3.0.9 stamp. If the
> two ever appear independently, the gate in `NifDumper.cpp` needs revisiting.

### 3.3 Mixed versions between a model and its animation

A `.nif` and its paired `.kf` do **not** always share a version. `monster/M011`
pairs a 20.2.0.8 model with a 20.3.0.9 animation; `npc/N920` is 20.2.0.8 on both
sides. The exporter must read the version per file and never assume the pair
matches.

### 3.4 Textures referenced as `.dds`, shipped as `.png`

`NiSourceTexture` stores a bare `.dds` filename with no directory. The corpus
ships `.png`. 54 of 66 references resolve by swapping the extension and looking
in `<entity type>/texture/`; the 12 that do not are corpus gaps (`char/` has no
`texture/` directory at all). Details in
[NAMING_CONVENTIONS.md §4](NAMING_CONVENTIONS.md).

**Phase 2 must treat a missing texture as a warning, not a fatal error.**

### 3.5 Geometry is split between two representations

Both `NiTriShape` (83) and `NiTriStrips` (133) occur, sometimes in the same
file. The exporter needs both paths: `NiTriStripsData` stores triangle *strips*
that must be de-stripified into a plain index buffer for Three.js.

Useful simplification, verified programmatically across all 17 models:
**no `NiTriStrips` is ever skinned.** Every `NiSkinInstance` hangs off a
`NiTriShape`. Skinning support in Phase 2 therefore only has to cover
`NiTriShape`, though this should be re-checked against the full dataset.

### 3.6 B-spline compressed animation dominates

`NiBSplineCompTransformInterpolator` is by far the most common animation block
(5654 occurrences, vs 1966 plain `NiTransformInterpolator`). The two `ride`
files alone carry ~4600.

This is the single biggest technical risk for Phase 3. These interpolators do
not store keyframes directly; they store quantised B-spline control points in
`NiBSplineData`, with the basis in `NiBSplineBasisData`. Exporting to
Three.js requires **sampling** the B-spline at fixed intervals rather than
copying keyframes across. niflib does expose the control-point data, so this is
a matter of implementing the evaluation, not of parsing.

---

## 4. niflib build compatibility

niflib (last commit `e291661`, Sept 2021) does **not** compile as C++17. Three
incompatibilities appeared, all in the library itself:

1. **`std::byte` ambiguity.** niflib headers do `using namespace std;`, so its
   own `Niflib::byte` typedef collides with C++17's `std::byte`
   (errors C2872 / C2556 / C2371 across ~10 files).
2. **`std::mem_fun_ref`** — removed in C++17, used in
   `TriStripper/detail/graph_array.h:447` (C2039).
3. **`std::binary_function`** — removed in C++17, used in
   `src/obj/NiSkinPartition.cpp:865,876` (C2504).

**Resolution — no submodule patch needed.** niflib is built as its own static
library pinned to **C++14** with `/permissive`, while our code stays on C++17
with `/permissive-`. The two link together without issue; nothing in niflib's
public headers exposes a C++17-only type. See the comments in
[`CMakeLists.txt`](../CMakeLists.txt).

### qhull excluded

`src/nifqhull.cpp` textually `#include`s the C sources of niflib's bundled
`qhull` git submodule. That submodule is **empty** in this checkout and its
recorded URL uses the `git://` protocol GitHub no longer serves, so it cannot be
fetched.

qhull is reachable only from `Inertia.cpp`'s Havok mass-property computation for
collision shapes that supply vertices but no triangles — code this exporter
never invokes. `nifqhull.cpp` is therefore dropped from the build and replaced
by [`src/nif/QhullStub.cpp`](../src/nif/QhullStub.cpp), which asserts in debug
builds if the path is ever reached.

Removing `Inertia.cpp` outright was rejected: 16 `bhk*` classes reference it, so
excluding it would cascade into a much larger set of edits.

---

## 5. Verdict: keep niflib, do not patch it

**Recommendation: proceed to Phase 2 with niflib as-is.**

* All 56 block types in the corpus parse correctly, including the full skinning,
  particle and animation sets the exporter needs.
* Both blocking defects are in *file headers*, not in block parsing, and are
  fixed by ~20 lines in our own code. The submodule stays pristine and can be
  updated from upstream freely.
* The C++17 incompatibility is resolved by a per-target language level, not by
  editing niflib.

Two items to watch as the dataset grows from 17 files to tens of thousands:

1. **Header anomaly frequency (§3.2).** Verify the truncated header and the
   bogus 20.3.0.9 stamp always co-occur.
2. **B-spline evaluation (§3.6).** The main implementation effort in Phase 3.

### Reproducing these results

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\gfnif-export.exe input --summary
```

Expected: `CORPUS SUMMARY: 27/27 file(s) parsed`, exit code 0.
