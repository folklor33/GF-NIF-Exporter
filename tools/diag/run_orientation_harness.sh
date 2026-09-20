#!/usr/bin/env bash
# run_orientation_harness.sh
#
# Drives the full visual-orientation validation harness (see the
# phase4-followup plan, step 2): stages the fixed sample list, runs
# blender_niftools_addon headlessly to produce reference .obj files, then
# compares them against the exporter's already-built .gfmodel/.gfbin output.
#
# Usage: tools/diag/run_orientation_harness.sh <gfmodel_out_dir> [json_out_path]
#
# <gfmodel_out_dir>  the exporter's -o output directory (already exported;
#                    this script does not run gfnif-export itself)
# [json_out_path]    optional path to write the machine-readable verdict JSON
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BLENDER="/c/Program Files/Blender Foundation/Blender 5.2/blender.exe"
SAMPLE_LIST="$SCRIPT_DIR/orientation_sample.txt"

GFMODEL_DIR="${1:?usage: run_orientation_harness.sh <gfmodel_out_dir> [json_out_path]}"
JSON_OUT="${2:-}"

STAGE_IN="$(mktemp -d)"
STAGE_OUT="$(mktemp -d)"
trap 'rm -rf "$STAGE_IN" "$STAGE_OUT"' EXIT

echo "Staging sample .nif files from $SAMPLE_LIST ..."
while IFS= read -r line; do
  line="${line%%#*}"
  line="$(echo "$line" | xargs)"
  [ -z "$line" ] && continue
  src="$REPO_ROOT/input/$line"
  if [ ! -f "$src" ]; then
    echo "  WARNING: sample file not found, skipping: $line" >&2
    continue
  fi
  base="$(basename "$line")"
  cp "$src" "$STAGE_IN/$base"
done < "$SAMPLE_LIST"

echo "Running self-test first (must pass before trusting any verdict) ..."
node "$SCRIPT_DIR/compare_orientation.js" --self-test

echo "Running blender_niftools_addon reference export (headless) ..."
"$BLENDER" -b --python "$REPO_ROOT/tools/nif_to_obj.py" -- "$STAGE_IN" "$STAGE_OUT"

echo "Comparing against $GFMODEL_DIR ..."
if [ -n "$JSON_OUT" ]; then
  node "$SCRIPT_DIR/compare_orientation.js" "$STAGE_OUT" "$GFMODEL_DIR" --json "$JSON_OUT"
else
  node "$SCRIPT_DIR/compare_orientation.js" "$STAGE_OUT" "$GFMODEL_DIR"
fi
