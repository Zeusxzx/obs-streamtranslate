import re, sys
src, dst = sys.argv[1], sys.argv[2]
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
    f.write("LIBRARY obs.dll\nEXPORTS\n")
    for n in names:
        f.write(f"    {n}\n")
print(f"{len(names)} exports")
