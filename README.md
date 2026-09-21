# GF NIF Exporter

A Windows CLI tool that converts Grand Fantasia `.nif` models (Gamebryo, NIF
20.2.0.8) and their `.kf` animations into a lightweight export format
(`.gfmodel` JSON + `.gfbin` binary buffers) for rendering in Angular/Three.js
with skeletal animation and particle systems.

> **Status: Phase 6 (full CLI pipeline) complete.**
> The exporter converts the whole corpus — geometry, skinning, materials,
> textures, skeletons, animations (embedded and `.kf`), and particle systems —
> in parallel, mirroring the input tree on output. Phase 7 (the Angular loader)
> is a separate project.

**2822 of 2825 `.nif` files convert in ~29 s** on 12 threads (2.07 GB of
output), byte-identically to the sequential exporter. See
[docs/PHASE6_FINDINGS.md](docs/PHASE6_FINDINGS.md).

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
[docs/PHASE1_FINDINGS.md §4](docs/PHASE1_FINDINGS.md). CLI11 is vendored as a
single header in `external/cli11/`, so there are no package-manager
dependencies.

## Usage

```
gfnif-export.exe --input <dir> --output <dir> [options]

  -i, --input <path>    Input root holding the entity directories
                        (monster/, npc/, ride/, effect/, elf/, char/,
                        chair/, item/), or a single .nif
  -o, --output <dir>    Output root; the input tree is mirrored here
      --entities <list> Restrict to some types, comma separated (monster,npc)
  -t, --threads <n>     Worker threads (default: hardware_concurrency)
      --debug           Verbose per-file logging, sequential, limited
      --debug-limit <n> Max files in --debug (default: 10)
      --noverb          Single repainting progress line, full parallelism
      --dry-run         Report what would be done, write nothing
      --overwrite       Re-convert even when the output is newer than the source
      --log-file <path> Detailed log, written in every mode
      --report <path>   JSON run report (stats, failures, slowest files)
      --verify-strips   Cross-check de-striping against niflib (slow)
```

### Common invocations

```sh
# the whole corpus, quiet, with a log and a machine-readable report
build\Release\gfnif-export.exe --input input --output out --noverb ^
    --log-file out\export.log --report out\report.json

# one entity type
build\Release\gfnif-export.exe --input input --output out --entities monster

# see exactly what happens to the first few files
build\Release\gfnif-export.exe --input input --output out --debug --debug-limit 5

# what would a re-run do? (writes nothing)
build\Release\gfnif-export.exe --input input --output out --dry-run

# one model, for diagnosis
build\Release\gfnif-export.exe --input input\monster\model\M011.nif --output out
```

Each `.nif` produces `NAME.gfmodel` (JSON structure) and `NAME.gfbin` (packed
float32/uint32 buffers). The schema is documented in
[docs/PHASE2_FINDINGS.md §7](docs/PHASE2_FINDINGS.md) and the animation and
particle additions in the Phase 3–5 findings.

**Exit codes:** `0` all files converted, `2` some files failed (the run still
completed), `1` the run could not start (bad path, bad option, unwritable
output).

By default a file is skipped when its `.gfmodel` **and** `.gfbin` both exist
and are at least as new as the source; `--overwrite` forces re-conversion.

### Output layout

The input tree is reproduced under `--output`:

```
input/monster/model/M011.nif  ->  out/monster/model/M011.gfmodel
                                  out/monster/model/M011.gfbin
```

Animations are picked up automatically from the sibling
`<type>/animation/NAME.kf`, per
[docs/NAMING_CONVENTIONS.md](docs/NAMING_CONVENTIONS.md).

### `dump` — inspect block structure (phase 1 diagnostic)

```sh
build\Release\gfnif-export.exe dump input\monster\model\M011.nif
build\Release\gfnif-export.exe dump input --summary
```

Output is a NifSkope-style block list followed by the block tree:

```
0 [NiNode] "Scene Root"
|-- 3 [NiTriShape] "M011"
|   |-- 6 [NiMaterialProperty] "M011"
|   `-- 8 [NiSkinInstance]  -> ptr: 0 11 12 17 ...
`-- 46 [NiNode] "Bip01"
```

### Viewing the result

`tools/viewer/` is a throwaway Three.js page for eyeballing exported models. It
is a diagnostic, not part of the Angular deliverable. Serve the repo root so the
`.gfbin` and the input textures are both reachable:

```sh
python -m http.server 8000
# http://localhost:8000/tools/viewer/?model=/out/monster/model/M011.gfmodel
```

## Layout

```
├── CMakeLists.txt
├── external/
│   ├── niflib/              submodule -> niftools/niflib (BSD 3-clause)
│   └── cli11/CLI11.hpp      vendored CLI11 2.4.2 (BSD 3-clause)
├── src/
│   ├── main.cpp             entry point (pipeline / dump)
│   ├── cli/                 argument parsing, progress reporting
│   ├── scanner/             .nif discovery and .kf pairing
│   ├── pipeline/            jobs, thread pool, orchestration, JSON report
│   ├── util/                logging, path helpers
│   ├── export/              SceneData -> .gfmodel + .gfbin
│   ├── nif/                 the extractors (mesh, material, skeleton,
│   │                        animation, particles) + header fix-ups
│   └── texture/             .dds reference -> .png on disk
├── tools/
│   ├── viewer/              throwaway Three.js validation viewer
│   └── tsprobe/             niflib thread-safety probe (see Phase 6 §2)
└── docs/                    PHASE1..6_FINDINGS.md, NAMING_CONVENTIONS.md
```

## Results so far

**Phase 1** — all `.nif`/`.kf` parse; 56 block types catalogued; two GF-specific
header anomalies fixed in memory.

**Phase 2** — static geometry, materials and textures; the `NiPhysXScene`
pre-flight guard that stops niflib crashing the process.

**Phase 3** — skeletons and skinning.

**Phase 4** — animations: embedded, external `.kf`, B-splines resampled to
30 Hz, `XYZ_ROTATION_KEY`.

**Phase 5** — particle systems (`NiParticleSystem`, emitters, modifiers).

**Phase 6** — the parallel pipeline: **6.7× faster (195.6 s → 29.2 s)**,
byte-identical output, deterministic across runs. niflib was found **not**
thread-safe as shipped — its lazy block-type registration is a real data race
(measured: 19 failures and 1 crash in 20 stress runs) — and is made safe by
forcing registration to completion before any worker starts. See
[docs/PHASE6_FINDINGS.md](docs/PHASE6_FINDINGS.md).

The niflib submodule remains unmodified throughout.

## Licence

niflib is included as a submodule under the BSD 3-clause licence
(`external/niflib/license.txt`). CLI11 is vendored under the BSD 3-clause
licence (header preamble in `external/cli11/CLI11.hpp`).
