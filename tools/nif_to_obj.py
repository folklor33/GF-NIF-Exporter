# nif_to_obj.py
import bpy
import os
import sys
import traceback
import mathutils
import math

def clean_scene():
    """Nettoie complètement la scène Blender"""
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False, confirm=False)
    
    for collection in [bpy.data.meshes, bpy.data.materials, bpy.data.textures, 
                       bpy.data.images, bpy.data.armatures, bpy.data.actions,
                       bpy.data.curves, bpy.data.lights, bpy.data.cameras]:
        for block in collection:
            if block.users == 0:
                collection.remove(block)

def remove_armatures_and_animations():
    """Supprime toutes les armatures et animations"""
    armatures = [obj for obj in bpy.data.objects if obj.type == 'ARMATURE']
    for arm in armatures:
        bpy.data.objects.remove(arm, do_unlink=True)
    
    for action in bpy.data.actions:
        bpy.data.actions.remove(action)
    
    for obj in bpy.data.objects:
        if obj.type == 'MESH':
            for modifier in obj.modifiers:
                if modifier.type == 'ARMATURE':
                    obj.modifiers.remove(modifier)


def get_depth_from_root(obj):
    """Calcule la profondeur dans la hiérarchie"""
    depth = 0
    parent = obj.parent
    while parent:
        depth += 1
        parent = parent.parent
    return depth

def keep_and_merge_valid_meshes():
    """Fusionne tous les meshes présents sans filtrage"""
    all_meshes = [obj for obj in bpy.data.objects if obj.type == 'MESH' and obj.data]
    
    if not all_meshes:
        return False, None
    
    print(f"  → Total meshes: {len(all_meshes)}")
    
    # Garder TOUS les meshes
    for obj in all_meshes:
        vertex_count = len(obj.data.vertices)
        print(f"    ✓ Gardé: {obj.name} ({vertex_count}v)")
    
    if len(all_meshes) == 1:
        main_mesh = all_meshes[0]
        bpy.ops.object.select_all(action='DESELECT')
        main_mesh.select_set(True)
        bpy.context.view_layer.objects.active = main_mesh
        return True, main_mesh
    
    # Fusionner TOUS les meshes
    print(f"  → Fusion de {len(all_meshes)} meshes...")
    bpy.ops.object.select_all(action='DESELECT')
    for mesh in all_meshes:
        mesh.select_set(True)
    
    bpy.context.view_layer.objects.active = all_meshes[0]
    bpy.ops.object.join()
    
    return True, bpy.context.view_layer.objects.active

def center_mesh_only(obj):
    """Centre le mesh en X/Y et le pose sur le sol (Z=0)"""
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.mode_set(mode='OBJECT')
    
    # 1. Appliquer toutes les transformations existantes
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    
    # 2. Calculer la bounding box
    bbox_corners = [mathutils.Vector(corner) for corner in obj.bound_box]
    
    # Centre X/Y
    center_x = sum(v.x for v in bbox_corners) / 8
    center_y = sum(v.y for v in bbox_corners) / 8
    
    # Z minimum (le point le plus bas du modèle)
    min_z = min(v.z for v in bbox_corners)
    
    # 3. Déplacer les vertices :
    #    - Centrer en X et Y
    #    - Remonter pour que le bas soit à Z=0
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.transform.translate(value=(-center_x, -center_y, -min_z))
    bpy.ops.object.mode_set(mode='OBJECT')
    
    # Recalculer les dimensions pour info
    bbox = [mathutils.Vector(corner) for corner in obj.bound_box]
    size_x = max(v.x for v in bbox) - min(v.x for v in bbox)
    size_y = max(v.y for v in bbox) - min(v.y for v in bbox)
    size_z = max(v.z for v in bbox) - min(v.z for v in bbox)
    max_dimension = max(size_x, size_y, size_z)
    
    return max_dimension

def main():
    argv = sys.argv
    argv = argv[argv.index("--") + 1:] if "--" in argv else []
    
    if len(argv) < 2:
        print("ERREUR: Arguments manquants!")
        sys.exit(1)
    
    input_directory = os.path.abspath(argv[0])
    output_directory = os.path.abspath(argv[1])
    
    print(f"Dossier d'entrée: {input_directory}")
    print(f"Dossier de sortie: {output_directory}")
    
    if not os.path.exists(input_directory):
        print(f"ERREUR: Le dossier d'entrée n'existe pas")
        sys.exit(1)
    
    if not os.path.exists(output_directory):
        os.makedirs(output_directory)
    
    if "io_scene_niftools" not in bpy.context.preferences.addons:
        print("ERREUR: L'addon NifTools n'est pas activé!")
        sys.exit(1)
    
    nif_files = [f for f in os.listdir(input_directory) if f.lower().endswith(".nif")]
    
    if not nif_files:
        print("ERREUR: Aucun fichier .nif trouvé!")
        sys.exit(1)
    
    total_files = len(nif_files)
    print(f"\nTrouvé {total_files} fichier(s) .nif à traiter\n")
    
    success_count = 0
    error_count = 0
    errors_detail = []
    
    for idx, nif_file in enumerate(nif_files, 1):
        nif_path = os.path.join(input_directory, nif_file)
        obj_name = os.path.splitext(nif_file)[0]
        obj_output_path = os.path.join(output_directory, f"{obj_name}.obj")
        
        print(f"[{idx}/{total_files}] Traitement: {nif_file}")
        
        print(f"[{idx}/{total_files}] Traitement: {nif_file}")
        
        try:
            clean_scene()
            
            print(f"  → Import du fichier NIF...")
            try:
                bpy.ops.import_scene.nif(filepath=nif_path, animation=False)
            except Exception as import_error:
                print(f"  ⚠ Erreur d'import partielle: {str(import_error)}")
                # Continue quand même si des meshes ont été importés
                if len(bpy.data.objects) == 0:
                    raise Exception(f"Import échoué complètement: {str(import_error)}")
            
            meshes_imported = [obj for obj in bpy.data.objects if obj.type == 'MESH']
            if len(meshes_imported) == 0:
                raise Exception("Aucun mesh importé")
            
            print(f"  → Suppression des animations...")
            remove_armatures_and_animations()
            
            print(f"  → Fusion des meshes...")
            success, main_obj = keep_and_merge_valid_meshes()
            if not success or not main_obj:
                raise Exception("Aucun mesh valide trouvé")
            
            vertex_count = len(main_obj.data.vertices)
            
            print(f"  → Centrage du mesh...")
            max_dimension = center_mesh_only(main_obj)
            print(f"  → Mesh: {vertex_count} vertices, taille max: {max_dimension:.3f}")
            
            print(f"  → Export OBJ...")
            bpy.ops.export_scene.obj(
                filepath=obj_output_path,
                use_selection=True,
                use_materials=False,
                use_triangles=False,
                use_normals=True,
                use_uvs=True,
                keep_vertex_order=True,
                axis_forward='-Z',
                axis_up='Y',
                global_scale=1.0,
                path_mode='AUTO'
            )
            
            if os.path.exists(obj_output_path):
                file_size = os.path.getsize(obj_output_path)
                if file_size > 0:
                    print(f"  ✓ Exporté: {obj_output_path} ({file_size} octets)")
                    success_count += 1
                else:
                    raise Exception("Fichier vide")
            else:
                raise Exception("Fichier non créé")
            
        except Exception as e:
            error_msg = f"  ✗ Échec: {str(e)}"
            print(error_msg)
            errors_detail.append(f"{nif_file}: {str(e)}")
            error_count += 1
            
            if "--verbose" in sys.argv:
                traceback.print_exc()
    
    print(f"\n{'='*50}")
    print("Export terminé!")
    print(f"Succès: {success_count}/{total_files}")
    print(f"Erreurs: {error_count}/{total_files}")
    
    if errors_detail:
        print(f"\nDétail des erreurs:")
        for error in errors_detail:
            print(f"  - {error}")
    
    print(f"{'='*50}")
    sys.exit(0 if error_count == 0 else 1)

if __name__ == "__main__":
    main()