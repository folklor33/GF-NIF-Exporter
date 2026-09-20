// compare_orientation.js
//
// Visual-orientation validation harness (see the phase4-followup plan).
//
// Compares the exporter's own .gfmodel/.gfbin output against an independent
// reference: an .obj produced by blender_niftools_addon via tools/nif_to_obj.py
// (native NIF import -> merge meshes -> export .obj). blender_niftools_addon
// is a completely separate codebase from this exporter, so agreement between
// the two is real evidence of correct orientation -- unlike this project's
// existing self-checks (GetSkinDeformation, quaternion norms, etc), which can
// only catch the exporter disagreeing with itself.
//
// Neither side's raw coordinates are compared directly:
//   - nif_to_obj.py exports with axis_forward='-Z' (or the 5.2+ equivalent,
//     forward_axis='NEGATIVE_Z'), axis_up='Y' -- a fixed axis remap from the
//     source Z-up frame to Blender's/Wavefront OBJ's Y-up convention.
//   - nif_to_obj.py's center_mesh_only() recenters X/Y to the bbox center and
//     drops the model's minimum Z to 0 -- an arbitrary translation.
// Both must be undone (or made irrelevant) before comparing. Rather than
// trying to invert center_mesh_only() exactly, this script compares
// translation-*invariant* and axis-remap-*corrected* quantities:
//   - PCA principal axes (translation-invariant already; remapped by undoing
//     the OBJ's fixed axis permutation before comparing to the exporter's
//     native Z-up frame)
//   - oriented bounding-box dimensions per axis (translation-invariant,
//     axis-order-sensitive -- compared after the same axis correction)
//   - dimension ratios per axis (catches a model lying on its side)
//   - relative top/bottom extent along the vertical axis (catches a model
//     flipped upside down)
//
// Usage:
//   node compare_orientation.js <obj_dir> <gfmodel_dir> [--self-test]
//
// <obj_dir>      directory of *.obj produced by nif_to_obj.py
// <gfmodel_dir>  directory tree containing the matching *.gfmodel/*.gfbin
//                (searched recursively, matched by basename)
//
// --self-test: runs the harness against its own known-good file plus a
// synthetically-rotated copy of the same data, and exits nonzero if the
// harness fails to (a) call the unrotated copy OK and (b) detect the
// synthetic rotation. This must pass before trusting any real verdict --
// see the plan's step 2.3.

const fs = require('fs');
const path = require('path');

// ---------- linear algebra: just enough for 3x3 symmetric eigendecomposition ----------

// Jacobi eigenvalue algorithm for a 3x3 symmetric matrix. Simple, robust,
// more than accurate enough for PCA on a few thousand points -- no need for
// a full numerical library for a 3x3 case.
function jacobiEigen3x3(A) {
  // A is a 3x3 array of arrays, symmetric.
  let a = A.map(row => row.slice());
  let v = [[1, 0, 0], [0, 1, 0], [0, 0, 1]];

  function offDiagNorm(m) {
    return Math.abs(m[0][1]) + Math.abs(m[0][2]) + Math.abs(m[1][2]);
  }

  for (let iter = 0; iter < 100 && offDiagNorm(a) > 1e-12; iter++) {
    // Find largest off-diagonal element
    let p = 0, q = 1, max = Math.abs(a[0][1]);
    if (Math.abs(a[0][2]) > max) { max = Math.abs(a[0][2]); p = 0; q = 2; }
    if (Math.abs(a[1][2]) > max) { max = Math.abs(a[1][2]); p = 1; q = 2; }
    if (max < 1e-15) break;

    const app = a[p][p], aqq = a[q][q], apq = a[p][q];
    const phi = 0.5 * Math.atan2(2 * apq, aqq - app);
    const c = Math.cos(phi), s = Math.sin(phi);

    const newApp = c * c * app - 2 * s * c * apq + s * s * aqq;
    const newAqq = s * s * app + 2 * s * c * apq + c * c * aqq;
    a[p][p] = newApp;
    a[q][q] = newAqq;
    a[p][q] = a[q][p] = 0;

    for (let k = 0; k < 3; k++) {
      if (k !== p && k !== q) {
        const akp = a[k][p], akq = a[k][q];
        a[k][p] = a[p][k] = c * akp - s * akq;
        a[k][q] = a[q][k] = s * akp + c * akq;
      }
      const vkp = v[k][p], vkq = v[k][q];
      v[k][p] = c * vkp - s * vkq;
      v[k][q] = s * vkp + c * vkq;
    }
  }

  const eigenvalues = [a[0][0], a[1][1], a[2][2]];
  const eigenvectors = [
    [v[0][0], v[1][0], v[2][0]],
    [v[0][1], v[1][1], v[2][1]],
    [v[0][2], v[1][2], v[2][2]],
  ];
  // Sort descending by eigenvalue
  const order = [0, 1, 2].sort((i, j) => eigenvalues[j] - eigenvalues[i]);
  return {
    values: order.map(i => eigenvalues[i]),
    vectors: order.map(i => eigenvectors[i]),
  };
}

function computeStats(points) {
  const n = points.length;
  const mean = [0, 0, 0];
  for (const p of points) { mean[0] += p[0]; mean[1] += p[1]; mean[2] += p[2]; }
  mean[0] /= n; mean[1] /= n; mean[2] /= n;

  const cov = [[0, 0, 0], [0, 0, 0], [0, 0, 0]];
  for (const p of points) {
    const d = [p[0] - mean[0], p[1] - mean[1], p[2] - mean[2]];
    for (let i = 0; i < 3; i++)
      for (let j = 0; j < 3; j++)
        cov[i][j] += d[i] * d[j];
  }
  for (let i = 0; i < 3; i++)
    for (let j = 0; j < 3; j++)
      cov[i][j] /= n;

  const eig = jacobiEigen3x3(cov);

  // Axis-aligned bbox (in the ORIGINAL frame, not PCA-rotated) -- cheap and
  // sufficient for the ratio/top-bottom checks, which only care about the
  // model's dominant up axis (already known: Z for the exporter's native
  // frame, Y after undoing nif_to_obj.py's axis remap).
  const min = [Infinity, Infinity, Infinity];
  const max = [-Infinity, -Infinity, -Infinity];
  for (const p of points) {
    for (let i = 0; i < 3; i++) {
      if (p[i] < min[i]) min[i] = p[i];
      if (p[i] > max[i]) max[i] = p[i];
    }
  }
  const size = [max[0] - min[0], max[1] - min[1], max[2] - min[2]];

  return { mean, cov, eig, min, max, size, n };
}

// ---------- OBJ parsing ----------

function parseObj(filePath) {
  const text = fs.readFileSync(filePath, 'utf8');
  const points = [];
  for (const line of text.split('\n')) {
    if (line.startsWith('v ')) {
      const parts = line.trim().split(/\s+/);
      points.push([parseFloat(parts[1]), parseFloat(parts[2]), parseFloat(parts[3])]);
    }
  }
  return points;
}

// nif_to_obj.py imports with axis_forward='Z', axis_up='Y', scale_correction=1.0
// (overriding blender_niftools_addon's own default of axis_up='-Y' plus a 0.1
// scale_correction -- Blender's Y-up/meters convention) and exports with
// forward_axis='Y', up_axis='Z' -- both identity remaps for a Z-up, unscaled
// source. The resulting .obj is therefore already in the exact same native
// Z-up frame and units as this exporter's own .gfmodel/.gfbin, so no inverse
// transform is needed here. Kept as a named no-op (rather than inlining
// objPoints directly) so a future change to nif_to_obj.py's axis/scale
// choices has one obvious place to update the corresponding correction.
function objToNativeFrame(p) {
  return p;
}

// ---------- gfmodel/.gfbin reading ----------

function readAccessor(buf, accessor) {
  const { byteOffset, itemSize, count, type } = accessor;
  const out = new Array(count);
  const readers = {
    float32: (off) => buf.readFloatLE(off),
    uint16: (off) => buf.readUInt16LE(off),
    uint32: (off) => buf.readUInt32LE(off),
  };
  const bytesPerElem = { float32: 4, uint16: 2, uint32: 4 }[type];
  const read = readers[type];
  for (let i = 0; i < count; i++) {
    const row = new Array(itemSize);
    for (let j = 0; j < itemSize; j++) {
      row[j] = read(byteOffset + (i * itemSize + j) * bytesPerElem);
    }
    out[i] = row;
  }
  return out;
}

function multiplyPointByColumnMajor4x4(p, m) {
  // m is column-major 16-element array (glTF/Three.js convention, matches
  // ToColumnMajor throughout this exporter).
  const x = p[0], y = p[1], z = p[2];
  return [
    m[0] * x + m[4] * y + m[8] * z + m[12],
    m[1] * x + m[5] * y + m[9] * z + m[13],
    m[2] * x + m[6] * y + m[10] * z + m[14],
  ];
}

// Reconstructs bind-pose WORLD-SPACE vertex positions for every mesh in a
// .gfmodel, using the mesh's own already-baked per-vertex `skinMatrix`
// (weighted blend over up to 4 joints) directly -- NOT by walking the bone
// hierarchy's `bindMatrixLocal`. This is a deliberate, important scope limit:
//
// The viewer computes `boneInverses[i] = bone.matrixWorld.invert() *
// skinMatrix` (tools/viewer/index.html, buildScene) at LOAD TIME, from
// whatever `bone.matrixWorld` the current `bindMatrixLocal` produces. This
// makes the very first rendered frame (bind pose, no clip playing)
// mathematically forced to reproduce `skinMatrix` exactly, REGARDLESS of
// whether `bindMatrixLocal` is correct -- any bindMatrixLocal error is
// invisible at bind pose and only shows up once an animation clip moves a
// bone away from it (the wrong rest lengths/offsets in the hierarchy then
// misplace limbs during playback). Reconstructing from `skinMatrix` here (as
// this function does) therefore checks the SAME thing the viewer's bind-pose
// frame checks -- it CANNOT detect a `bindMatrixLocal` defect, only a defect
// in the skin matrices themselves or in the raw mesh geometry.
//
// This is why this harness alone was not enough to catch the monster/M009
// regression from ReconstructBindPoseFromSkin (SkeletonExtractor) during this
// session's own use of it: M009 verdicts OK here even while the bone
// hierarchy it exports is broken, because the defect only shows up once a
// clip actually plays. Do not read an OK verdict as "animation-safe" -- it
// only means bind-pose geometry and skin matrices agree with the reference.
// A real per-clip check would need to replay a track's bone deltas through
// `bindMatrixLocal` and compare posed vertex positions, which is out of
// scope for this pass (see the plan's step 3a: bindMatrixLocal is isolated
// and A/B tested directly instead, not through this harness).
//
// A mesh with nodeIndex >= 0 (animated-mesh reattachment, PHASE4_FINDINGS
// §14.3) is skipped here for the same reason as above: its vertices are
// node-local by design and meaningless without walking scene.nodes.
function loadGfModelPoints(gfmodelPath) {
  const model = JSON.parse(fs.readFileSync(gfmodelPath, 'utf8'));
  const gfbinPath = path.join(path.dirname(gfmodelPath), model.binary);
  const buf = fs.readFileSync(gfbinPath);

  const points = [];
  let skippedReattached = 0;

  for (const mesh of model.meshes) {
    const positions = readAccessor(buf, mesh.attributes.position);

    if (mesh.nodeIndex >= 0) {
      skippedReattached++;
      continue;
    }

    if (mesh.isSkinned && mesh.skinBindings && mesh.skinBindings.length) {
      const skinIndex = readAccessor(buf, mesh.attributes.skinIndex);
      const skinWeight = readAccessor(buf, mesh.attributes.skinWeight);
      const boneMatrices = mesh.skinBindings.map(b => b.skinMatrix);

      for (let i = 0; i < positions.length; i++) {
        const idx = skinIndex[i], wt = skinWeight[i];
        let acc = [0, 0, 0];
        let wsum = 0;
        for (let j = 0; j < 4; j++) {
          const w = wt[j];
          if (w <= 0) continue;
          const bindingIdx = idx[j];
          if (bindingIdx >= boneMatrices.length) continue;
          const world = multiplyPointByColumnMajor4x4(positions[i], boneMatrices[bindingIdx]);
          acc[0] += w * world[0];
          acc[1] += w * world[1];
          acc[2] += w * world[2];
          wsum += w;
        }
        if (wsum > 0) {
          points.push([acc[0] / wsum, acc[1] / wsum, acc[2] / wsum]);
        } else {
          points.push(positions[i]); // unweighted vertex, rare fallback
        }
      }
    } else {
      // Unskinned, not reattached: already world-space (Phase 2 flattening).
      for (const p of positions) points.push(p);
    }
  }

  return { points, skippedReattached, meshCount: model.meshes.length };
}

// ---------- verdict ----------

const ROTATION_TOL_DEG = 12; // principal-axis misalignment beyond this is flagged
const RATIO_TOL = 0.35;      // relative dimension mismatch beyond this is flagged
const TOPBOTTOM_TOL = 0.35;  // relative asymmetry mismatch beyond this is flagged

function angleBetweenDeg(a, b) {
  const dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  const na = Math.hypot(...a), nb = Math.hypot(...b);
  let cos = dot / (na * nb);
  cos = Math.max(-1, Math.min(1, cos));
  const deg = Math.acos(Math.abs(cos)) * 180 / Math.PI; // axis has no sign
  return deg;
}

// An axis pair is only meaningful to compare when its own eigenvalue is
// clearly separated from its neighbours in BOTH clouds -- otherwise PCA is
// free to pick either of two near-tied directions arbitrarily (a boxy prop
// like a chair or cart has no single well-defined "long axis" the way an
// elongated character does), and comparing such an axis produces a false
// "rotated" verdict on a shape that is not actually rotated. Require each
// eigenvalue to be at least 15% larger than the next one down to trust the
// axis built from it.
function axisIsWellSeparated(values, i) {
  const next = values[i + 1];
  if (next === undefined) return true; // smallest axis has nothing after it
  const denom = Math.max(values[i], 1e-9);
  return (values[i] - next) / denom > 0.15;
}

function compareOne(objPoints, gfPoints) {
  const objNative = objPoints.map(objToNativeFrame);
  const refStats = computeStats(objNative);
  const expStats = computeStats(gfPoints);

  // Principal axis alignment: compare ONLY the dominant (largest-eigenvalue)
  // axis direction between the two point clouds, in the shared native Z-up
  // frame. This is deliberately restricted to axis 0, not all three:
  // secondary/tertiary axes on an asymmetric bind pose (e.g. one arm/leg
  // posed slightly differently between the two independent pipelines, or a
  // near-symmetric footprint where PCA has no strong preference) swap or
  // drift by tens of degrees on files with no visible orientation problem at
  // all (measured directly on monster/M005 during this harness's own
  // development: axis 0 agreed to 3.3 deg while axis 1 disagreed by 20+ deg
  // on a file with no reported defect) -- axis 0 is what actually answers
  // "is the model's long axis pointing the same way", which is what a
  // visible rotation/lying-on-its-side defect changes. Still gated by
  // axisIsWellSeparated: a shape with no clear dominant axis (a flat disc, a
  // perfect cube) can't give a meaningful angle at all.
  const axis0Trusted = axisIsWellSeparated(refStats.eig.values, 0) && axisIsWellSeparated(expStats.eig.values, 0);
  const maxAxisAngle = axis0Trusted ? angleBetweenDeg(refStats.eig.vectors[0], expStats.eig.vectors[0]) : 0;
  const anyAxisTrusted = axis0Trusted;

  // Dimension ratio per axis (bbox size), relative difference.
  const dimRatios = [0, 1, 2].map(i => {
    const r = refStats.size[i], e = expStats.size[i];
    const denom = Math.max(r, e, 1e-6);
    return Math.abs(r - e) / denom;
  });
  const maxDimRatioErr = Math.max(...dimRatios);

  // Top/bottom asymmetry along the vertical (Z, native frame) axis: the
  // fraction of total Z-extent that lies above vs below the centroid. A
  // flipped model swaps which side is "heavier".
  function verticalAsymmetry(stats, points) {
    const zc = stats.mean[2];
    let above = 0, below = 0;
    for (const p of points) {
      if (p[2] >= zc) above++; else below++;
    }
    return (above - below) / points.length;
  }
  const refAsym = verticalAsymmetry(refStats, objNative);
  const expAsym = verticalAsymmetry(expStats, gfPoints);
  const asymDiff = Math.abs(refAsym - expAsym);

  let verdict = 'OK';
  const reasons = [];
  if (anyAxisTrusted && maxAxisAngle > ROTATION_TOL_DEG) {
    verdict = 'ROTATED';
    reasons.push(`principal axis misaligned by ${maxAxisAngle.toFixed(1)} deg`);
  }
  if (maxDimRatioErr > RATIO_TOL) {
    verdict = verdict === 'OK' ? 'SCALE_MISMATCH' : verdict + '+SCALE_MISMATCH';
    reasons.push(`dimension ratio mismatch ${(maxDimRatioErr * 100).toFixed(0)}%`);
  }
  if (asymDiff > TOPBOTTOM_TOL) {
    verdict = verdict === 'OK' ? 'FLIPPED' : verdict + '+FLIPPED';
    reasons.push(`vertical asymmetry mismatch (ref ${refAsym.toFixed(2)} vs export ${expAsym.toFixed(2)})`);
  }
  if (!anyAxisTrusted) {
    reasons.push('no PCA axis well-separated in both clouds; rotation check skipped (shape too symmetric/boxy)');
  }

  return {
    verdict,
    reasons,
    maxAxisAngle,
    anyAxisTrusted,
    refSize: refStats.size,
    expSize: expStats.size,
    maxDimRatioErr,
    refAsym,
    expAsym,
  };
}

// ---------- self-test ----------

function rotatePointsX90(points) {
  // Rotate 90 degrees about the X axis: (x,y,z) -> (x,-z,y)
  return points.map(([x, y, z]) => [x, -z, y]);
}

// Deterministic PRNG (mulberry32) so the self-test is reproducible run to
// run -- Math.random() previously made pass/fail flaky (see git history:
// a near-tie between the two smaller PCA eigenvalues on a symmetric X/Y
// footprint let their eigenvectors swap order arbitrarily between runs).
function mulberry32(seed) {
  return function () {
    seed |= 0; seed = (seed + 0x6D2B79F5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function selfTest() {
  // Synthetic "reference" point cloud: a clearly asymmetric blob (tall along
  // Z, medium along X, narrow along Y -- three well-separated dimensions, so
  // PCA's principal axes are unambiguous and don't depend on tie-breaking),
  // standing in for a standing character model in the native Z-up frame.
  const rand = mulberry32(12345);
  const synthetic = [];
  for (let i = 0; i < 2000; i++) {
    const z = rand() * 2.0;              // tall along Z
    const x = (rand() - 0.5) * 0.8;      // medium along X
    const y = (rand() - 0.5) * 0.2;      // narrow along Y
    synthetic.push([x, y, z]);
  }

  // "Reference" as if it came from the OBJ path: objToNativeFrame is the
  // identity (nif_to_obj.py's import/export axis settings are chosen so the
  // .obj is already in the native Z-up frame, see objToNativeFrame's own
  // comment), so the "OBJ-frame" input to compareOne is just the synthetic
  // cloud itself here.
  const objFrame = synthetic;

  // Case 1: exporter output identical to reference (in native frame) -> OK
  const identical = compareOne(objFrame, synthetic);

  // Case 2: exporter output rotated 90 deg about X relative to reference -> ROTATED
  const rotated = compareOne(objFrame, rotatePointsX90(synthetic));

  console.log('Self-test case 1 (identical):', identical.verdict, identical.reasons);
  console.log('Self-test case 2 (90deg X rotation):', rotated.verdict, rotated.reasons,
    `(max axis angle ${rotated.maxAxisAngle.toFixed(1)} deg)`);

  const pass1 = identical.verdict === 'OK';
  const pass2 = rotated.verdict.includes('ROTATED') && rotated.maxAxisAngle > 60;

  if (!pass1) console.error('SELF-TEST FAILED: identical case did not verdict OK');
  if (!pass2) console.error('SELF-TEST FAILED: 90-degree rotation was not detected');

  return pass1 && pass2;
}

// ---------- main ----------

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
    console.error('Usage: node compare_orientation.js <obj_dir> <gfmodel_dir> [--json out.json]');
    process.exit(1);
  }
  const [objDir, gfmodelDir] = args;
  const jsonIdx = args.indexOf('--json');
  const jsonOut = jsonIdx >= 0 ? args[jsonIdx + 1] : null;

  const objFiles = fs.readdirSync(objDir).filter(f => f.toLowerCase().endsWith('.obj'));
  const results = {};

  for (const objFile of objFiles) {
    const baseName = path.basename(objFile, '.obj');
    const gfmodelPath = findGfModel(gfmodelDir, baseName);
    if (!gfmodelPath) {
      results[baseName] = { verdict: 'NO_GFMODEL_FOUND' };
      console.log(`${baseName}: NO_GFMODEL_FOUND`);
      continue;
    }
    try {
      const objPoints = parseObj(path.join(objDir, objFile));
      const { points: gfPoints, skippedReattached, meshCount } = loadGfModelPoints(gfmodelPath);
      if (gfPoints.length === 0) {
        results[baseName] = { verdict: 'NO_COMPARABLE_GEOMETRY', skippedReattached, meshCount };
        console.log(`${baseName}: NO_COMPARABLE_GEOMETRY (skippedReattached=${skippedReattached}/${meshCount})`);
        continue;
      }
      const cmp = compareOne(objPoints, gfPoints);
      results[baseName] = { ...cmp, skippedReattached, meshCount, objVerts: objPoints.length, gfVerts: gfPoints.length };
      console.log(`${baseName}: ${cmp.verdict}` + (cmp.reasons.length ? ` (${cmp.reasons.join('; ')})` : ''));
    } catch (e) {
      results[baseName] = { verdict: 'ERROR', error: String(e) };
      console.log(`${baseName}: ERROR - ${e}`);
    }
  }

  if (jsonOut) {
    fs.writeFileSync(jsonOut, JSON.stringify(results, null, 2));
    console.log(`\nWrote ${jsonOut}`);
  }
}

main();
