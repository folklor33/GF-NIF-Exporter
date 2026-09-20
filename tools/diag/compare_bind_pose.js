// compare_bind_pose.js
//
// Validates the exporter's skeleton bind pose (SkeletonExtractor's
// bindMatrixLocal) directly, by comparing each bone's reconstructed
// ARMATURE-SPACE (world, relative to the skeleton root) position against an
// independent reference: blender_niftools_addon's own imported armature rest
// pose, dumped by dump_bone_bind_pose.py.
//
// This exists because tools/diag/compare_orientation.js's mesh-geometry check
// CANNOT see a bindMatrixLocal defect: the viewer computes each skinned
// mesh's boneInverses FROM the current bone matrices at load time
// (tools/viewer/index.html, buildScene), so the bind-pose mesh always
// reproduces the baked skinMatrix correctly regardless of whether
// bindMatrixLocal is right -- the defect only becomes visible once an
// animation clip moves a bone away from its (possibly wrong) rest pose.
// Comparing bone positions directly, independent of any mesh or animation,
// is the only way to catch this without actually replaying a clip.
//
// Usage:
//   node compare_bind_pose.js <bone_json_dir> <gfmodel_dir>
//
// <bone_json_dir>  directory of *.json produced by dump_bone_bind_pose.py
//                  (one file per sample .nif, same basename)
// <gfmodel_dir>    directory tree containing the matching *.gfmodel

const fs = require('fs');
const path = require('path');

// SkeletonExtractor's bindMatrixLocal is column-major (ToColumnMajor, glTF/
// Three.js convention, matches every other matrix this exporter writes).
// dump_bone_bind_pose.py's matrix_local is dumped row-major (documented in
// that script). Both represent the SAME row-vector-applied transform
// (v' = v * M in the exporter's own row-vector convention throughout, per
// PHASE3/4_FINDINGS) -- reading one as row-major and the other as
// column-major and then multiplying points the same way makes them
// comparable without an extra transpose, since a column-major flat array
// interpreted with column-major indexing IS the transpose of the same array
// interpreted row-major, and this exporter's own multiplyPointColumnMajor
// helper already expects the glTF layout consistently. To avoid ambiguity,
// this script converts both into one explicit 4x4 nested-array form (row i,
// col j) before doing anything else.
function columnMajorFlatToRowsCols(flat) {
  // flat[c*4+r] = element at row r, col c (glTF/Three.js convention)
  const m = [[0,0,0,0],[0,0,0,0],[0,0,0,0],[0,0,0,0]];
  for (let c = 0; c < 4; c++)
    for (let r = 0; r < 4; r++)
      m[r][c] = flat[c * 4 + r];
  return m;
}

function rowMajorFlatToRowsCols(flat) {
  const m = [[0,0,0,0],[0,0,0,0],[0,0,0,0],[0,0,0,0]];
  for (let r = 0; r < 4; r++)
    for (let c = 0; c < 4; c++)
      m[r][c] = flat[r * 4 + c];
  return m;
}

// Row-vector convention throughout this exporter (v' = v * M): multiplying
// two transforms so that applying A then B is `A_then_B = A * B` in this
// convention (NOT B*A as it would be for the column-vector convention).
function matMul(a, b) {
  const out = [[0,0,0,0],[0,0,0,0],[0,0,0,0],[0,0,0,0]];
  for (let i = 0; i < 4; i++)
    for (let j = 0; j < 4; j++) {
      let s = 0;
      for (let k = 0; k < 4; k++) s += a[i][k] * b[k][j];
      out[i][j] = s;
    }
  return out;
}

const IDENTITY = [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]];

function translationOf(m) {
  // Row-vector convention: translation lives in row 3 (last row), matching
  // niflib's Matrix44 layout used throughout this exporter's own C++ (see
  // SkeletonExtractor.cpp's own comments on row-vector Matrix44).
  return [m[3][0], m[3][1], m[3][2]];
}

function loadGfModelSkeleton(gfmodelPath) {
  const model = JSON.parse(fs.readFileSync(gfmodelPath, 'utf8'));
  if (!model.skeletons || model.skeletons.length === 0) return null;
  const skel = model.skeletons[0];

  const worldMatrices = new Array(skel.bones.length);
  for (let i = 0; i < skel.bones.length; i++) {
    const bone = skel.bones[i];
    const local = columnMajorFlatToRowsCols(bone.bindMatrixLocal);
    const parentWorld = bone.parent >= 0 ? worldMatrices[bone.parent] : IDENTITY;
    worldMatrices[i] = matMul(local, parentWorld);
  }

  const byName = {};
  skel.bones.forEach((b, i) => { byName[b.name] = worldMatrices[i]; });
  return byName;
}

// blender_niftools_addon renames bones on import to Blender's own left/right
// naming convention ("Bip01 L Thigh" -> "Bip01 Thigh.L", confirmed by direct
// comparison against the exporter's own bone list for monster/M009: without
// this normalization only 20 of 70 bones matched by exact name, all of them
// bones with no L/R marker at all -- the addon's own naming-convention
// remap, not a real corpus difference). Undo it so bones compare by their
// original NIF name.
function denormalizeBlenderSideSuffix(name) {
  // "Bip01 Thigh.L" -> base="Bip01 Thigh", side="L" -> "Bip01 L Thigh":
  // the side marker moves from a ".L"/".R" suffix back to right before the
  // last space-separated word, matching the exporter's own NIF-native names.
  const m = name.match(/^(.*)\.(L|R)$/);
  if (!m) return name;
  const [, base, side] = m;
  const lastSpace = base.lastIndexOf(' ');
  if (lastSpace < 0) return `${side} ${base}`;
  return `${base.slice(0, lastSpace)} ${side}${base.slice(lastSpace)}`;
}

function loadReferenceBones(jsonPath) {
  const raw = JSON.parse(fs.readFileSync(jsonPath, 'utf8'));
  const byName = {};
  for (const [name, flat] of Object.entries(raw)) {
    byName[denormalizeBlenderSideSuffix(name)] = rowMajorFlatToRowsCols(flat);
  }
  return byName;
}

// Tolerance: PHASE4_FINDINGS §14.2 measured M017's reconstructed bone
// positions matching blender_niftools_addon "to 3 decimal places" in the
// same native units this exporter uses -- 0.01 is a deliberately generous
// tolerance above that (this is a corpus-wide automated gate, not a single
// hand-checked case, and floating-point/PyFFI-vs-niflib rounding differences
// are expected to be larger than a single hand check's best-case precision).
const POSITION_TOL = 0.01;

function compareSkeletons(refBones, expBones) {
  const commonNames = Object.keys(expBones).filter(n => n in refBones);
  const onlyInRef = Object.keys(refBones).filter(n => !(n in expBones));
  const onlyInExp = Object.keys(expBones).filter(n => !(n in refBones));

  const perBone = [];
  for (const name of commonNames) {
    const refPos = translationOf(refBones[name]);
    const expPos = translationOf(expBones[name]);
    const dist = Math.hypot(refPos[0] - expPos[0], refPos[1] - expPos[1], refPos[2] - expPos[2]);
    perBone.push({ name, refPos, expPos, dist });
  }
  perBone.sort((a, b) => b.dist - a.dist);

  const worst = perBone.length ? perBone[0] : null;
  const badBones = perBone.filter(b => b.dist > POSITION_TOL);

  return {
    verdict: badBones.length === 0 ? 'OK' : 'BONE_POSITION_MISMATCH',
    boneCount: commonNames.length,
    badBoneCount: badBones.length,
    worstBone: worst ? { name: worst.name, dist: worst.dist } : null,
    badBones: badBones.slice(0, 10).map(b => ({ name: b.name, dist: b.dist })),
    onlyInRef,
    onlyInExp,
  };
}

function findGfModel(gfmodelDir, baseName) {
  const stack = [gfmodelDir];
  const target = baseName + '.gfmodel';
  while (stack.length) {
    const dir = stack.pop();
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      const full = path.join(dir, entry.name);
      if (entry.isDirectory()) stack.push(full);
      else if (entry.name === target) return full;
    }
  }
  return null;
}

function main() {
  const args = process.argv.slice(2);
  if (args.length < 2) {
    console.error('Usage: node compare_bind_pose.js <bone_json_dir> <gfmodel_dir> [--json out.json]');
    process.exit(1);
  }
  const [jsonDir, gfmodelDir] = args;
  const jsonIdx = args.indexOf('--json');
  const jsonOut = jsonIdx >= 0 ? args[jsonIdx + 1] : null;

  const boneFiles = fs.readdirSync(jsonDir).filter(f => f.toLowerCase().endsWith('.json'));
  const results = {};

  for (const boneFile of boneFiles) {
    const baseName = path.basename(boneFile, '.json');
    const refBones = loadReferenceBones(path.join(jsonDir, boneFile));
    if (Object.keys(refBones).length === 0) {
      results[baseName] = { verdict: 'NO_REFERENCE_ARMATURE' };
      console.log(`${baseName}: NO_REFERENCE_ARMATURE (file has no skeleton, or Blender found none)`);
      continue;
    }

    const gfmodelPath = findGfModel(gfmodelDir, baseName);
    if (!gfmodelPath) {
      results[baseName] = { verdict: 'NO_GFMODEL_FOUND' };
      console.log(`${baseName}: NO_GFMODEL_FOUND`);
      continue;
    }

    const expBones = loadGfModelSkeleton(gfmodelPath);
    if (!expBones) {
      results[baseName] = { verdict: 'EXPORTER_HAS_NO_SKELETON' };
      console.log(`${baseName}: EXPORTER_HAS_NO_SKELETON (reference has one -- possible regression)`);
      continue;
    }

    const cmp = compareSkeletons(refBones, expBones);
    results[baseName] = cmp;
    const worstStr = cmp.worstBone ? `worst: ${cmp.worstBone.name} off by ${cmp.worstBone.dist.toFixed(4)}` : 'no common bones';
    console.log(`${baseName}: ${cmp.verdict} (${cmp.badBoneCount}/${cmp.boneCount} bones over tolerance; ${worstStr})`);
    if (cmp.badBoneCount > 0) {
      for (const b of cmp.badBones) console.log(`    ${b.name}: off by ${b.dist.toFixed(4)}`);
    }
  }

  if (jsonOut) {
    fs.writeFileSync(jsonOut, JSON.stringify(results, null, 2));
    console.log(`\nWrote ${jsonOut}`);
  }
}

main();
