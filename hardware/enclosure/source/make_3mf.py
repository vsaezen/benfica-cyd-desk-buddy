"""Pack the two binary STL parts into a standards-compliant 3MF build plate."""
import os
import struct
import zipfile
import xml.etree.ElementTree as ET

HERE = os.path.dirname(__file__)
EXPORT = os.path.join(HERE, "export")
MAIN_PARTS = [
    # Rev. G body is already oriented with the exterior front on the bed. Its
    # aperture grows inward, so the lit-area mask remains open when sliced.
    ("01_main_body_FRONT_DOWN_NO_SUPPORT.stl", "Main body front-down", "1 0 0 0 1 0 0 0 1 76 94 0"),
    # The wider body and rear cover are stacked vertically on the A1 plate.
    ("02_rear_panel.stl", "Rear panel", "1 0 0 0 0 -1 0 1 0 76 163 1.5"),
]

TRIM_PARTS = [
    ("03_screen_trim.stl", "Thick screen accent", "1 0 0 0 1 0 0 0 1 60 125 0"),
    ("04_rear_seam_trim.stl", "Rear seam accent", "1 0 0 0 1 0 0 0 1 128 45 0"),
    ("05_speaker_trim.stl", "Speaker accent trim", "1 0 0 0 1 0 0 0 1 160 125 0"),
]


def read_binary_stl(path):
    with open(path, "rb") as f:
        f.read(80)
        count = struct.unpack("<I", f.read(4))[0]
        verts, tris, index = [], [], {}
        for _ in range(count):
            data = struct.unpack("<12fH", f.read(50))
            tri = []
            for p in (data[3:6], data[6:9], data[9:12]):
                key = tuple(round(v, 6) for v in p)
                if key not in index:
                    index[key] = len(verts)
                    verts.append(key)
                tri.append(index[key])
            tris.append(tuple(tri))
    return verts, tris


content_types = b'''<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
 <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
 <Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>
</Types>'''
rels = b'''<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
 <Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>
</Relationships>'''

def write_3mf(parts, title, output_name):
    ns = "http://schemas.microsoft.com/3dmanufacturing/core/2015/02"
    ET.register_namespace("", ns)
    model = ET.Element(f"{{{ns}}}model", {"unit": "millimeter", "xml:lang": "es-ES"})
    metadata = ET.SubElement(model, f"{{{ns}}}metadata", {"name": "Title"})
    metadata.text = title
    resources = ET.SubElement(model, f"{{{ns}}}resources")
    build = ET.SubElement(model, f"{{{ns}}}build")

    for oid, (filename, name, transform) in enumerate(parts, 1):
        verts, tris = read_binary_stl(os.path.join(EXPORT, filename))
        obj = ET.SubElement(resources, f"{{{ns}}}object", {
            "id": str(oid), "type": "model", "name": name})
        mesh = ET.SubElement(obj, f"{{{ns}}}mesh")
        vs = ET.SubElement(mesh, f"{{{ns}}}vertices")
        for x, y, z in verts:
            ET.SubElement(vs, f"{{{ns}}}vertex", {
                "x": str(x), "y": str(y), "z": str(z)})
        ts = ET.SubElement(mesh, f"{{{ns}}}triangles")
        for a, b, c in tris:
            ET.SubElement(ts, f"{{{ns}}}triangle", {
                "v1": str(a), "v2": str(b), "v3": str(c)})
        ET.SubElement(build, f"{{{ns}}}item", {
            "objectid": str(oid), "transform": transform})

    out = os.path.join(EXPORT, output_name)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("[Content_Types].xml", content_types)
        archive.writestr("_rels/.rels", rels)
        archive.writestr("3D/3dmodel.model", ET.tostring(
            model, encoding="utf-8", xml_declaration=True))
    print(out)


if __name__ == "__main__":
    write_3mf(
        MAIN_PARTS,
        "Benfica CYD Desk Buddy - Freenove 3.2 - REV G ASYMMETRIC LCD AND EASY CLOSE",
        "Benfica_CYD_Desk_Buddy_Freenove_3P2_REV_G_LCD_ALIGNED_EASY_CLOSE.3mf")
    write_3mf(
        TRIM_PARTS,
        "Benfica CYD Desk Buddy - Freenove 3.2 - REV G PREMIUM TRIMS",
        "Benfica_CYD_Desk_Buddy_Freenove_3P2_REV_G_PREMIUM_TRIMS.3mf")
