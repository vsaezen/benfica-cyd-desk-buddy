import bpy
import bmesh
import math
import os
from mathutils import Vector

OUT = os.path.join(os.path.dirname(__file__), "export")
os.makedirs(OUT, exist_ok=True)

# Freenove FNK0114L_3P2 / E32R32P official mechanical drawing (mm)
PCB_W = 93.70          # landscape orientation
PCB_H = 55.04
HOLE_X = 86.70         # 93.70 - 2*3.50
HOLE_Z = 48.00
HOLE_D = 3.20
# The drawing distinguishes the touch-panel viewing area from the illuminated
# LCD active area.  The front mask must follow LCD AA (not RTP VA), otherwise
# part of the factory black border remains visible around the image.
LCD_ACTIVE_W = 64.80
LCD_ACTIVE_H = 48.60
LCD_BACKLIGHT_W = 77.70
LCD_BACKLIGHT_H = 55.04
TOUCH_OD_W = 77.30
TOUCH_OD_H = 54.64

# Purchased components / deliberately tolerant pockets (mm)
SPEAKER_D = 45.0
SPEAKER_T = 12.0
DRV_W = 18.0
DRV_H = 17.0
DRV_T = 2.0
MOTOR_D = 10.0
MOTOR_T = 2.7

# Rev. F adds a genuine cable service chamber at the USB-C side.  The extra
# width is intentional: a standard straight USB-C plug can be connected while
# the display is already resting in the case, and a low-profile 90-degree lead
# can turn towards the rear panel-mount outlet without being crushed.
CASE_W = 148.0
CASE_D = 92.0
CASE_H = 78.0
WALL = 2.8
FRONT_ANGLE = math.atan2(70.0, 48.0)  # 55.56°, exactly follows the case face

# Revision G: dimensions driven by the second physical fit test.
SCREEN_SPACER_D = 5.6       # compact annulus; clears PCB/glass perimeter
SCREEN_SPACER_DEPTH = 2.2   # stops at the PCB front face, never crosses it
SCREEN_CLEARANCE_D = 3.45   # M3 through-bolt, printed horizontally/inclined
SCREEN_HEAD_D = 6.3         # avoids tangency with the enlarged drop-in tunnel
SCREEN_RAMP_DEPTH = 0.72    # shallow integral transition, not a separate raised bezel
SCREEN_GLASS_FACE_INSET = 0.82  # only 0.10 mm behind the end of the ramp
SCREEN_PAD_INSET = 3.85     # 2.75..4.95 mm; ends at PCB front face
PCB_PREVIEW_INSET = 5.72    # PCB centre behind touch/LCD/tape stack
USB_SERVICE_INNER_X = -72.35  # leaves a 1.65 mm exterior side skin
USB_SERVICE_LOCAL_Z = 0.0
REAR_BOSS_X = CASE_W/2 - 11.0
REAR_BOSS_D = 7.6           # narrower so it clears the locating tongue
REAR_TONGUE_SIDE_GAP = 3.2  # per side; the Rev. F full ring still caught the shell
REAR_TONGUE_Z_GAP = 1.8     # top and bottom, measured prototype correction
REAR_TONGUE_DEPTH = 1.55    # only 0.80 mm protrudes beyond the panel inner face
REAR_TONGUE_WALL = 1.0       # five 0.20 mm lines; also clears the screw pads

# Rev. G: the module drops in from behind through an oversized, straight inner
# tunnel and simply rests on the back of the integral front mask.  There is no
# external pocket or indentation. The second printed test established an
# asymmetric real-world LCD position: extend the old opening 1.50 mm to the
# left and retract its right edge by 3.27 mm. The resulting 62.23 mm window is
# centred 2.385 mm left of the PCB and hides the measured dead strip.
SCREEN_FRONT_LEFT_DELTA = 1.50
SCREEN_FRONT_RIGHT_MASK = 3.27
SCREEN_FRONT_OPEN_W = (LCD_ACTIVE_W - 0.8) + SCREEN_FRONT_LEFT_DELTA - SCREEN_FRONT_RIGHT_MASK
SCREEN_FRONT_OPEN_H = LCD_ACTIVE_H - 0.8
SCREEN_FRONT_OPEN_X = -(SCREEN_FRONT_LEFT_DELTA + SCREEN_FRONT_RIGHT_MASK) / 2
# Clearance is based on the largest outline (LCD backlight), not only the
# slightly smaller touch foil: 1.20 mm per side for a reliable printed fit.
SCREEN_APERTURE_W = LCD_BACKLIGHT_W + 2.4
SCREEN_APERTURE_H = LCD_BACKLIGHT_H + 2.4
# The optional faceplate clears the complete module, while its rounded corner
# still overlaps the four countersunk screw heads.
SCREEN_TRIM_OPEN_W = LCD_BACKLIGHT_W + 2.1
SCREEN_TRIM_OPEN_H = LCD_BACKLIGHT_H + 2.0
SCREEN_TRIM_OUTER_HEIGHT = 4.60
SCREEN_TRIM_INNER_HEIGHT = 3.80
SCREEN_TRIM_HEAD_POCKET_D = 13.0
SCREEN_TRIM_HEAD_POCKET_DEPTH = 3.10


def clean():
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)


def mat(name, color):
    m = bpy.data.materials.new(name)
    m.diffuse_color = (*color, 1.0)
    return m


RED = mat("SLB red", (0.58, 0.015, 0.02))
BLACK = mat("Rear black", (0.025, 0.025, 0.03))
GREEN = mat("PCB preview", (0.03, 0.25, 0.12))
BLUE = mat("Screen preview", (0.02, 0.12, 0.28))
GOLD = mat("Premium accent", (0.72, 0.43, 0.06))


def box(name, size, loc=(0, 0, 0), material=None, bevel=0):
    bpy.ops.mesh.primitive_cube_add(location=loc)
    o = bpy.context.object
    o.name = name
    o.dimensions = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    if bevel:
        mod = o.modifiers.new("edge softness", 'BEVEL')
        mod.width = bevel
        mod.segments = 3
        bpy.context.view_layer.objects.active = o
        bpy.ops.object.modifier_apply(modifier=mod.name)
    if material:
        o.data.materials.append(material)
    return o


def cyl(name, radius, depth, loc=(0, 0, 0), rot=(0, 0, 0), material=None, verts=64):
    bpy.ops.mesh.primitive_cylinder_add(vertices=verts, radius=radius, depth=depth, location=loc, rotation=rot)
    o = bpy.context.object
    o.name = name
    if material:
        o.data.materials.append(material)
    return o


def rounded_rect_solid(name, width, height, depth, radius, material=None):
    """Rounded rectangle lying flat in XY, with an accurately flat underside."""
    parts = [
        box(name + " centre x", (width - 2*radius, height, depth), (0, 0, depth/2), material),
        box(name + " centre y", (width, height - 2*radius, depth), (0, 0, depth/2), material),
    ]
    for x in (-width/2 + radius, width/2 - radius):
        for y in (-height/2 + radius, height/2 - radius):
            parts.append(cyl(name + " corner", radius, depth, (x, y, depth/2), material=material))
    shape = parts[0]
    for feature in parts[1:]:
        boolean(shape, feature, 'UNION')
    shape.name = name
    return shape


def rounded_frame(name, outer_w, outer_h, inner_w, inner_h, depth, outer_r, inner_r, material):
    outer = rounded_rect_solid(name, outer_w, outer_h, depth, outer_r, material)
    inner = rounded_rect_solid(name + " opening", inner_w, inner_h, depth + 2.0, inner_r)
    inner.location.z -= 1.0
    boolean(outer, inner)
    return outer


def analytic_rounded_frame(name, outer_w, outer_h, inner_w, inner_h, depth, outer_r, inner_r, material):
    """Boolean-free rounded frame for ultra-thin decorative parts."""
    def loop(w, h, r, seg=12):
        pts = []
        corners = [
            (w/2-r, h/2-r, 0),
            (-w/2+r, h/2-r, 90),
            (-w/2+r, -h/2+r, 180),
            (w/2-r, -h/2+r, 270),
        ]
        for ci, (cx, cy, start) in enumerate(corners):
            count = seg - 1 if ci == len(corners) - 1 else seg
            for s in range(count):
                a = math.radians(start + 90*s/(seg-1))
                pts.append((cx + r*math.cos(a), cy + r*math.sin(a)))
        return pts

    outer = loop(outer_w, outer_h, outer_r)
    inner = loop(inner_w, inner_h, inner_r)
    n = len(outer)
    verts = []
    for z in (0.0, depth):
        verts.extend([(x, y, z) for x, y in outer])
        verts.extend([(x, y, z) for x, y in inner])
    faces = []
    for i in range(n):
        j = (i + 1) % n
        ob_i, ob_j = i, j
        ib_i, ib_j = n+i, n+j
        ot_i, ot_j = 2*n+i, 2*n+j
        it_i, it_j = 3*n+i, 3*n+j
        faces.extend([
            (ot_i, ot_j, it_j, it_i),
            (ob_j, ob_i, ib_i, ib_j),
            (ob_i, ob_j, ot_j, ot_i),
            (ib_j, ib_i, it_i, it_j),
        ])
    mesh = bpy.data.meshes.new(name + "Mesh")
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    obj.data.materials.append(material)
    return obj


def analytic_frame_xz(name, outer_w, outer_h, inner_w, inner_h,
                      depth, outer_r, inner_r, material):
    """Single-shell rounded locating ring in X/Z, extending into negative Y."""
    outer = rounded_loop(outer_w, outer_h, outer_r)
    inner = rounded_loop(inner_w, inner_h, inner_r)
    count = len(outer)
    y_front = -0.75
    y_back = y_front - depth
    verts = [(x, y_front, z) for x, z in outer]
    verts += [(x, y_front, z) for x, z in inner]
    verts += [(x, y_back, z) for x, z in outer]
    verts += [(x, y_back, z) for x, z in inner]
    faces = []
    for index in range(count):
        nxt = (index + 1) % count
        of, ofn = index, nxt
        inf, infn = count+index, count+nxt
        ob, obn = 2*count+index, 2*count+nxt
        inb, inbn = 3*count+index, 3*count+nxt
        faces.extend([
            (of, ofn, infn, inf),
            (obn, ob, inb, inbn),
            (ofn, of, ob, obn),
            (inf, infn, inbn, inb),
        ])
    mesh = bpy.data.meshes.new(name + "Mesh")
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    obj.data.materials.append(material)
    return obj


def rounded_loop(width, height, radius, segments=14):
    """Clockwise rounded-rectangle loop with stable point count."""
    points = []
    corners = [
        (width/2-radius, height/2-radius, 0),
        (-width/2+radius, height/2-radius, 90),
        (-width/2+radius, -height/2+radius, 180),
        (width/2-radius, -height/2+radius, 270),
    ]
    for corner_index, (cx, cy, start) in enumerate(corners):
        count = segments - 1 if corner_index == len(corners) - 1 else segments
        for step in range(count):
            angle = math.radians(start + 90*step/(segments-1))
            points.append((cx + radius*math.cos(angle), cy + radius*math.sin(angle)))
    return points


def rounded_rect_prism(name, width, height, depth, radius, material=None):
    """Constant-section rounded prism centred on local Z, with no edge recess."""
    loop = rounded_loop(width, height, radius)
    count = len(loop)
    verts = [(x, y, -depth/2) for x, y in loop]
    verts += [(x, y, depth/2) for x, y in loop]
    faces = [tuple(reversed(range(count))), tuple(range(count, 2*count))]
    for index in range(count):
        nxt = (index + 1) % count
        faces.append((index, nxt, count+nxt, count+index))
    mesh = bpy.data.meshes.new(name + "Mesh")
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    if material:
        obj.data.materials.append(material)
    return obj


def rounded_panel_xz(name, width, height, depth, radius, material=None):
    """Watertight rounded X/Z panel extruded through Y, without bevel artifacts."""
    loop = rounded_loop(width, height, radius)
    count = len(loop)
    verts = [(x, -depth/2, z) for x, z in loop]
    verts += [(x, depth/2, z) for x, z in loop]
    faces = [tuple(reversed(range(count))), tuple(range(count, 2*count))]
    for index in range(count):
        nxt = (index + 1) % count
        faces.append((index, nxt, count+nxt, count+index))
    mesh = bpy.data.meshes.new(name + "Mesh")
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    if material:
        obj.data.materials.append(material)
    return obj


def rounded_rect_frustum(name, outer_w, outer_h, inner_w, inner_h,
                         depth, outer_radius, inner_radius, material=None):
    """Closed rounded frustum: wide at local +Z, narrow at local -Z."""
    outer = rounded_loop(outer_w, outer_h, outer_radius)
    inner = rounded_loop(inner_w, inner_h, inner_radius)
    count = len(outer)
    verts = [(x, y, depth/2) for x, y in outer]
    verts += [(x, y, -depth/2) for x, y in inner]
    faces = [tuple(range(count)), tuple(reversed(range(count, 2*count)))]
    for index in range(count):
        nxt = (index + 1) % count
        faces.append((index, nxt, count+nxt, count+index))
    mesh = bpy.data.meshes.new(name + "Mesh")
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    if material:
        obj.data.materials.append(material)
    return obj


def rounded_ramp_tunnel(name, outer_w, outer_h, inner_w, inner_h,
                        ramp_depth, total_depth, outer_radius, inner_radius):
    """Single watertight cutter: exterior ramp followed by a straight opening."""
    outer = rounded_loop(outer_w, outer_h, outer_radius)
    inner = rounded_loop(inner_w, inner_h, inner_radius)
    count = len(outer)
    z_outer = 0.15
    z_inner = -ramp_depth
    z_deep = -total_depth
    verts = [(x, y, z_outer) for x, y in outer]
    verts += [(x, y, z_inner) for x, y in inner]
    verts += [(x, y, z_deep) for x, y in inner]
    faces = [tuple(range(count)), tuple(reversed(range(2*count, 3*count)))]
    for index in range(count):
        nxt = (index + 1) % count
        faces.extend([
            (index, nxt, count+nxt, count+index),
            (count+index, count+nxt, 2*count+nxt, 2*count+index),
        ])
    mesh = bpy.data.meshes.new(name + "Mesh")
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    return obj


def rounded_loft_solid(name, sections):
    """Watertight rounded loft.

    Each section is (width, height, radius, local_z[, x_offset]). The optional
    offset lets the visible mask follow the physical LCD while the large
    drop-in tunnel remains centred on the PCB.
    """
    loops = [rounded_loop(section[0], section[1], section[2])
             for section in sections]
    count = len(loops[0])
    verts = []
    for loop, section in zip(loops, sections):
        z = section[3]
        x_offset = section[4] if len(section) > 4 else 0.0
        verts.extend([(x + x_offset, y, z) for x, y in loop])
    faces = [tuple(range(count)),
             tuple(reversed(range((len(sections)-1)*count, len(sections)*count)))]
    for level in range(len(sections)-1):
        first = level*count
        second = (level+1)*count
        for index in range(count):
            nxt = (index + 1) % count
            faces.append((first+index, first+nxt, second+nxt, second+index))
    mesh = bpy.data.meshes.new(name + "Mesh")
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    return obj


def stepped_cylinder(name, sections, verts_count=64):
    """Watertight coaxial cutter from [(local_z, radius), ...]."""
    rings = []
    for z, radius in sections:
        rings.append([
            (radius*math.cos(2*math.pi*i/verts_count),
             radius*math.sin(2*math.pi*i/verts_count), z)
            for i in range(verts_count)
        ])
    verts = [vertex for ring in rings for vertex in ring]
    faces = [tuple(reversed(range(verts_count))),
             tuple(range((len(rings)-1)*verts_count, len(rings)*verts_count))]
    for level in range(len(rings)-1):
        first = level*verts_count
        second = (level+1)*verts_count
        for index in range(verts_count):
            nxt = (index + 1) % verts_count
            faces.append((first+index, first+nxt, second+nxt, second+index))
    mesh = bpy.data.meshes.new(name + "Mesh")
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    return obj


def rounded_ramp_frame(name, outer_w, outer_h, inner_w, inner_h,
                       outer_height, inner_height, outer_radius, inner_radius,
                       material):
    """Flat-bottom faceplate whose visible surface slopes gently to the glass."""
    outer = rounded_loop(outer_w, outer_h, outer_radius)
    inner = rounded_loop(inner_w, inner_h, inner_radius)
    count = len(outer)
    verts = []
    verts.extend([(x, y, 0.0) for x, y in outer])
    verts.extend([(x, y, 0.0) for x, y in inner])
    verts.extend([(x, y, outer_height) for x, y in outer])
    verts.extend([(x, y, inner_height) for x, y in inner])
    faces = []
    for index in range(count):
        nxt = (index + 1) % count
        ob, obn = index, nxt
        ib, ibn = count+index, count+nxt
        ot, otn = 2*count+index, 2*count+nxt
        it, itn = 3*count+index, 3*count+nxt
        faces.extend([
            (ot, otn, itn, it),        # continuous visible ramp
            (obn, ob, ib, ibn),        # flat glue face
            (ob, obn, otn, ot),        # outer wall
            (ibn, ib, it, itn),        # thin edge next to glass
        ])
    mesh = bpy.data.meshes.new(name + "Mesh")
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    obj.data.materials.append(material)
    return obj


def boolean(target, cutter, operation='DIFFERENCE', solver='EXACT'):
    bpy.context.view_layer.objects.active = target
    mod = target.modifiers.new("boolean", 'BOOLEAN')
    mod.operation = operation
    mod.solver = solver
    mod.object = cutter
    bpy.ops.object.modifier_apply(modifier=mod.name)
    bpy.data.objects.remove(cutter, do_unlink=True)


def mesh_health(obj, label):
    """Build-time diagnostic used to catch the operation that opens a solid."""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    boundary = sum(1 for edge in bm.edges if edge.is_boundary)
    non_manifold = sum(1 for edge in bm.edges if not edge.is_manifold)
    print(f"MESH_HEALTH {label}: boundary={boundary} non_manifold={non_manifold}")
    if boundary:
        centres = [(edge.verts[0].co + edge.verts[1].co) * 0.5
                   for edge in bm.edges if edge.is_boundary]
        print("  BOUNDARY_BOUNDS "
              f"x={min(v.x for v in centres):.2f}..{max(v.x for v in centres):.2f} "
              f"y={min(v.y for v in centres):.2f}..{max(v.y for v in centres):.2f} "
              f"z={min(v.z for v in centres):.2f}..{max(v.z for v in centres):.2f}")
    bm.free()


def join(name, objects, material=None):
    bpy.ops.object.select_all(action='DESELECT')
    for o in objects:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]
    bpy.ops.object.join()
    objects[0].name = name
    if material and not objects[0].data.materials:
        objects[0].data.materials.append(material)
    return objects[0]


def wedge_mesh(name, width, depth, height, inset=0):
    # Cross section: truly flat base, short front lip, inclined face, closed top and rear.
    x0, x1 = -width/2, width/2
    y0, y1 = -depth/2, depth/2
    z0 = inset
    pts = [
        (y0, z0),
        (y0, z0 + 8),
        (y0 + 48, height - inset),
        (y1, height - inset),
        (y1, z0),
    ]
    verts = []
    for x in (x0, x1):
        verts.extend([(x, y, z) for y, z in pts])
    n = len(pts)
    faces = [tuple(range(n)), tuple(range(2*n-1, n-1, -1))]
    for i in range(n):
        j = (i+1) % n
        faces.append((i, n+i, n+j, j))
    mesh = bpy.data.meshes.new(name + "Mesh")
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    return obj


def oriented_box(name, size, center, angle=FRONT_ANGLE):
    o = box(name, size, center)
    # Local Z follows the inclined face; local Y is its normal.
    o.rotation_euler[0] = angle - math.pi/2
    return o


def face_point(x, local_z, inset=0.0):
    """Map screen-plane X/Z and inward depth to the inclined front face."""
    center_y, center_z = -24.0, 40.1
    return (
        x,
        center_y + math.cos(FRONT_ANGLE)*local_z + math.sin(FRONT_ANGLE)*inset,
        center_z + math.sin(FRONT_ANGLE)*local_z - math.cos(FRONT_ANGLE)*inset,
    )


def make_body():
    outer = wedge_mesh("Main shell", CASE_W, CASE_D, CASE_H)
    outer.data.materials.append(RED)
    bevel = outer.modifiers.new("rounded exterior", 'BEVEL')
    bevel.width = 5.0
    bevel.segments = 6
    bpy.context.view_layer.objects.active = outer
    bpy.ops.object.modifier_apply(modifier=bevel.name)

    # Cut the display aperture while the body is still a simple solid. Rev. G
    # uses a small active-area mask at the exterior and a generously oversized
    # drop-in tunnel behind it.  The glass simply comes forward from the open
    # rear and rests behind this continuous 0.72 mm transition: no groove,
    # ledge or external indentation has to match the module outline.
    center_y, center_z = -24.0, 40.1
    # The glass pocket must overlap the hollow cavity decisively.  A 5.2 mm
    # cutter ended just short after the cavity keep-outs were introduced and
    # left a thin printable membrane across the display opening.  Fourteen
    # millimetres passes fully into the enclosure while remaining far from all
    # electronics and mounting points.
    glass_depth = 14.0
    screen_cut = rounded_loft_solid("integral flush screen ramp", [
        # The lit LCD area is almost square-cornered.  A 1.2 mm radius conceals
        # its black mask without clipping the visible UI corners.
        (SCREEN_FRONT_OPEN_W, SCREEN_FRONT_OPEN_H, 1.2, 0.18, SCREEN_FRONT_OPEN_X),
        (SCREEN_FRONT_OPEN_W, SCREEN_FRONT_OPEN_H, 1.2, -0.04, SCREEN_FRONT_OPEN_X),
        # The opening grows inward, so front-down layers recede instead of
        # depositing unsupported plastic across the centre of the aperture.
        (SCREEN_APERTURE_W, SCREEN_APERTURE_H, 0.8, -SCREEN_RAMP_DEPTH),
        (SCREEN_APERTURE_W, SCREEN_APERTURE_H, 0.8,
         -(SCREEN_RAMP_DEPTH+glass_depth)),
    ])
    screen_cut.rotation_euler[0] = FRONT_ANGLE
    screen_cut.location = (0, center_y, center_z)
    mesh_health(screen_cut, "integral ramp screen cutter")

    # Hollow cavity, deliberately open through the rear for assembly.
    cavity = wedge_mesh("cavity", CASE_W-2*WALL, CASE_D-WALL, CASE_H, inset=WALL)
    cavity.location.y += WALL + 1.5

    # Form the four screen pads as keep-outs in the cavity.  Leaving their
    # volume in the original shell produces one continuous watertight body;
    # adding the same pads afterwards triggers an Exact-boolean seam on the
    # positive-X tessellation of Blender's bevel.
    for sx in (-1, 1):
        for sz in (-1, 1):
            x = sx * HOLE_X/2
            local_z = sz * HOLE_Z/2
            # The compact support is concentric with the real PCB hole.  Its
            # rear face stops at the PCB front plane, unlike earlier supports
            # that crossed the board volume and prevented the screen seating.
            spacer_center = face_point(x, local_z, SCREEN_PAD_INSET)
            keepout = box("screen pad cavity keepout",
                          (SCREEN_SPACER_D, SCREEN_SPACER_DEPTH, SCREEN_SPACER_D),
                          spacer_center, bevel=0.55)
            keepout.rotation_euler[0] = FRONT_ANGLE - math.pi/2
            boolean(cavity, keepout, solver='MANIFOLD')

    # Recess the inner left wall around the on-board USB-C connector.  Together
    # with the wider shell this provides 25.5 mm from the PCB edge to the side
    # skin, enough to insert a normal plug and ample room for a compact right-
    # angle extension to turn towards the rear USB-C panel opening.  The cutter
    # starts 2.4 mm behind the inclined exterior, so no slot is visible outside.
    usb_service_right_x = -42.0
    usb_service_w = usb_service_right_x - USB_SERVICE_INNER_X
    usb_service = oriented_box(
        "internal USB-C plug and cable chamber",
        (usb_service_w, 27.6, 21.0),
        face_point((USB_SERVICE_INNER_X + usb_service_right_x)/2,
                   USB_SERVICE_LOCAL_Z, 16.2))
    boolean(cavity, usb_service, operation='UNION')
    # Hollow the enclosure first, then drive the display tunnel well into that
    # already empty volume.  Keeping these booleans sequential avoids the
    # multi-edge seam that a wide Rev. F aperture created when both negative
    # volumes were unioned before subtraction.
    mesh_health(cavity, "cavity with USB service chamber")
    boolean(outer, cavity)
    mesh_health(outer, "body after cavity")
    boolean(outer, screen_cut)
    mesh_health(outer, "body after independent screen tunnel")

    # Rear-cover screw bosses corresponding to the four panel holes.
    rear_bosses = []
    rear_bridges = []
    for x in (-REAR_BOSS_X, REAR_BOSS_X):
        for z in (11.0, 67.0):
            # Rear face ends at Y=41.3: 0.4 mm before the cover pads.  This leaves
            # the locating tongue completely unobstructed instead of meeting it at Y=46.
            p = cyl("recessed rear cover boss", REAR_BOSS_D/2, 7.0,
                    (x, 37.8, z), (math.pi/2, 0, 0), RED)
            rear_bosses.append(p)
            # Rib reaches the side wall but stops well ahead of the seal plane.
            bridge = box("inset rear boss rib", (9.4, 5.0, 5.0),
                         ((CASE_W/2-6.5 if x > 0 else -CASE_W/2+6.5), 37.0, z), RED, 0.8)
            rear_bridges.append(bridge)
    # Exact unions avoid leaving intersecting shells for the slicer to repair.
    for feature in rear_bosses + rear_bridges:
        boolean(outer, feature, 'UNION')
    mesh_health(outer, "body after solid mount unions")

    # Drill only after every spacer is part of the body.  Use two overlapping
    # manifold cylinders per M3 path; Blender's manifold solver rejects the
    # otherwise valid custom stepped cutter, while these primitives make the
    # same hidden head pocket and through clearance without ambiguous edges.
    for sx in (-1, 1):
        for sz in (-1, 1):
            x = sx*HOLE_X/2
            local_z = sz*HOLE_Z/2
            clearance = cyl("front M3 clearance", SCREEN_CLEARANCE_D/2, 10.0,
                            face_point(x, local_z, 4.0),
                            (FRONT_ANGLE, 0, 0), verts=48)
            boolean(outer, clearance)
            head_pocket = cyl("front M3 head pocket", SCREEN_HEAD_D/2, 1.9,
                              face_point(x, local_z, 0.65),
                              (FRONT_ANGLE, 0, 0), verts=48)
            boolean(outer, head_pocket)

    mesh_health(outer, "body after front screen screw bores")

    for x in (-REAR_BOSS_X, REAR_BOSS_X):
        for z in (11.0, 67.0):
            boolean(outer, cyl("rear cover pilot", 1.35, 10.0,
                               (x, 37.8, z), (math.pi/2,0,0)))
    mesh_health(outer, "body after final screw bores")
    outer.name = "01_main_body"
    return outer


def make_rear():
    # Rear panel overlaps the opening and carries an internal locating tongue.
    panel = rounded_panel_xz(
        "Rear panel", CASE_W-4.0, CASE_H-4.0, 3.0, 4.5, BLACK)
    mesh_health(panel, "rear base")
    # Hollow locating tongue. The Rev. F ring still stopped the cover roughly
    # 2 mm before the sealing face on the physical print. Rev. G is deliberately
    # shallow and loose: 3.2 mm per side, 1.8 mm vertically and only 0.8 mm
    # projecting beyond the panel's inner face. The screws do the final
    # clamping; this ring now only centres the cover.
    tongue_outer_w = (CASE_W - 2*WALL) - 2*REAR_TONGUE_SIDE_GAP
    rear_cavity_h = (CASE_H - 2*WALL)
    tongue_outer_h = rear_cavity_h - 2*REAR_TONGUE_Z_GAP
    tongue_wall = REAR_TONGUE_WALL
    tongue = analytic_frame_xz(
        "continuous rear locating tongue",
        tongue_outer_w, tongue_outer_h,
        tongue_outer_w-2*tongue_wall, tongue_outer_h-2*tongue_wall,
        REAR_TONGUE_DEPTH, 3.0, 1.4, BLACK)
    boolean(panel, tongue, 'UNION')
    mesh_health(panel, "rear after tongue")

    # Speaker grille, centred left; 45 mm speaker retained by an internal ring.
    speaker_x, speaker_z = -27.0, 8.0
    for ix in range(-3, 4):
        for iz in range(-3, 4):
            x, z = speaker_x + ix*5.0, speaker_z + iz*5.0
            if (x-speaker_x)**2 + (z-speaker_z)**2 <= 18.5**2:
                hole = cyl("speaker grille", 1.45, 8.0, (x, 0, z), (math.pi/2, 0, 0), verts=32)
                boolean(panel, hole)
    mesh_health(panel, "rear after speaker grille")
    ring_outer = cyl("speaker retainer", (SPEAKER_D+2.0)/2, 4.0, (speaker_x, -2.8, speaker_z), (math.pi/2,0,0), BLACK)
    ring_inner = cyl("speaker pocket", (SPEAKER_D+0.6)/2, 7.0, (speaker_x, -2.8, speaker_z), (math.pi/2,0,0))
    boolean(ring_outer, ring_inner)

    # Rear USB-C panel-mount opening, moved to the lower edge as requested.
    usb = box("USB-C rear opening", (16.2, 8.0, 9.2), (0.0, 0, -29.0), bevel=1.6)
    boolean(panel, usb)
    mesh_health(panel, "rear after USB opening")

    # Inner pocket frames. They are open at the top and include cable exits.
    drv_parts = [
        box("DRV base", (DRV_W+5.0, 1.8, DRV_H+5.0), (32.0, -1.8, 2.0), BLACK),
        box("DRV left", (1.6, 5.0, DRV_H+5.0), (32.0-(DRV_W+5.0)/2, -5.0, 2.0), BLACK),
        box("DRV right", (1.6, 5.0, DRV_H+5.0), (32.0+(DRV_W+5.0)/2, -5.0, 2.0), BLACK),
        box("DRV bottom", (DRV_W+5.0, 5.0, 1.6), (32.0, -5.0, 2.0-(DRV_H+5.0)/2), BLACK),
    ]
    motor_outer = cyl("motor retainer", (MOTOR_D+2.0)/2, 4.5, (6.0, -3.2, -19.0), (math.pi/2,0,0), BLACK)
    motor_inner = cyl("motor pocket", (MOTOR_D+0.5)/2, 7.0, (6.0, -3.2, -19.0), (math.pi/2,0,0))
    boolean(motor_outer, motor_inner)

    # Four M3 clearance holes and matching compact raised pads.
    pads = []
    for x in (-REAR_BOSS_X, REAR_BOSS_X):
        for z in (-28.0, 28.0):
            pad = cyl("rear screw pad", REAR_BOSS_D/2, 3.0,
                      (x, -2.8, z), (math.pi/2,0,0), BLACK)
            pads.append(pad)

    for feature in [ring_outer, motor_outer] + drv_parts:
        boolean(panel, feature, 'UNION')
    mesh_health(panel, "rear after component holders")
    for pad in pads:
        boolean(panel, pad, 'UNION')
    mesh_health(panel, "rear after solid screw pads")

    for x in (-REAR_BOSS_X, REAR_BOSS_X):
        for z in (-28.0, 28.0):
            clearance = cyl("rear M3 clearance", 1.65, 10.0,
                            (x, -1.5, z), (math.pi/2, 0, 0), verts=48)
            boolean(panel, clearance)
            head_pocket = cyl("rear M3 head pocket", 3.15, 1.8,
                              (x, 0.9, z), (math.pi/2, 0, 0), verts=48)
            boolean(panel, head_pocket)
    mesh_health(panel, "rear after final screw bores")
    panel.name = "02_rear_panel"
    return panel


def make_decorative_trims():
    # A substantial premium accent hides the four external M3 heads. Its flat
    # underside remains outside the glass; the red body beneath still locates
    # the display, so increasing faceplate thickness cannot change screen fit.
    screen_trim = rounded_ramp_frame(
        "03_screen_faceplate", 106.0, 69.0,
        SCREEN_TRIM_OPEN_W, SCREEN_TRIM_OPEN_H,
        SCREEN_TRIM_OUTER_HEIGHT, SCREEN_TRIM_INNER_HEIGHT, 7.5, 3.0, GOLD)
    # The second prototype uses protruding screw heads with washers rather than
    # low-profile heads. Rev. G therefore provides a 13 mm diameter, 3.1 mm
    # deep hidden chamber at every screw while retaining at least 0.7 mm of
    # solid material above it.
    for sx in (-1, 1):
        for sz in (-1, 1):
            recess = cyl("screen screw head cover pocket",
                         SCREEN_TRIM_HEAD_POCKET_D/2,
                         SCREEN_TRIM_HEAD_POCKET_DEPTH,
                         (sx*HOLE_X/2, sz*HOLE_Z/2,
                          SCREEN_TRIM_HEAD_POCKET_DEPTH/2), verts=64)
            boolean(screen_trim, recess)
    screen_trim.name = "03_screen_faceplate"
    rear_trim = analytic_rounded_frame(
        "04_rear_seam_trim", CASE_W-5.8, 72.2, CASE_W-9.6, 68.4,
        0.8, 4.2, 2.4, GOLD)
    speaker_trim = cyl("05_speaker_trim", 24.6, 0.8, (0, 0, 0.4), material=GOLD)
    boolean(speaker_trim, cyl("speaker trim opening", 23.0, 2.8, (0, 0, 0.4)))
    return [screen_trim, rear_trim, speaker_trim]


def add_preview_components():
    pcb = oriented_box("PREVIEW Freenove PCB", (PCB_W, 1.6, PCB_H),
                       face_point(0, 0, PCB_PREVIEW_INSET))
    pcb.data.materials.append(GREEN)
    screen = oriented_box("PREVIEW flush touch glass", (TOUCH_OD_W, 1.2, TOUCH_OD_H),
                          face_point(0, 0, SCREEN_GLASS_FACE_INSET + 0.6))
    screen.data.materials.append(BLUE)
    speaker = cyl("PREVIEW speaker", SPEAKER_D/2, SPEAKER_T, (-30, 83.0, 48.0), (math.pi/2,0,0))
    speaker.data.materials.append(BLACK)
    return [pcb, screen, speaker]


def export_stl(obj, filename):
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.wm.stl_export(filepath=os.path.join(OUT, filename), export_selected_objects=True)


def repair_boundaries(obj):
    """Close tiny seams and give the slicer one coherent outward winding."""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.001)
    bmesh.ops.dissolve_degenerate(bm, dist=0.001, edges=list(bm.edges))
    boundary = [e for e in bm.edges if e.is_boundary]
    if boundary:
        bmesh.ops.holes_fill(bm, edges=boundary, sides=0)
    # A visually open, manifold tunnel can still contain locally reversed
    # triangles after stacked booleans. Bambu then treats the aperture as a
    # repairable defect and silently fills it in the generated G-code. Rebuild
    # the winding on the connected shell before triangulating and exporting.
    bmesh.ops.recalc_face_normals(bm, faces=list(bm.faces))
    bmesh.ops.triangulate(bm, faces=list(bm.faces))
    bm.normal_update()
    bm.to_mesh(obj.data)
    bm.free()
    obj.data.update()


def setup_render(body, rear, previews, trims):
    # Position panel alongside body for an exploded inspection render.
    rear.location = (82, 15, 40)
    rear.rotation_euler = (math.pi/2, 0, 0)
    for t in trims:
        t.hide_render = True
    # Show a gold copy of the screen trim in its final glued position.
    trim_preview = trims[0].copy()
    trim_preview.data = trims[0].data.copy()
    bpy.context.collection.objects.link(trim_preview)
    trim_preview.name = "PREVIEW screen accent"
    trim_preview.hide_render = False
    trim_preview.rotation_euler[0] = FRONT_ANGLE
    trim_preview.location = face_point(0, 0, -0.02)
    for p in previews:
        p.hide_render = False
    bpy.ops.object.light_add(type='AREA', location=(0,-130,150))
    bpy.context.object.data.energy = 1200
    bpy.context.object.data.shape = 'DISK'
    bpy.context.object.data.size = 120
    bpy.ops.object.light_add(type='AREA', location=(150,-20,70))
    bpy.context.object.data.energy = 800
    bpy.context.object.data.size = 90
    bpy.ops.object.camera_add(location=(190,-230,155))
    cam = bpy.context.object
    bpy.context.scene.camera = cam
    direction = Vector((15, 8, 36)) - cam.location
    cam.rotation_euler = direction.to_track_quat('-Z','Y').to_euler()
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_WORKBENCH'
    scene.display.shading.light = 'STUDIO'
    scene.display.shading.studio_light = 'paint.sl'
    scene.display.shading.color_type = 'MATERIAL'
    scene.display.shading.show_shadows = True
    scene.display.shading.show_cavity = True
    scene.display.shading.cavity_type = 'BOTH'
    scene.display.shading.background_type = 'VIEWPORT'
    scene.display.shading.background_color = (0.82, 0.82, 0.82)
    scene.render.resolution_x = 1200
    scene.render.resolution_y = 900
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'PNG'
    scene.render.filepath = os.path.join(OUT, "preview.png")
    scene.view_settings.look = 'AgX - Medium High Contrast'
    bpy.ops.render.render(write_still=True)

    # QA close-up without the decorative faceplate.  This shows the continuous
    # red transition ending less than one millimetre above the touch glass.
    rear.hide_render = True
    trim_preview.hide_render = True
    body.hide_render = False
    for p in previews:
        p.hide_render = True
    previews[1].hide_render = False
    cam.location = (0, -190, 108)
    direction = Vector((0, -18, 39)) - cam.location
    cam.rotation_euler = direction.to_track_quat('-Z','Y').to_euler()
    scene.render.filepath = os.path.join(OUT, "front_integral_ramp.png")
    bpy.ops.render.render(write_still=True)

    # QA view through the open rear: compact screen pads and recessed cover
    # bosses must remain clear of both the PCB outline and the locating tongue.
    for p in previews:
        p.hide_render = True
    cam.location = (140, 185, 115)
    direction = Vector((0, 10, 39)) - cam.location
    cam.rotation_euler = direction.to_track_quat('-Z','Y').to_euler()
    scene.render.filepath = os.path.join(OUT, "body_inside.png")
    bpy.ops.render.render(write_still=True)

    # Second QA render: inner face of the rear cover, showing tongue and fixings.
    body.hide_render = True
    rear.hide_render = False
    for p in previews:
        p.hide_render = True
    trim_preview.hide_render = True
    rear.location = (0, 0, 40)
    rear.rotation_euler = (0, 0, 0)
    cam.location = (105, -175, 112)
    direction = Vector((0, 0, 40)) - cam.location
    cam.rotation_euler = direction.to_track_quat('-Z','Y').to_euler()
    scene.render.filepath = os.path.join(OUT, "rear_inside.png")
    bpy.ops.render.render(write_still=True)

    # Third QA render: complete accent kit laid out as it will be printed.
    rear.hide_render = True
    for t in trims:
        t.hide_render = False
    trims[0].location = (-66, 20, 0)
    trims[1].location = (48, 18, 0)
    trims[2].location = (-66, -45, 0)
    cam.data.type = 'ORTHO'
    cam.data.ortho_scale = 220
    cam.location = (0, 0, 245)
    direction = Vector((0, 0, 0)) - cam.location
    cam.rotation_euler = direction.to_track_quat('-Z','Y').to_euler()
    scene.render.filepath = os.path.join(OUT, "decorative_trims.png")
    bpy.ops.render.render(write_still=True)


clean()
body = make_body()
rear = make_rear()
trims = make_decorative_trims()
repair_boundaries(body)
repair_boundaries(rear)
for trim in trims:
    repair_boundaries(trim)
mesh_health(body, "body after boundary repair")
mesh_health(rear, "rear after boundary repair")
for trim in trims:
    mesh_health(trim, trim.name + " after boundary repair")
export_stl(body, "01_main_body.stl")
export_stl(rear, "02_rear_panel.stl")
export_stl(trims[0], "03_screen_trim.stl")
export_stl(trims[1], "04_rear_seam_trim.stl")
export_stl(trims[2], "05_speaker_trim.stl")

# Dedicated print orientation requested by the user: exterior screen face on
# the bed, with no slicer-side "lay on face" calculation.  The Rev. F aperture
# grows from the viewing window into the glass pocket, so the integral ramp
# remains open while successive front-down layers recede away from the hole.
front_down = body.copy()
front_down.data = body.data.copy()
bpy.context.collection.objects.link(front_down)
front_down.name = "01_main_body_FRONT_DOWN"
front_down.rotation_euler[0] = math.pi - FRONT_ANGLE
bpy.ops.object.select_all(action='DESELECT')
front_down.select_set(True)
bpy.context.view_layer.objects.active = front_down
bpy.ops.object.transform_apply(location=False, rotation=True, scale=False)
min_z = min(vertex.co.z for vertex in front_down.data.vertices)
front_down.location.z = -min_z
bpy.ops.object.transform_apply(location=True, rotation=False, scale=False)
repair_boundaries(front_down)
mesh_health(front_down, "front-down body after orientation")
export_stl(front_down, "01_main_body_FRONT_DOWN_NO_SUPPORT.stl")
bpy.data.objects.remove(front_down, do_unlink=True)

# Save clean printable source before moving pieces for the render.
bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, "Benfica_CYD_Desk_Buddy.blend"))
previews = add_preview_components()
setup_render(body, rear, previews, trims)
