# GF NIF Exporter

A Windows CLI tool that converts Grand Fantasia `.nif` models (Gamebryo, NIF
20.2.0.8) and their `.kf` animations into a lightweight export format
(`.gfmodel` JSON + `.gfbin` binary buffers) for rendering in Angular/Three.js
with skeletal animation and particle systems.

> **Status: Phase 1 (socle + validation) complete.**
> The executable currently does one thing: dump the block structure of a
> `.nif`/`.kf` so we can verify niflib handles the real game data. The
> conversion pipeline itself starts in Phase 2.

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

```sh
# dump one file
build\Release\gfnif-export.exe input\monster\model\M011.nif

# walk a directory recursively
build\Release\gfnif-export.exe input\monster

# corpus-wide block type tally only
build\Release\gfnif-export.exe input --summary
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

Exit code is `0` when every file parsed, `2` if any failed.

## Layout

```
├── CMakeLists.txt
├── vcpkg.json               manifest (no third-party deps yet; hook for later phases)
├── external/niflib/         submodule -> niftools/niflib (BSD 3-clause)
├── src/
│   ├── main.cpp             CLI entry point
│   └── nif/
│       ├── NifDumper.hpp/.cpp   block dump via niflib, incl. GF header fix-ups
│       └── QhullStub.cpp        replaces the excluded src/nifqhull.cpp
└── docs/
    ├── PHASE1_FINDINGS.md   block inventory, spec divergences, niflib verdict
    └── NAMING_CONVENTIONS.md  input layout, nif/kf pairing, texture resolution
```

## Phase 1 results

All **27** `.nif`/`.kf` files in the test corpus parse, covering **56** distinct
block types. Two GF-specific header anomalies required in-memory fix-ups (a
truncated header string and a mis-stamped version); the niflib submodule is
unmodified. Full detail in [docs/PHASE1_FINDINGS.md](docs/PHASE1_FINDINGS.md).

## Licence

niflib is included as a submodule under the BSD 3-clause licence
(`external/niflib/license.txt`).
