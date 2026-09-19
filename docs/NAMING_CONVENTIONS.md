# Naming & layout conventions (observed)

Everything here was observed directly from the Phase 1 test corpus in `input/`
(27 `.nif`/`.kf` files, 9033 `.png`). It records what the data actually does,
not what the NIF spec permits.

## 1. Directory layout

The input root holds one directory per entity type:

```
input/
├── chair/     ├── char/     ├── effect/   ├── elf/
├── item/      ├── monster/  ├── npc/      └── ride/
```

Each entity type contains up to three subdirectories:

| Subdirectory | Contents        | Present in                                     |
|--------------|-----------------|------------------------------------------------|
| `model/`     | `*.nif`         | all 8 types                                    |
| `texture/`   | `*.png`         | all except `char/`                             |
| `animation/` | `*.kf`          | `chair`, `char`, `item`, `monster`, `npc`, `ride` |

`effect/` and `elf/` have **no** `animation/` directory: their animation is
embedded in the `.nif` itself (via `NiTransformController` /
`NiTextureTransformController`), not shipped as external `.kf`.

`char/` has **no** `texture/` directory in this corpus, although its models do
reference textures. See §4.

## 2. Model ↔ animation pairing

**The rule: `<type>/model/NAME.nif` pairs with `<type>/animation/NAME.kf` —
identical basename, sibling directory, same entity type.**

Verified across the whole corpus:

| Type    | Models              | Animations      | Paired          |
|---------|---------------------|-----------------|-----------------|
| chair   | C011, C027          | C011, C027      | both            |
| char    | B2A02, B2A15, X6AQ9 | X6AQ9           | X6AQ9           |
| effect  | S13103, S14141      | —               | —               |
| elf     | X1006, X1016        | —               | —               |
| item    | W667, WA85          | W667            | W667            |
| monster | M011, M903          | M011, M903      | both            |
| npc     | N009, N920          | N009, N920      | both            |
| ride    | R834, R880          | R834, R880      | both            |

Two properties held without exception:

* **No orphan `.kf`.** Every `.kf` has a same-named `.nif`. A resolver can
  safely treat the `.nif` as the entry point and the `.kf` as optional.
* **A `.nif` may have no `.kf`** (B2A02, B2A15, WA85, and all of `effect/`,
  `elf/`). Missing animation is normal, not an error.

There is no cross-type lookup: a `monster` model never pulls animation from
`npc/animation/`. Resolution stays inside one entity-type directory.

## 3. Entity name prefixes

Basenames encode the entity type in their first character:

| Prefix | Type    | Examples          |
|--------|---------|-------------------|
| `C`    | chair   | C011, C027        |
| `B`/`X`| char    | B2A02, X6AQ9      |
| `S`    | effect  | S13103, S14141    |
| `X`    | elf     | X1006, X1016      |
| `W`    | item    | W667, WA85        |
| `M`    | monster | M011, M903        |
| `N`    | npc     | N009, N920        |
| `R`    | ride    | R834, R880        |

`X` appears in both `char/` and `elf/`, so **the prefix is not a reliable type
discriminator** — the containing directory is authoritative. Do not infer the
entity type from the filename.

## 4. Texture references

`NiSourceTexture` blocks store a bare filename with a **`.dds`** extension and
no directory component, e.g. `M01101.dds`. The shipped corpus contains `.png`
instead, converted upstream with the basename preserved.

**Resolution rule:** take the `NiSourceTexture` filename, replace the extension
with `.png`, and look it up in `<same entity type>/texture/`.

Measured across all 17 `.nif` files: **54 of 66** distinct texture references
resolve this way. The 12 that do not are all in `char/` (which ships no
`texture/` directory) and `elf/` (whose `texture/` holds unrelated `F*.png`
files). These are gaps in the *test corpus*, not in the convention — the
exporter must nonetheless treat a missing texture as a warning and continue,
rather than failing the model.

Texture names carry a suffix that hints at their role (`H01`, `H51`, `H55`…),
e.g. `X6AQ9H01.dds`, `X6AQ9H51.dds`. The exact meaning was not determined in
Phase 1 and is not needed for export.

## 5. Animation clip names (inside `.kf`)

Each `.kf` holds one or more `NiControllerSequence` blocks, one per clip, and
the count of `NiControllerSequence` equals the count of `NiTextKeyExtraData`
in every file — one text-key track per clip.

Observed clip names follow a `<verb><NN>` pattern:

```
stand01  stand02  move01  move51  battle01  attack01  hurt01  death01
```

`NiTextKeyExtraData` consistently carries `start` and `end` markers delimiting
the clip range.

Clip counts vary widely: 1 for `chair/C011`, 86–87 for the two `ride` files.
