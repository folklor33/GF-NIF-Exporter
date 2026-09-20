# dump_bone_bind_pose.py
#
# Dumps each bone's rest-pose ARMATURE-SPACE (not parent-local) transform, as
# imported by blender_niftools_addon, to a JSON file: {bone_name: [16 floats,
# row-major]}. Used by tools/diag/compare_bind_pose.js to check the exporter's
# own SkeletonExtractor::bindMatrixLocal against an independent reference,
# directly -- unlike tools/diag/compare_orientation.js's mesh-geometry check,
# this can actually see a bindMatrixLocal defect (see that script's own doc
# comment for why it can't: THREE.SkinnedMesh's boneInverses are computed FROM
# the current bone matrices at load time, so a bindMatrixLocal error is
# invisible in the mesh at bind pose and only shows up once an animation clip
# moves a bone away from it).
#
# Same identity axis/scale import settings as nif_to_obj.py, so the dumped
# matrices are directly comparable (no remap needed) to the exporter's own
# bindMatrixLocal, reconstructed into armature-space world matrices the same
# way SkeletonExtractor/the viewer do (parent-before-child chain multiply).
#
# Usage: blender -b --python dump_bone_bind_pose.py -- <nif_path> <json_out_path>

import bpy
import sys
import json
import os

def clean_scene():
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False, confirm=False)
    for collection in [bpy.data.meshes, bpy.data.materials, bpy.data.textures,
                       bpy.data.images, bpy.data.armatures, bpy.data.actions,
                       bpy.data.curves, bpy.data.lights, bpy.data.cameras]:
        for block in collection:
            if block.users == 0:
                collection.remove(block)

def main():
    argv = sys.argv
    argv = argv[argv.index("--") + 1:] if "--" in argv else []
    if len(argv) < 2:
        print("ERREUR: usage: dump_bone_bind_pose.py -- <nif_path> <json_out_path>")
        sys.exit(1)

    nif_path = os.path.abspath(argv[0])
    json_out_path = os.path.abspath(argv[1])

    clean_scene()

    bpy.ops.import_scene.nif(
        filepath=nif_path,
        animation=False,
        axis_forward='Z',
        axis_up='Y',
        scale_correction=1.0,
    )

    armatures = [obj for obj in bpy.data.objects if obj.type == 'ARMATURE']
    if not armatures:
        print(f"NO_ARMATURE: {nif_path}")
        with open(json_out_path, 'w') as f:
            json.dump({}, f)
        sys.exit(0)

    arm = armatures[0]
    bones = {}
    for bone in arm.data.bones:
        # matrix_local is armature-space (relative to the armature object's
        # own origin, not parent-bone-relative) -- exactly what
        # SkeletonExtractor's bindMatrixLocal chain-multiplies up to, and
        # exactly what compare_bind_pose.js reconstructs from the exported
        # bindMatrixLocal for comparison.
        m = bone.matrix_local
        # Blender matrices are column-major internally but m[i][j] indexes
        # row i, column j (row-vector-of-rows access) -- flatten row-major
        # here and let compare_bind_pose.js transpose if it wants
        # column-major, documented explicitly there.
        flat = [m[i][j] for i in range(4) for j in range(4)]
        bones[bone.name] = flat

    with open(json_out_path, 'w') as f:
        json.dump(bones, f)

    print(f"Dumped {len(bones)} bones from {nif_path} to {json_out_path}")

if __name__ == "__main__":
    main()
