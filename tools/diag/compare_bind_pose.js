// compare_bind_pose.js
//
// Validates the exporter's skeleton bind pose (SkeletonExtractor's
// bindMatrixLocal) directly, by comparing each bone's reconstructed
// ARMATURE-SPACE (world, relative to the skeleton root) position AND
// ORIENTATION against an independent reference: blender_niftools_addon's own
// imported armature rest pose, dumped by dump_bone_bind_pose.py.
//
// Two independent checks (see PHASE4_FINDINGS' M009 writeup, which is what
// motivated adding the second -- the original position-only check verdicted
// M009 OK while the exported model visibly showed the body flipped relative
// to the limbs):
//   1. Per-bone position (original check): each bone's world translation
//      close to the reference.
//   2. Assembly coherence: the RELATIVE rotation between specific pairs of
//      bones known to straddle a prior tear boundary, reference vs export.
//      This is what actually catches "two groups each individually close to
//      their own reference bone, but twisted relative to each other" -- and
//      it needs no absolute per-bone orientation reference, so it sidesteps
//      a dead end this harness went down during development: a per-bone
//      ABSOLUTE rotation check (comparing each bone's world orientation
//      directly to the reference) was tried and dropped. It assumed
//      blender_niftools_addon's import applies one constant, global change
//      of basis relative to this exporter's native frame -- checked by
//      computing the ref->export rotation correction independently for
//      several bones on the same file and finding they were NOT the same
//      matrix (Bip01 showed one permutation, Bip01 Pelvis/Spine a different
//      one, Bip01 L Thigh/Calf a near-identity), so no single correction
//      exists to make that check meaningful, and every attempt produced huge
//      false-positive rates even on M389 (a file with no known defect). The
//      RELATIVE check below needs no such correction: it cancels any
//      per-branch orientation-convention difference by construction.
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

// SkeletonExtractor's bindMatrixLocal is written by ToColumnMajor
// (SkeletonExtractor.cpp: `out[i*4+j] = m[i][j]`, where niflib's Matrix44
// m[i] IS row i -- confirmed directly against niflib's own
// Matrix44::GetTranslation(), which reads m[3][0..2] -- so `out` actually
// lays out FLAT[row*4+col], i.e. plain ROW-MAJOR despite the function's name
// and doc-comment claiming glTF/column-major order. Verified by hand on a
// real bone (M389's Bip01): its translation (-1.6e-8, -0.333, 0.912) sits at
// flat[12..15], and flat[row*4+col] with row=3 gives exactly indices 12-15 --
// flat[col*4+row] (true column-major) would NOT put translation there for a
// row-vector matrix. dump_bone_bind_pose.py's matrix_local is dumped
// genuinely row-major too (documented in that script), so both sources use
// this same flat[row*4+col] layout and share this one reader.
//
// This function used to read flat[c*4+r] (true column-major), which for
// THIS exporter's actual row-major output silently transposed every matrix:
// translation ended up in column 3 of every row instead of row 3, so
// translationOf's `m[3]` read all-zero garbage on both sides (explaining why
// every file's position check always reported "off by 0.0000" -- it was
// comparing zero to zero, not the real translations), and the rotation block
// was contaminated with transposed data, producing large bogus per-bone
// rotation errors that appeared to worsen down the bone chain. Fixed here to
// match the writer's actual layout.
function flatToRowsCols(flat) {
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

// Upper-left 3x3 (rotation+scale) block, as three row vectors -- row-vector
// convention throughout, matching translationOf.
function rotationRowsOf(m) {
  return [
    [m[0][0], m[0][1], m[0][2]],
    [m[1][0], m[1][1], m[1][2]],
    [m[2][0], m[2][1], m[2][2]],
  ];
}

function normalize3(v) {
  const n = Math.hypot(v[0], v[1], v[2]) || 1;
  return [v[0] / n, v[1] / n, v[2] / n];
}

// The relative rotation taking bone A's frame to bone B's frame: R_rel =
// R_A^T * R_B (row-vector convention). Comparing this between the reference
// and the export -- rather than each bone's absolute orientation -- is what
// catches two internally-consistent bone groups that disagree with each
// other (each bone individually "close enough" to its own reference, but the
// two groups twisted relative to one another). See PHASE4_FINDINGS' M009
// writeup: a per-bone-absolute check can pass while this fails.
function relativeRotationDeg(rowsA, rowsB) {
  const a = rowsA.map(normalize3);
  const b = rowsB.map(normalize3);
  const r = [[0, 0, 0], [0, 0, 0], [0, 0, 0]];
  for (let i = 0; i < 3; i++)
    for (let j = 0; j < 3; j++)
      for (let k = 0; k < 3; k++)
        r[i][j] += a[k][i] * b[k][j];
  return r;
}

function relativeRotationAngleDeg(refRowsA, refRowsB, expRowsA, expRowsB) {
  const refRel = relativeRotationDeg(refRowsA, refRowsB);
  const expRel = relativeRotationDeg(expRowsA, expRowsB);
  const r = [[0, 0, 0], [0, 0, 0], [0, 0, 0]];
  for (let i = 0; i < 3; i++)
    for (let j = 0; j < 3; j++)
      for (let k = 0; k < 3; k++)
        r[i][j] += refRel[k][i] * expRel[k][j];
  const trace = r[0][0] + r[1][1] + r[2][2];
  let cos = (trace - 1) / 2;
  cos = Math.max(-1, Math.min(1, cos));
  return Math.acos(cos) * 180 / Math.PI;
}

function loadGfModelSkeleton(gfmodelPath) {
  const model = JSON.parse(fs.readFileSync(gfmodelPath, 'utf8'));
  if (!model.skeletons || model.skeletons.length === 0) return null;
  const skel = model.skeletons[0];

  const worldMatrices = new Array(skel.bones.length);
  for (let i = 0; i < skel.bones.length; i++) {
    const bone = skel.bones[i];
    const local = flatToRowsCols(bone.bindMatrixLocal);
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

// Blender's own Matrix (dump_bone_bind_pose.py's bone.matrix_local) is a
// COLUMN-VECTOR-convention transform (v' = M @ v, translation in the last
// COLUMN), whereas this exporter's bindMatrixLocal is ROW-VECTOR-convention
// (v' = v * M, translation in the last ROW) throughout, per every other
// comment in this codebase. The same physical transform, expressed in the
// two conventions for the same basis, is one matrix transposed relative to
// the other -- so after flatToRowsCols reconstructs Blender's [row][col]
// values verbatim, that matrix must be transposed once to land translation
// in row 3 and become directly comparable (by translationOf/rotationRowsOf)
// to the exporter's own matrices.
//
// Confirmed by hand on M389's Bip01: the dumped flat array has translation
// (-1.6e-8, -0.333, 0.912) at flat[3], flat[7], flat[11] (the LAST element
// of each row) -- i.e. in column 3, exactly where a column-vector-convention
// matrix keeps it, and NOT where niflib's row-vector translation (row 3)
// would put the same numbers.
function transpose4(m) {
  const out = [[0,0,0,0],[0,0,0,0],[0,0,0,0],[0,0,0,0]];
  for (let r = 0; r < 4; r++)
    for (let c = 0; c < 4; c++)
      out[r][c] = m[c][r];
  return out;
}

function loadReferenceBones(jsonPath) {
  const raw = JSON.parse(fs.readFileSync(jsonPath, 'utf8'));
  const byName = {};
  for (const [name, flat] of Object.entries(raw)) {
    byName[denormalizeBlenderSideSuffix(name)] = transpose4(flatToRowsCols(flat));
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

// Degrees, for the RELATIVE (assembly-coherence) rotation check only -- see
// that check's own comment for why an absolute per-bone rotation tolerance
// is not used here. Generous above ordinary floating-point/PyFFI-vs-niflib
// rounding (same rationale as POSITION_TOL), but far below the kind of error
// a wrong bind-pose reconstruction actually produces (M009's Pelvis/Spine
// boundary measured well over 90 degrees off before the fix -- see
// PHASE4_FINDINGS).
const RELATIVE_ROTATION_TOL_DEG = 2.0;

// Bone-name pairs known to straddle a group boundary this exporter's bind
// pose has previously torn along (PHASE4_FINDINGS' M009 writeup: Pelvis is
// reconstructed from skin data while its child Spine, and everything below
// it, keeps the raw NiNode transform). Checked on every file that has both
// names, not just M009, since the same tear shape can recur on any skinned
// biped-style skeleton in the corpus. This list is deliberately small and
// explicit rather than "all adjacent pairs" -- it targets the exact
// known-recurring failure mode, not a full combinatorial sweep.
const ASSEMBLY_COHERENCE_PAIRS = [
  ['Bip01 Pelvis', 'Bip01 Spine'],
  ['Bip01 Spine', 'Bip01 L Thigh'],
  ['Bip01 Spine', 'Bip01 Hand'],
  ['Bip01 L Hand', 'Bip01 L Finger1'],
  ['Bip01 R Hand', 'Bip01 R Finger1'],
  ['Bip01 Pelvis', 'Bip01 Tail'],
];

// NOTE on per-bone ABSOLUTE rotation: an earlier version of this harness
// tried to compare each bone's absolute world orientation against the
// reference, on the theory that blender_niftools_addon imports into a fixed,
// constant global change of basis relative to this exporter's native frame
// (this WOULD be true for a pure axis-order/handedness remap applied
// uniformly at import). That theory was checked by computing the ref->export
// rotation correction independently for many bones on the same file (M009)
// and comparing them: they were NOT the same matrix (Bip01 showed one
// permutation, Bip01 Pelvis/Spine a different one, Bip01 L Thigh/Calf a
// near-identity) -- so there is no single global correction to estimate, and
// no reliable per-bone absolute-rotation check without one. Rather than ship
// a check whose false-positive rate on already-known-good files (M389) is
// unmeasured and was seen to be very high during development, this harness
// deliberately limits itself to POSITION (translationOf, trustworthy: it
// does not depend on any rotation-frame assumption) and RELATIVE rotation
// between specific bone pairs (below), which cancels any per-branch
// orientation convention difference by construction and needs no correction
// estimate at all.
function compareSkeletons(refBones, expBones) {
  const commonNames = Object.keys(expBones).filter(n => n in refBones);
  const onlyInRef = Object.keys(refBones).filter(n => !(n in expBones));
  const onlyInExp = Object.keys(expBones).filter(n => !(n in refBones));

  const perBone = [];
  for (const name of commonNames) {
    const refM = refBones[name], expM = expBones[name];
    const refPos = translationOf(refM);
    const expPos = translationOf(expM);
    const dist = Math.hypot(refPos[0] - expPos[0], refPos[1] - expPos[1], refPos[2] - expPos[2]);
    perBone.push({ name, refPos, expPos, dist });
  }
  perBone.sort((a, b) => b.dist - a.dist);

  const worst = perBone.length ? perBone[0] : null;
  const badBones = perBone.filter(b => b.dist > POSITION_TOL);

  // Assembly-coherence: for each known-risky pair present in both skeletons,
  // compare the RELATIVE rotation between the two bones, reference vs
  // export. This is independent of each bone's own absolute-position/
  // rotation check above -- it is exactly what catches two groups that are
  // each individually within tolerance of their own reference bone but
  // twisted relative to each other (the M009 symptom: "the body is flipped
  // relative to the limbs", not "every bone is in the wrong place").
  const assemblyChecks = [];
  for (const [nameA, nameB] of ASSEMBLY_COHERENCE_PAIRS) {
    if (!(nameA in refBones) || !(nameB in refBones)) continue;
    if (!(nameA in expBones) || !(nameB in expBones)) continue;
    const angleDeg = relativeRotationAngleDeg(
      rotationRowsOf(refBones[nameA]), rotationRowsOf(refBones[nameB]),
      rotationRowsOf(expBones[nameA]), rotationRowsOf(expBones[nameB]),
    );
    assemblyChecks.push({ nameA, nameB, angleDeg });
  }
  const badAssemblyPairs = assemblyChecks.filter(c => c.angleDeg > RELATIVE_ROTATION_TOL_DEG);

  const verdicts = [];
  if (badBones.length > 0) verdicts.push('BONE_POSITION_MISMATCH');
  if (badAssemblyPairs.length > 0) verdicts.push('ASSEMBLY_INCOHERENT');

  return {
    verdict: verdicts.length === 0 ? 'OK' : verdicts.join('+'),
    boneCount: commonNames.length,
    badBoneCount: badBones.length,
    worstBone: worst ? { name: worst.name, dist: worst.dist } : null,
    badBones: badBones.slice(0, 10).map(b => ({ name: b.name, dist: b.dist })),
    assemblyChecks,
    badAssemblyPairs,
    onlyInRef,
    onlyInExp,
  };
}

// ---------- self-test ----------
//
// Synthesizes exactly the M009 failure shape: two rigid bone groups, each
// individually placed EXACTLY where the reference says (so the position
// check above would pass on every bone), but with the second group rotated
// 90 degrees relative to the first -- reproducing "the body is flipped
// relative to the limbs" without needing a real .nif or Blender.
// This must pass before trusting any real verdict from this harness (the
// M009 case itself: the OLD position-only harness verdicted it OK).
const ROT_Z90_ROWS = [[0, 1, 0], [-1, 0, 0], [0, 0, 1]]; // row-vector: p' = p * R, 90 deg about Z

function matFromRotAndPos(rotRows, pos) {
  return [
    [rotRows[0][0], rotRows[0][1], rotRows[0][2], 0],
    [rotRows[1][0], rotRows[1][1], rotRows[1][2], 0],
    [rotRows[2][0], rotRows[2][1], rotRows[2][2], 0],
    [pos[0], pos[1], pos[2], 1],
  ];
}

const IDENTITY_ROWS = [[1, 0, 0], [0, 1, 0], [0, 0, 1]];

function selfTest() {
  // A tiny two-group skeleton mirroring M009's Pelvis/Spine tear: "Body" and
  // "BodyChild" form one group (e.g. Pelvis and Spine); "Limb" and
  // "LimbChild" form a second group re-anchored under Body but internally
  // rigid (e.g. Thigh and Calf hanging off the mis-anchored Spine).
  const refBones = {
    Body: matFromRotAndPos(IDENTITY_ROWS, [0, 0, 0]),
    BodyChild: matFromRotAndPos(IDENTITY_ROWS, [0, 0, 1]),
    Limb: matFromRotAndPos(IDENTITY_ROWS, [0, 0, 2]),
    LimbChild: matFromRotAndPos(IDENTITY_ROWS, [0, 0, 3]),
  };

  // Case 1: exporter output identical to reference -> OK on all three checks.
  const identical = compareSkeletons(refBones, refBones);

  // Case 2: reproduce the M009 tear. Limb/LimbChild's POSITIONS are kept
  // exactly as the reference (as ReconstructBindPoseFromSkin's re-anchoring
  // does: it does not un-place the subtree, just leaves its orientation on
  // the wrong side of the convention boundary) but their ORIENTATION is
  // rotated 90 degrees relative to Body/BodyChild. A position-only check
  // sees perfect agreement on every single bone; only the assembly-coherence
  // check can see this.
  const tornExpBones = {
    Body: refBones.Body,
    BodyChild: refBones.BodyChild,
    Limb: matFromRotAndPos(ROT_Z90_ROWS, translationOf(refBones.Limb)),
    LimbChild: matFromRotAndPos(ROT_Z90_ROWS, translationOf(refBones.LimbChild)),
  };
  const savedPairs = ASSEMBLY_COHERENCE_PAIRS.splice(0, ASSEMBLY_COHERENCE_PAIRS.length,
    ['Body', 'Limb']);
  const torn = compareSkeletons(refBones, tornExpBones);
  ASSEMBLY_COHERENCE_PAIRS.splice(0, ASSEMBLY_COHERENCE_PAIRS.length, ...savedPairs);

  console.log('Self-test case 1 (identical):', identical.verdict);
  console.log('Self-test case 2 (M009-shaped tear):', torn.verdict,
    `(badBoneCount=${torn.badBoneCount}, badAssemblyPairs=${torn.badAssemblyPairs.length})`);

  const pass1 = identical.verdict === 'OK';
  // The critical assertion: position agreement must be perfect (this is what
  // the OLD harness checked and would have called this case OK), while the
  // assembly-coherence check must catch it.
  const pass2 = torn.badBoneCount === 0 && torn.badAssemblyPairs.length > 0;

  if (!pass1) console.error('SELF-TEST FAILED: identical case did not verdict OK');
  if (!pass2) {
    console.error('SELF-TEST FAILED: M009-shaped tear was not caught ' +
      '(a position-only check would have missed this -- the whole point of this harness)');
  }

  return pass1 && pass2;
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
  if (args.includes('--self-test')) {
    const ok = selfTest();
    process.exit(ok ? 0 : 1);
  }

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
    const worstStr = cmp.worstBone ? `worst pos: ${cmp.worstBone.name} off by ${cmp.worstBone.dist.toFixed(4)}` : 'no common bones';
    console.log(`${baseName}: ${cmp.verdict} (${cmp.badBoneCount}/${cmp.boneCount} pos over tol; ${worstStr})`);
    if (cmp.badBoneCount > 0) {
      for (const b of cmp.badBones) console.log(`    pos ${b.name}: off by ${b.dist.toFixed(4)}`);
    }
    if (cmp.badAssemblyPairs.length > 0) {
      for (const p of cmp.badAssemblyPairs) console.log(`    assembly ${p.nameA} <-> ${p.nameB}: relative rotation off by ${p.angleDeg.toFixed(1)} deg`);
    }
  }

  if (jsonOut) {
    fs.writeFileSync(jsonOut, JSON.stringify(results, null, 2));
    console.log(`\nWrote ${jsonOut}`);
  }
}

main();
