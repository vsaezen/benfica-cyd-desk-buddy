"""Pack every Rev. G part onto one Bambu Lab A1 build plate."""
import os
import zipfile
import xml.etree.ElementTree as ET

from make_3mf import EXPORT, content_types, read_binary_stl, rels


# Plate coordinates are in millimetres on a 256 x 256 mm A1 bed.
#
# - Body: lower left, already front-down.
# - Screen faceplate and speaker ring: above the body.
# - Rear panel: rotated 90 degrees at the lower right.
# - Rear seam ring: upper right.
#
# Every outer edge retains at least 8 mm of bed margin. Adjacent solid parts
# retain at least 2 mm. Bounding boxes do not overlap; Bambu Studio otherwise
# assigns nested, non-touching rings to a second virtual plate.
ONE_PLATE_PARTS = [
    ("01_main_body_FRONT_DOWN_NO_SUPPORT.stl", "Main body front-down",
     "1 0 0 0 1 0 0 0 1 82 96.77 0"),
    ("02_rear_panel.stl", "Rear panel rotated",
     "0 1 0 0 0 -1 1 0 0 211 80 1.5"),
    ("03_screen_trim.stl", "Thick screen accent rotated",
     "0 1 0 -1 0 0 0 0 1 42.5 176 0"),
    ("05_speaker_trim.stl", "Speaker accent",
     "1 0 0 0 1 0 0 0 1 103.6 147.6 0"),
    ("04_rear_seam_trim.stl", "Rear seam accent",
     "1 0 0 0 1 0 0 0 1 176.9 211.9 0"),
]


def transform_vertex(vertex, transform):
    """Apply a 3MF row-vector affine transform to one XYZ vertex."""
    values = [float(value) for value in transform.split()]
    x, y, z = vertex
    return (
        x*values[0] + y*values[3] + z*values[6] + values[9],
        x*values[1] + y*values[4] + z*values[7] + values[10],
        x*values[2] + y*values[5] + z*values[8] + values[11],
    )


def write_combined_one_plate(parts, title, output_name):
    """Write disconnected parts as one mesh so Bambu cannot auto-split them."""
    vertices = []
    triangles = []
    for filename, _name, transform in parts:
        part_vertices, part_triangles = read_binary_stl(
            os.path.join(EXPORT, filename))
        base = len(vertices)
        vertices.extend(transform_vertex(vertex, transform)
                        for vertex in part_vertices)
        triangles.extend((a+base, b+base, c+base)
                         for a, b, c in part_triangles)

    ns = "http://schemas.microsoft.com/3dmanufacturing/core/2015/02"
    ET.register_namespace("", ns)
    model = ET.Element(f"{{{ns}}}model", {
        "unit": "millimeter", "xml:lang": "es-ES"})
    metadata = ET.SubElement(model, f"{{{ns}}}metadata", {"name": "Title"})
    metadata.text = title
    resources = ET.SubElement(model, f"{{{ns}}}resources")
    obj = ET.SubElement(resources, f"{{{ns}}}object", {
        "id": "1", "type": "model", "name": "All Rev G parts - one A1 plate"})
    mesh = ET.SubElement(obj, f"{{{ns}}}mesh")
    xml_vertices = ET.SubElement(mesh, f"{{{ns}}}vertices")
    for x, y, z in vertices:
        ET.SubElement(xml_vertices, f"{{{ns}}}vertex", {
            "x": str(x), "y": str(y), "z": str(z)})
    xml_triangles = ET.SubElement(mesh, f"{{{ns}}}triangles")
    for a, b, c in triangles:
        ET.SubElement(xml_triangles, f"{{{ns}}}triangle", {
            "v1": str(a), "v2": str(b), "v3": str(c)})
    build = ET.SubElement(model, f"{{{ns}}}build")
    ET.SubElement(build, f"{{{ns}}}item", {"objectid": "1"})

    output = os.path.join(EXPORT, output_name)
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("[Content_Types].xml", content_types)
        archive.writestr("_rels/.rels", rels)
        archive.writestr("3D/3dmodel.model", ET.tostring(
            model, encoding="utf-8", xml_declaration=True))
    print(output)
    xs, ys, zs = zip(*vertices)
    print("Combined plate bounds: "
          f"X {min(xs):.2f}..{max(xs):.2f}, "
          f"Y {min(ys):.2f}..{max(ys):.2f}, "
          f"Z {min(zs):.2f}..{max(zs):.2f} mm")


write_combined_one_plate(
    ONE_PLATE_PARTS,
    "Benfica CYD Desk Buddy - Freenove 3.2 - REV G ALL PARTS ONE A1 PLATE",
    "Benfica_CYD_Desk_Buddy_Freenove_3P2_REV_G_ALL_IN_ONE_A1_PLATE.3mf")
