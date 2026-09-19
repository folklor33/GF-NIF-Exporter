# GF NIF Exporter

A Windows CLI tool that converts Grand Fantasia `.nif` models (Gamebryo, NIF
20.2.0.8) and their `.kf` animations into a lightweight export format
(`.gfmodel` JSON + `.gfbin` binary buffers) for rendering in Angular/Three.js
with skeletal animation and particle systems.

> **Status: Phase 2 (static mesh, materials, textures) complete.**
> The exporter converts a `.nif`'s static bind-pose geometry and materials into
> `.gfmodel` + `.gfbin`. Skeleton, animations and particle systems arrive in
> phases 3 to 5; the multi-file scanner and parallel pipeline in phase 6.

## Requirements

* Windows, Visual Studio 2022 (the bundled CMake 3.31 is sufficient)
* Git (for the niflib submodule)

CMake is not required on `PATH`; the copy shipped with Visual Studio works:

```
C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin
```

## Building

```sh
git submodule update --init --recursive
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The `qhull` submodule inside niflib is intentionally **not** required — see
[docs/PHASE1_FINDINGS.md §4](docs/PHASE1_FINDINGS.md).

## Usage

### `export` — convert models (phase 2)

```sh
# one file
build\Release\gfnif-export.exe export input\monster\model\M011.nif -o out

# the whole corpus, mirroring the input tree under out\
build\Release\gfnif-export.exe export input -o out --input-root input

# list every warning, and cross-check de-striping against niflib
build\Release\gfnif-export.exe export input -o out --input-root input -v --verify-strips
```

Each `.nif` produces `NAME.gfmodel` (JSON structure) and `NAME.gfbin` (packed
float32/uint32 buffers). The schema is documented in
[docs/PHASE2_FINDINGS.md §7](docs/PHASE2_FINDINGS.md).

Exit code is `0` when every file converted, `2` if any failed.

### `dump` — inspect block structure (phase 1 diagnostic)

```sh
# dump one file
build\Release\gfnif-export.exe dump input\monster\model\M011.nif

# walk a directory recursively
build\Release\gfnif-export.exe dump input\monster

# corpus-wide block type tally only
build\Release\gfnif-export.exe dump input --summary
```

Output is a NifSkope-style block list followed by the block tree:

```
0 [NiNode] "Scene Root"
|-- 3 [NiTriShape] "M011"
|   |-- 6 [NiMaterialProperty] "M011"
|   |-- 4 [NiTexturingProperty]
|   |   `-- 5 [NiSourceTexture]
|   `-- 8 [NiSkinInstance]  -> ptr: 0 11 12 17 ...
|       |-- 9 [NiSkinData]
|       `-- 10 [NiSkinPartition]
`-- 46 [NiNode] "Bip01"
```

`-> ptr:` lists non-owning back-references (bone lists, controller targets),
which are shown but not descended into. Blocks reached more than once are marked
`(see above)` rather than printed twice.

### Viewing the result

`tools/viewer/` is a throwaway Three.js page for eyeballing exported models. It
is a diagnostic, not part of the Angular deliverable. Serve the repo root so the
`.gfbin` and the input textures are both reachable:

```sh
python -m http.server 8000
# then open:
# http://localhost:8000/tools/viewer/?model=/out/monster/model/M011.gfmodel
```

## Layout

```
├── CMakeLists.txt
├── vcpkg.json               manifest (no third-party deps yet; hook for later phases)
├── external/niflib/         submodule -> niftools/niflib (BSD 3-clause)
├── src/
│   ├── main.cpp             CLI entry point (dump / export)
│   ├── export/
│   │   ├── SceneModel.hpp       niflib-free intermediate representation
│   │   └── GfxFormatWriter.*    SceneData -> .gfmodel + .gfbin
│   ├── nif/
│   │   ├── HeaderNormalizer.*   GF header fix-ups + unsupported-block guard
│   │   ├── MeshExtractor.*      NiTriShape/NiTriStrips -> MeshData
│   │   ├── MaterialExtractor.*  NiMaterialProperty/NiTexturingProperty -> MaterialData
│   │   ├── NifDumper.*          block dump (phase 1 diagnostic)
│   │   └── QhullStub.cpp        replaces the excluded src/nifqhull.cpp
│   └── texture/
│       └── TextureResolver.*    .dds reference -> .png on disk
├── tools/viewer/            throwaway Three.js validation viewer
└── docs/
    ├── PHASE1_FINDINGS.md   block inventory, spec divergences, niflib verdict
    ├── PHASE2_FINDINGS.md   export results, de-striping, texture resolution
    └── NAMING_CONVENTIONS.md  input layout, nif/kf pairing, texture resolution
```

## Results so far

**Phase 1** — all `.nif`/`.kf` files parse; 56 distinct block types catalogued.
Two GF-specific header anomalies needed in-memory fix-ups (truncated header
string, mis-stamped version). See [docs/PHASE1_FINDINGS.md](docs/PHASE1_FINDINGS.md).

**Phase 2** — **2823 of 2825** `.nif` files convert (6.19M vertices, 7.28M
triangles, ~11 s), with **98.1%** of texture references resolved. The two skips
are niflib limitations: one file uses `NiPhysXScene`, which niflib does not
implement and which crashed the process until a pre-flight block-type check was
added; another uses unsupported NIF version 20.3.1.0. See
[docs/PHASE2_FINDINGS.md](docs/PHASE2_FINDINGS.md).

The niflib submodule remains unmodified throughout.

## Licence

niflib is included as a submodule under the BSD 3-clause licence
(`external/niflib/license.txt`).
