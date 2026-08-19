#!/usr/bin/env python3
"""Static checks on main/level.c.

Catches the mistakes that are invisible when eyeballing ASCII maps:
wrong row width, missing spawn/exit, and objectives walled off from the
player. Run before flashing.
"""
import re, sys, collections

W, H = 23, 28
src = open("main/level.c").read()

# Strip comments so map rows are the only quoted strings we see.
src_nc = re.sub(r"//[^\n]*", "", src)
blocks = re.findall(r"\{\s*\.name\s*=\s*\"(.*?)\".*?\.rows\s*=\s*\{(.*?)\},\s*\.guards\s*=\s*\{(.*?)\},\s*\.guard_count\s*=\s*(\d+)", src_nc, re.S)

if not blocks:
    sys.exit("FAIL: no levels parsed out of main/level.c")

def bfs(grid, start):
    seen = {start}
    q = collections.deque([start])
    while q:
        x, y = q.popleft()
        for dx, dy in ((1,0),(-1,0),(0,1),(0,-1)):
            n = (x+dx, y+dy)
            if 0 <= n[0] < W and 0 <= n[1] < H and n not in seen and grid[n[1]][n[0]] != '#':
                seen.add(n); q.append(n)
    return seen

errors, warnings = [], []

for name, rows_src, guards_src, gcount in blocks:
    rows = re.findall(r'"([^"]*)"', rows_src)
    tag = f"[{name}]"

    if len(rows) != H:
        errors.append(f"{tag} has {len(rows)} rows, expected {H}")
        continue
    for i, r in enumerate(rows):
        if len(r) != W:
            errors.append(f"{tag} row {i} is {len(r)} chars, expected {W}: {r!r}")
    if any(len(r) != W for r in rows):
        continue

    bad = set("".join(rows)) - set("#.@HE")
    if bad:
        errors.append(f"{tag} unexpected characters: {sorted(bad)}")

    cells = lambda ch: [(x, y) for y in range(H) for x in range(W) if rows[y][x] == ch]
    starts, exits, hostages = cells('@'), cells('E'), cells('H')

    if len(starts) != 1: errors.append(f"{tag} has {len(starts)} player starts, expected 1")
    if len(exits)  != 1: errors.append(f"{tag} has {len(exits)} exits, expected 1")
    if not hostages:     warnings.append(f"{tag} has no hostages")
    if not starts or not exits:
        continue

    # Border must be sealed.
    for x in range(W):
        if rows[0][x] != '#' or rows[H-1][x] != '#':
            errors.append(f"{tag} top/bottom border leaks at x={x}"); break
    for y in range(H):
        if rows[y][0] != '#' or rows[y][W-1] != '#':
            errors.append(f"{tag} left/right border leaks at y={y}"); break

    reach = bfs(rows, starts[0])
    if exits[0] not in reach:
        errors.append(f"{tag} exit {exits[0]} unreachable from start {starts[0]}")
    for h in hostages:
        if h not in reach:
            errors.append(f"{tag} hostage {h} unreachable from start {starts[0]}")

    # Guard waypoints: on floor, and on the same connected component.
    wps = re.findall(r"\.wx\s*=\s*\{([^}]*)\}\s*,\s*\.wy\s*=\s*\{([^}]*)\}\s*,\s*\.wp_count\s*=\s*(\d+)", guards_src)
    if len(wps) != int(gcount):
        errors.append(f"{tag} guard_count={gcount} but {len(wps)} guard defs")
    for gi, (xs, ys, n) in enumerate(wps):
        xs = [int(v) for v in xs.split(",") if v.strip()]
        ys = [int(v) for v in ys.split(",") if v.strip()]
        n = int(n)
        if len(xs) < n or len(ys) < n:
            errors.append(f"{tag} guard {gi} declares wp_count={n} but gives {len(xs)}/{len(ys)} coords")
            continue
        for k in range(n):
            p = (xs[k], ys[k])
            if not (0 <= p[0] < W and 0 <= p[1] < H):
                errors.append(f"{tag} guard {gi} waypoint {k} {p} out of bounds")
            elif rows[p[1]][p[0]] == '#':
                errors.append(f"{tag} guard {gi} waypoint {k} {p} is inside a wall")
            elif p not in reach:
                warnings.append(f"{tag} guard {gi} waypoint {k} {p} is cut off from the player area")

    free = sum(r.count('.') + r.count('@') + r.count('H') + r.count('E') for r in rows)
    unreached = free - len(reach)
    if unreached > 0:
        warnings.append(f"{tag} {unreached} floor tiles are sealed off from the player")

print(f"parsed {len(blocks)} levels")
for w in warnings: print("  warn: " + w)
for e in errors:   print("  FAIL: " + e)
print("OK" if not errors else f"{len(errors)} error(s)")
sys.exit(1 if errors else 0)
