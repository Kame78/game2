"""Blender headless decimation for the necro-dagger.

Run with:  blender --background --python decimate.py -- <src.glb> <dst.glb> <target_tris>
Produces a decimated GLB with baked-in materials that raylib's LoadModel can handle
(vertex count kept well under the 65,535 index limit).
"""

import bpy
import os
import sys


def main():
    argv = sys.argv
    if "--" not in argv:
        print("usage: decimate.py -- <src> <dst> <target_tris>")
        return 1
    args = argv[argv.index("--") + 1:]
    src, dst, target = args[0], args[1], int(args[2])

    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=src)

    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    if not meshes:
        print("no mesh objects found")
        return 2

    obj = max(meshes, key=lambda o: len(o.data.polygons))
    bpy.context.view_layer.objects.active = obj

    tris = sum(len(p.vertices) - 2 for p in obj.data.polygons)
    ratio = min(1.0, max(0.01, target / max(1, tris)))
    print(f"input tris={tris}, target={target}, ratio={ratio:.4f}")

    mod = obj.modifiers.new(name="decim", type="DECIMATE")
    mod.decimate_type = "COLLAPSE"
    mod.ratio = ratio
    mod.use_collapse_triangulate = True

    bpy.ops.object.modifier_apply(modifier=mod.name)
    print(f"output tris={len(obj.data.polygons)}, verts={len(obj.data.vertices)}")

    out_dir = os.path.dirname(dst) or "."
    os.makedirs(out_dir, exist_ok=True)

    bpy.ops.export_scene.gltf(
        filepath=dst,
        export_format="GLB",
        use_selection=False,
        export_apply=True,
        export_yup=True,
        export_texcoords=True,
        export_normals=True,
        export_materials="EXPORT",
        export_image_format="AUTO",
    )
    print(f"wrote {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
