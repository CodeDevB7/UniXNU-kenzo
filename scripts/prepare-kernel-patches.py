#!/usr/bin/env python3
"""Drop a Sheng-only progress hunk from the generic XNU patch for this board build."""

from pathlib import Path
import sys


root = Path(sys.argv[1])
patch = root / "patches/xnu/zzz-boot-progress.patch"
text = patch.read_text()
lines = text.splitlines(keepends=True)
start_marker = "@@ -773,7 +775,9 @@"
next_marker = "@@ -1010,7 +1014,9 @@"

starts = [i for i, line in enumerate(lines) if line.rstrip("\r\n") == start_marker]
if not starts:
    if "sheng_pmgr_run" in text:
        raise SystemExit(f"unsupported Sheng-only hunk layout in {patch}")
    print("Generic boot-progress patch already has no Sheng-only PMGR hunk.")
    raise SystemExit(0)
if len(starts) != 1:
    raise SystemExit(f"expected one Sheng-only PMGR hunk in {patch}")

start = starts[0]
ends = [i for i in range(start + 1, len(lines)) if lines[i].rstrip("\r\n") == next_marker]
if len(ends) != 1:
    raise SystemExit(f"cannot locate the following generic hunk in {patch}")
end = ends[0]
hunk = "".join(lines[start:end])
if "XNU_SHENG_PMGR" not in hunk or "sheng_pmgr_run" not in hunk:
    raise SystemExit(f"refusing to alter unexpected patch content in {patch}")

# Removing two added probe lines changes the new-file line offset for the following hunk.
lines[end] = lines[end].replace("+1014,9", "+1012,9", 1)
patch.write_text("".join(lines[:start] + lines[end:]))
print("Removed the optional Sheng PMGR progress hunk from the generic boot-progress patch.")
