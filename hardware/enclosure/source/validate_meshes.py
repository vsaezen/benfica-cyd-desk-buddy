import bpy
import bmesh
import os
import glob

EXPORT = os.path.join(os.path.dirname(__file__), "export")
for path in sorted(glob.glob(os.path.join(EXPORT, "0*.stl"))):
    filename = os.path.basename(path)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    bpy.ops.wm.stl_import(filepath=path)
    obj = bpy.context.object
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    boundary = sum(1 for e in bm.edges if e.is_boundary)
    non_manifold = sum(1 for e in bm.edges if not e.is_manifold)
    print(f"{filename}: vertices={len(bm.verts)} faces={len(bm.faces)} boundary_edges={boundary} non_manifold_edges={non_manifold}")
    problem_edges = [e for e in bm.edges if not e.is_manifold]
    if problem_edges:
        centres = []
        for edge in problem_edges[:12]:
            centre = (edge.verts[0].co + edge.verts[1].co) * 0.5
            centres.append(tuple(round(value, 3) for value in centre))
        print(f"  first_problem_edge_centres={centres}")
    bm.free()
