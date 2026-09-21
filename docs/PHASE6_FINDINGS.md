# Phase 6 findings — CLI pipeline, parallelism and performance

**Goal of Phase 6:** turn the per-file diagnostic tool into the single
executable described in the initial plan — walk an entity tree, convert
everything in parallel, mirror the tree on output, and report properly. This
phase changes **how** the corpus is exported, never **what** is exported.

**Result: the full corpus converts in 29.2 s instead of 195.6 s (6.7×) on 12
threads, and the 5644 output files are byte-identical to those produced by the
sequential Phase 5 tool.** Two successive parallel runs are also byte-identical
to each other. niflib turned out **not** to be thread-safe as shipped: its lazy
block-type registration is a genuine data race, measured at 19 failures and
1 crash in 20 stress runs, and closing it was a precondition for this phase
(§2). The single biggest cost in the corpus is not geometry but **28 malformed
`.kf` files that consume 39% of the total work to produce nothing** (§5).

---

## 1. Baseline, measured before any change

The brief quoted "about a dozen seconds for ~400 MB". That figure predates
animations. Re-measured at commit `e985a2c`, sequential, 12-core machine:

| | |
|---|---|
| Wall time, full corpus | **195.6 s** |
| Files converted | 2822 / 2825 |
| Output | **5644 files, 2.07 GB** |
| Known permanent failures | 3 (unchanged) |

The 3 failures are the documented set and are not regressions:

```
item/model/WF20.nif   : niflib parse failed: bad allocation
monster/model/M156.nif: unsupported block type 'NiPhysXScene'
npc/model/N600.nif    : no geometry found (183 blocks parsed)
```

### 1.1 The bottleneck is CPU, not I/O — measured, not assumed

Before choosing any parallel strategy, the question "CPU or I/O?" was settled
by measurement, because the answer decides whether write batching is worth
anything at all:

| Work | Time |
|---|---|
| Export `monster/` (413.7 MB of output) | **31.2 s** |
| Write those *same bytes* (warm cache, plain copy) | **0.91 s (453 MB/s)** |

**I/O is ~3% of wall time.** The workload is overwhelmingly CPU-bound — NIF
parsing and B-spline resampling at 30 Hz. This ruled out async I/O, write
batching and output buffering as optimisations before any of them were written,
and justified the simplest possible design: one file per job, one thread per
core, ordinary blocking writes.

---

## 2. The niflib thread-safety verdict: **not thread-safe as shipped**

This was flagged as the critical unknown for the phase, and the honest answer
is that the natural model — one file per job, no niflib objects shared — is
**necessary but not sufficient**. niflib carries global mutable state.

### 2.1 The audit

Every namespace-scope mutable object in niflib, and its verdict:

| State | Where | Verdict |
|---|---|---|
| `g_objects_registered` + `ObjectRegistry::object_map` | `src/niflib.cpp:41`, `src/ObjectRegistry.cpp` | **Real race.** See below. |
| `RefObject::_ref_count` | `include/RefObject.h:110` | Non-atomic `++`/`--`. Safe **only** because no object graph crosses threads. |
| `RefObject::objectsInMemory` | `src/RefObject.cpp:29` | Racy global counter. Benign here: torn values only, never read by this tool. |
| `Type::num_types` | `src/Type.cpp:8` | Incremented from `Type` constructors at static-init, before `main`. Safe. |
| `hdrInfo` / `strInfo` | `src/NIF_IO.cpp:826-827` | Use `ios_base::pword` — **per-stream**, not global. Safe; each job owns its stream. |

The race is niflib's lazy registration, inside `ReadNifList`:

```cpp
if ( g_objects_registered == false ) {   // plain bool, no atomics, no mutex
    g_objects_registered = true;
    RegisterObjects();                   // 441 inserts into a global std::map
}
```

Two threads can both read `false`. Worse, one thread can set the flag and still
be midway through its 441 inserts while another thread — seeing the flag
already `true` — starts *reading* that same `std::map`. That is undefined
behaviour, and in practice it returns null factories: a null `CreateObject` is
a block niflib **silently drops**.

Note that the pre-existing `EnsureNiflibObjectsRegistered()` in
`HeaderNormalizer.cpp` did *not* close this. It calls `RegisterObjects()` but
never sets `g_objects_registered`, so every worker's first `ReadNifList` would
still re-run the registration.

### 2.2 The measurement (`tools/tsprobe/`)

Per the discipline of earlier phases, the verdict is measured, not reasoned.
`tools/tsprobe/tsprobe.cpp` reproduces that exact guard from 12 threads and
then does what `ReadNifList` does for every block: look a type up. 20 runs of
each variant:

| Variant | Runs with failed lookups | Crashes |
|---|---|---|
| As shipped | **19 / 20** (1149–5736 failed lookups each) | **1** (`0xC0000005`) |
| With the fix | **0 / 20** | 0 |

This is exactly the intermittent, hard-to-diagnose corruption the brief warned
about, and it would have been invisible in a summary line: the files would
still "convert", just with blocks missing.

### 2.3 The fix

`Orchestrator::PrepareNiflibForConcurrentUse()`, called once before any worker
starts:

```cpp
Niflib::RegisterObjects();
Niflib::g_objects_registered = true;   // so ReadNifList never writes the map again
```

After this, `object_map` is **read-only** for the rest of the process and every
worker only ever reads it. `g_objects_registered` is a non-static
namespace-scope `bool` with external linkage, so it is declared `extern` from
our side — the same technique the codebase already used to reach
`Niflib::RegisterObjects()`. **The niflib submodule remains unmodified**, as it
has been since Phase 1.

**Verdict: niflib is safe for one-file-per-thread conversion if and only if
block-type registration is forced to completion before the threads start, and
no niflib object graph is shared between threads.** Both conditions hold here.
If the submodule is ever updated, re-run `tsprobe`.

---

## 3. Crash isolation

`FindUnsupportedBlockType` (Phase 2) is retained and still runs before
`ReadNifList` on every file. It is a *pre-flight* check on the header's own
block-type table, so it never hands niflib the input that would kill it —
`monster/model/M156.nif` (`NiPhysXScene`) is rejected cleanly as a normal
failure, in parallel exactly as sequentially.

Beyond that, `RunConversionJob` wraps its entire body in `catch (...)`. This
matters more in a threaded run than a sequential one: an exception escaping a
worker thread calls `std::terminate` and takes down the whole run, so one bad
file would cost 2824 good ones. With the guard it costs one line in the report.

**A separate process per file was considered and rejected.** Phase 2 used that
for *diagnosis*, but as a production strategy it would cost a process spawn per
file against a ~60 ms median conversion, and it buys nothing the pre-flight
check does not already provide: no file in the corpus now crashes the process.
The full corpus ran to completion in every one of the ~20 parallel runs made
during this phase, with no crash and no hang.

---

## 4. Determinism

Two successive full runs produce **byte-identical output across all 5644
files** (SHA-256 manifest comparison). Three properties give this:

1. **The scan is sorted.** `ScanEntities` sorts by path, so the job list does
   not depend on directory iteration order.
2. **Results are written by index, never appended.** Each worker writes only
   `results[i]`, so the aggregate, the failure list and the report are
   independent of which worker finished first.
3. **Jobs never share output.** One `.nif` writes exactly one `.gfmodel` +
   `.gfbin` pair, so no two workers can touch the same file.

Ordered-output lists are sorted with an explicit tiebreak (slowest files by
time, then by path) so that even equal values cannot reorder between runs.

---

## 5. Performance

### 5.1 Thread scaling (full corpus, 12-core machine)

| Threads | Wall | files/s | Speedup |
|---|---|---|---|
| 1 | 171.9 s | 16.4 | 1.0× |
| 2 | 100.4 s | 28.1 | 1.7× |
| 4 | 50.2 s | 56.3 | 3.4× |
| 6 | 43.4 s | 65.1 | 4.0× |
| 8 | 30.4 s | 92.8 | 5.7× |
| **12 (default)** | **28.9 s** | **97.8** | **5.9×** |
| 16 | 27.7 s | 102.0 | 6.2× |
| 24 | 29.0 s | 97.4 | 5.9× |

Scaling is near-linear to 8 threads, then flattens; 24 regresses. **The
retained configuration is the default, `hardware_concurrency`** (12 here): it
sits in the flat region, within ~4% of the best observed value, and needs no
machine-specific tuning. Against the 195.6 s sequential baseline measured with
the *old* binary, the end-to-end improvement is **6.7×**.

Run-to-run variance on this machine is substantial (29–40 s for identical
work), so single-sample comparisons below ~10% are meaningless — a point that
mattered in §5.3.

### 5.2 Where the time actually goes: 28 broken `.kf` files

The slowest file in the corpus is `chair/model/C067.nif` at ~9 s, against a
median of well under 0.1 s. Isolating it:

| | |
|---|---|
| `C067.nif` **with** its companion `.kf` | **9.08 s** |
| `C067.nif` with the `.kf` hidden | **0.03 s** |

The `.nif` is 329 KB and its geometry is unremarkable (15 meshes, 4077 verts).
**All 9 s is niflib working through a malformed `.kf`** (`premature end of
stream`) before giving up — and the animation is then correctly reported as
missing, so the time produces nothing.

This is not isolated. Across the corpus, **28 models have a companion `.kf`
that fails to parse** (15 `premature end of stream`, 13 `bad allocation`).
Timed individually and uncontended, those 28 models cost **67.8 s of the
~172 s single-threaded total — 39% of all the work in the corpus, spent on
animation data that is discarded.**

This is pre-existing Phase 4 behaviour, not a Phase 6 regression: the models
still convert, and the failure is already logged as a warning per file. It is
recorded here because it is the dominant cost in the corpus and the main reason
parallel scaling flattens — at 12 threads the whole run is ~29 s, and a single
file accounts for ~9 s of critical path. **Anyone wanting the next large
speedup should look here, not at the thread count.**

### 5.3 An optimisation that was implemented, measured, and removed

Longest-first dispatch was implemented on the reasonable theory that the ~9 s
job must not be picked up last. Measured over 4 interleaved A/B pairs at 12
threads (interleaved specifically because of the variance noted in §5.1):

| Dispatch order | Median | Min |
|---|---|---|
| Heaviest-first (`.nif` size + `.kf` weight) | 30.1 s | 29.9 s |
| Scan order | **29.2 s** | **29.2 s** |

It was **no better and usually slightly worse**, so it was removed rather than
kept as unjustified complexity. The reason it fails is instructive: `.nif` size
is a *bad* cost proxy for this corpus, because the expensive models are
**small** `.nif` files with broken `.kf` companions. A useful heuristic would
have to weight the `.kf`, which the scanner cannot judge without parsing it —
i.e. doing the expensive thing it is trying to schedule. The atomic
one-index-at-a-time work queue already keeps every worker busy until the tail,
which is where the remaining time goes.

### 5.4 I/O

2.07 GB is written per full run, at ~73 MB/s measured end-to-end — an order of
magnitude below the 453 MB/s the disk sustains (§1.1), confirming writes are
never the constraint. No special write strategy is used or needed.

---

## 6. Architecture

```
src/
├── cli/
│   ├── Args.*                 CLI11 parsing + rejection of impossible combinations
│   └── ProgressReporter.*     verbose stream vs in-place progress line
├── scanner/
│   └── EntityScanner.*        recursive .nif discovery, <type>/animation/NAME.kf pairing
├── pipeline/
│   ├── ConversionJob.*        one file in, one JobResult out; never throws
│   ├── ThreadPool.*           atomic-counter parallel-for
│   └── Orchestrator.*         niflib prep, dispatch, aggregation, JSON report
└── util/
    ├── Logger.*               thread-safe, progress-line aware, file always complete
    └── PathUtils.*            relative-tree mirroring, up-to-date checks
```

~1650 lines. Two points are load-bearing rather than incidental:

**`ParallelFor` is a parallel-for, not a task queue.** Every job is "convert
file *i*", and indexing by *i* is precisely what makes the result order
independent of completion order (§4). Work is handed out one index at a time
from an atomic counter rather than statically partitioned, so a 9 s file does
not strand a worker while others idle.

**The Logger and the progress line share the console.** In `--noverb` the
progress line owns the current terminal row, so any other output must erase it
first and let it repaint. The Logger takes `ClearLine`/`Redraw` callbacks for
this. The **log file receives everything regardless of console mode**, so
`--log-file` is always complete even when the console shows only a bar.

### 6.1 CLI11

Vendored as its single header at `external/cli11/CLI11.hpp` (v2.4.2, BSD
3-clause) rather than added to `vcpkg.json`, since the project had no
third-party dependencies and this keeps it that way. Included as a `SYSTEM`
directory so its warnings do not surface under our `/W3 /permissive-`.

---

## 7. CLI behaviour

### 7.1 Conflicting options

| Combination | Behaviour | Why |
|---|---|---|
| `--debug --threads 8` | Warns, stays **sequential** | `--debug` exists to produce a log that reads in file order. |
| `--debug --noverb` | **Refused**, exit 1 | Genuinely contradictory: maximum vs minimum console output. |
| `--dry-run --overwrite` | Warns, proceeds | Not contradictory, just redundant — but `--overwrite` still changes which files are *listed* as work. |
| `--entities dragon` | **Refused**, exit 1, lists valid types | A typo must not silently scan nothing. |
| `--debug-limit 0` | **Refused**, exit 1 | Would process nothing. |
| `--threads 999` | **Refused**, exit 1 | Typo guard. |

### 7.2 Edge cases verified

Nonexistent path, empty directory, partial tree (`model/` only, no
`animation/`), unwritable output root, and single-file input were each tested:
all exit cleanly with a specific message and no crash. An unwritable output is
reported as `cannot create output root ...: Access is denied.` with exit 1
rather than 2824 per-file failures.

### 7.3 A scanner bug this testing caught

`--input input/chair --entities chair` initially matched **zero** files. The
entity type was derived from the first component of each path *relative to the
scan root*, so with the root already at `input/chair` every file came out with
an empty type and the filter excluded everything. `RootEntityType` now
recognises a scan root that is itself an entity directory. This also restored
companion-`.kf` pairing for entity-rooted scans (111/111 for `chair`), which
had silently broken for the same reason — a good argument for testing the
partial-tree cases the brief asked for.

### 7.4 Compatibility

`dump` (Phase 1) and `export <path> -o <dir>` (Phase 2) still work; `export` is
rewritten onto the new pipeline, with its bare positional becoming `--input`
and `--input-root` accepted and ignored (the pipeline derives it from
`--input`). `--input` pointing at a single `.nif` is the supported way to run
one model through the real pipeline, which is how most diagnosis is done.

Skip logic: a file is skipped when **both** outputs exist and are at least as
new as the source. Requiring both means a half-written pair from an interrupted
run is correctly re-converted rather than treated as done.

---

## 8. Validation summary

| Deliverable | Result |
|---|---|
| Byte-identical to sequential reference | **Yes** — 5644 / 5644 files, SHA-256 |
| Determinism across two parallel runs | **Yes** — 5644 / 5644 files |
| Conversion count unchanged | **Yes** — 2822 / 2825, same 3 failures |
| Totals unchanged | **Yes** — 5,597,529 verts / 6,782,167 tris |
| niflib thread-safety | **Measured**: unsafe as shipped, safe with §2.3 |
| Speedup | **6.7×** (195.6 s → 29.2 s) |

The niflib submodule remains unmodified.

---

## 9. Open points for Phase 7

* **The 28 malformed `.kf` files (§5.2) are the dominant remaining cost** — 39%
  of total work for discarded output. Detecting the truncation cheaply before
  handing the stream to niflib would roughly halve single-threaded runtime.
  Out of scope here: it would change extraction behaviour, and this phase was
  required to be byte-identical.
* 302 texture references still unresolved (98% resolved), unchanged since
  Phase 2 and a corpus gap rather than a bug.
* The progress line assumes a ≤80-column console and is truncated to fit; it
  does not query the real terminal width.
