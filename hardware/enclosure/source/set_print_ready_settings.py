"""Force the final Bambu project to print without support, raft or brim."""
import json
import os
import sys
import tempfile
import zipfile


if len(sys.argv) != 2:
    raise SystemExit("usage: set_print_ready_settings.py PROJECT.3mf")

project = os.path.abspath(sys.argv[1])
config_name = "Metadata/project_settings.config"

with zipfile.ZipFile(project, "r") as source:
    entries = [(item, source.read(item.filename)) for item in source.infolist()]

updated = False
fd, temporary = tempfile.mkstemp(
    prefix="print_ready_", suffix=".3mf", dir=os.path.dirname(project))
os.close(fd)
try:
    with zipfile.ZipFile(temporary, "w") as target:
        for item, data in entries:
            if item.filename == config_name:
                settings = json.loads(data.decode("utf-8"))
                settings["enable_support"] = "0"
                settings["raft_layers"] = "0"
                settings["brim_type"] = "no_brim"
                settings["brim_width"] = "0"
                data = json.dumps(settings, ensure_ascii=False, indent=4).encode("utf-8")
                updated = True
            target.writestr(item, data)
    if not updated:
        raise RuntimeError(f"{config_name} is missing from {project}")
    os.replace(temporary, project)
finally:
    if os.path.exists(temporary):
        os.unlink(temporary)

print(project)
print("enable_support=0 raft_layers=0 brim_type=no_brim brim_width=0")
