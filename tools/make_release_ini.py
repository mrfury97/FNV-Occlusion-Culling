# Makes release\OcclusionCulling.ini from the developer OcclusionCulling.ini: the release defaults (the values measured
# in play), no [Debug] section (release builds have no measuring log, debug boxes or compare key) and a header that
# says what the release does. Run: python tools\make_release_ini.py
import os
import re

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = os.path.join(root, "OcclusionCulling.ini")
dst = os.path.join(root, "release", "OcclusionCulling.ini")

VALUES = {
    "fMinRadius": "40",
    "iMaxQueriesPerFrame": "1000",
    "bDistantLOD": "1",
    "bLampCulling": "1",
    "fRevealDepth": "150",
}
HEADER = [
    "; Occlusion Culling - Data\\NVSE\\Plugins\\OcclusionCulling.ini (read when the game starts)",
    "; Objects, NPCs and creatures, doors and activators, SpeedTree trees, land, distant terrain LOD, grass and lamps",
    "; that the graphics card proves are fully covered by what is already drawn are not drawn in the main view (lamps:",
    "; switched off there). NPCs and particle systems that are not drawn update less often. Shadows, reflections, the",
    "; player and anything near the camera are never touched. Start-up lines and any error go to",
    "; logs\\OcclusionCulling.log in the game folder.",
]

raw = open(src, "rb").read()
nl = "\r\n" if b"\r\n" in raw else "\n"
lines = raw.decode("utf-8").split(nl)
first_section = next(i for i, l in enumerate(lines) if l.startswith("["))
out = HEADER + [""]
section = None
done = set()
for line in lines[first_section:]:
    if line.startswith("["):
        section = line.strip()
    if section == "[Debug]":
        continue
    m = re.match(r"^(\w+)=(.*)$", line)
    if m and m.group(1) in VALUES:
        line = f"{m.group(1)}={VALUES[m.group(1)]}"
        done.add(m.group(1))
    line = line.replace("1 = on, 0 = off (measuring only)", "1 = on, 0 = off")
    line = line.replace("0 = only count such lamps in the log; 1 = also switch them off", "0 = leave lamps alone; 1 = switch them off")
    out.append(line)
assert done == set(VALUES), set(VALUES) - done
while out and out[-1].strip() == "":
    out.pop()
os.makedirs(os.path.dirname(dst), exist_ok=True)
open(dst, "wb").write((nl.join(out) + nl).encode("utf-8"))
print("wrote", dst)
