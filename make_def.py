import re, sys
src, dst = sys.argv[1], sys.argv[2]
dll = sys.argv[3] if len(sys.argv) > 3 else "obs.dll"
names = []
started = False
for line in open(src, errors="ignore"):
    if "ordinal hint" in line:
        started = True
        continue
    if started:
        m = re.match(r"\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)", line)
        if m:
            names.append(m.group(1))
with open(dst, "w") as f:
    f.write(f"LIBRARY {dll}\nEXPORTS\n")
    for n in names:
        f.write(f"    {n}\n")
print(f"{len(names)} exports from {dll}")
