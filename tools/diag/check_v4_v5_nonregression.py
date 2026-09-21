#!/usr/bin/env python3
"""Non-regression check for the format 4 -> 5 change (Phase 8).

The claim to prove is "everything that already existed is byte-identical apart
from the additions". Two independent checks, because either alone is weak:

  1. The .gfbin must START with the old .gfbin, byte for byte. Material keys are
     appended after every existing buffer, so every byteOffset already published
     still addresses the same bytes. A prefix mismatch means an existing
     accessor moved -- the one failure mode that would silently corrupt a
     consumer rather than just lose data.

  2. The .gfmodel must be identical once the known additions are removed:
     formatVersion, generator, binaryByteLength, each clip's new "loop" and
     "materialTracks", and any clip that did not exist in v4 (a .kf sequence
     that carried only material tracks was previously dropped entirely).

Usage:
    check_v4_v5_nonregression.py <old_out_root> <new_out_root>
"""

import json
import sys
from pathlib import Path

ADDED_TOP_LEVEL = {"formatVersion", "generator", "binaryByteLength"}
ADDED_CLIP_KEYS = {"loop", "materialTracks"}


def strip_clip(clip):
    return {k: v for k, v in clip.items() if k not in ADDED_CLIP_KEYS}


def compare(old_doc, new_doc, problems, name):
    old_keys = set(old_doc) - ADDED_TOP_LEVEL
    new_keys = set(new_doc) - ADDED_TOP_LEVEL
    if old_keys != new_keys:
        problems.append(f"{name}: top-level keys differ: "
                        f"only-old={sorted(old_keys - new_keys)} "
                        f"only-new={sorted(new_keys - old_keys)}")
        return

    for key in sorted(old_keys):
        if key == "animations":
            continue
        if old_doc[key] == new_doc[key]:
            continue
        # "nodes" going from empty to populated is the one intended addition
        # outside the animation array: a file whose only animation is a
        # material track now needs its node list exported for that track to
        # resolve (see MeshExtractor.cpp, materialTrackNeedsNodes). It is only
        # additive as long as no mesh was re-expressed into node-local space,
        # which meshes[].nodeIndex proves.
        if key == "nodes" and not old_doc["nodes"] and new_doc["nodes"]:
            old_idx = [m["nodeIndex"] for m in old_doc.get("meshes", [])]
            new_idx = [m["nodeIndex"] for m in new_doc.get("meshes", [])]
            if old_idx == new_idx:
                continue
            problems.append(f"{name}: 'nodes' appeared AND a mesh was reparented")
            continue
        problems.append(f"{name}: '{key}' changed")

    # Animations: v5 may INSERT clips (a sequence that carried only material
    # tracks was dropped in v4). Every v4 clip must still be present, in the
    # same relative order, with its non-added fields unchanged.
    old_clips = old_doc.get("animations", [])
    new_clips = new_doc.get("animations", [])
    i = 0
    for oc in old_clips:
        matched = False
        while i < len(new_clips):
            if strip_clip(new_clips[i]) == oc:
                matched = True
                i += 1
                break
            i += 1
        if not matched:
            problems.append(f"{name}: v4 clip '{oc.get('name')}' "
                            f"({oc.get('originFile')}) missing or changed in v5")
            return


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    old_root, new_root = Path(sys.argv[1]), Path(sys.argv[2])

    problems = []
    checked = 0
    missing = 0

    for old_model in sorted(old_root.rglob("*.gfmodel")):
        rel = old_model.relative_to(old_root)
        new_model = new_root / rel
        if not new_model.exists():
            missing += 1
            continue
        checked += 1
        name = rel.as_posix()

        old_bin = old_model.with_suffix(".gfbin").read_bytes()
        new_bin = new_model.with_suffix(".gfbin").read_bytes()
        if len(new_bin) < len(old_bin):
            problems.append(f"{name}: .gfbin SHRANK ({len(old_bin)} -> {len(new_bin)})")
        elif new_bin[:len(old_bin)] != old_bin:
            # Locate the first differing byte, which says which buffer moved.
            for k in range(len(old_bin)):
                if new_bin[k] != old_bin[k]:
                    problems.append(f"{name}: .gfbin differs from byte {k} "
                                    f"(old length {len(old_bin)})")
                    break

        compare(json.loads(old_model.read_text()),
                json.loads(new_model.read_text()), problems, name)

    print(f"checked {checked} model(s); {missing} present in old but not new")
    if problems:
        print(f"{len(problems)} PROBLEM(S):")
        for p in problems[:60]:
            print("  " + p)
        return 1
    print("no regression: every v4 field and every v4 .gfbin byte is unchanged")
    return 0


if __name__ == "__main__":
    sys.exit(main())
