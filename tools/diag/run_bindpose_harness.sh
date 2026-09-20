#!/usr/bin/env bash
# run_bindpose_harness.sh
#
# Drives the bind-pose (bone rest position) validation harness: dumps each
# sample .nif's armature rest pose via blender_niftools_addon
# (dump_bone_bind_pose.py), then compares those bone positions directly
# against the exporter's own bindMatrixLocal (compare_bind_pose.js).
#
# This is separate from run_orientation_harness.sh's mesh-geometry check,
# which cannot see a bindMatrixLocal defect (see compare_bind_pose.js's own
# doc comment for why).
#
# Usage: tools/diag/run_bindpose_harness.sh <gfmodel_out_dir> [json_out_path]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BLENDER="/c/Program Files/Blender Foundation/Blender 5.2/blender.exe"
SAMPLE_LIST="$SCRIPT_DIR/orientation_sample.txt"

GFMODEL_DIR="${1:?usage: run_bindpose_harness.sh <gfmodel_out_dir> [json_out_path]}"
JSON_OUT="${2:-}"

STAGE_BONES="$(mktemp -d)"
trap 'rm -rf "$STAGE_BONES"' EXIT

echo "Dumping reference bone bind poses from $SAMPLE_LIST ..."
while IFS= read -r line; do
  line="${line%%#*}"
  line="$(echo "$line" | xargs)"
  [ -z "$line" ] && continue
  src="$REPO_ROOT/input/$line"
  if [ ! -f "$src" ]; then
    echo "  WARNING: sample file not found, skipping: $line" >&2
    continue
  fi
  base="$(basename "$line" .nif)"
  "$BLENDER" -b --python "$SCRIPT_DIR/dump_bone_bind_pose.py" -- "$src" "$STAGE_BONES/$base.json" \
    2>&1 | grep -E "^Dumped|^NO_ARMATURE" || true
done < "$SAMPLE_LIST"

echo "Comparing against $GFMODEL_DIR ..."
if [ -n "$JSON_OUT" ]; then
  node "$SCRIPT_DIR/compare_bind_pose.js" "$STAGE_BONES" "$GFMODEL_DIR" --json "$JSON_OUT"
else
  node "$SCRIPT_DIR/compare_bind_pose.js" "$STAGE_BONES" "$GFMODEL_DIR"
fi
