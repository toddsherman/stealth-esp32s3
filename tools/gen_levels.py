#!/usr/bin/env python3
"""Generates main/level_gen.c - 100 procedural stages of rising difficulty.

Every stage is validated before it is emitted: the map must be sealed, fully
connected, and the exit, every hostage and every guard waypoint must be
reachable from the player's start. A stage that fails is discarded and
regenerated, so the output is always playable.

Difficulty ramps along four axes: wall density (tighter mazes, longer detours),
guard count, hostage count, and how far the exit sits from the spawn.

Deterministic: a fixed seed means regenerating produces the same 100 stages.
"""
import random, collections, sys

W, H = 23, 28
MAX_GUARDS, MAX_HOSTAGES = 6, 4
COUNT = 100
SEED = 20260819

ADJ = ["COLD","QUIET","LOW","LONG","DARK","THIN","BLIND","SILENT","SHORT","DEEP",
       "GLASS","IRON","PAPER","SALT","AMBER","SLATE","HOLLOW","NARROW","BITTER",
       "PALE","STILL","SHARP","BLACK","GREY","LAST","FIRST","OPEN","CLOSED"]
NOUN = ["WATCH","YARD","HALL","LINE","GATE","VAULT","ROOM","WING","STAIR","DOCK",
        "MARKET","CHAPEL","OFFICE","GARDEN","TUNNEL","LOBBY","ANNEX","DEPOT",
        "STUDY","KITCHEN","CELLAR","ATRIUM","LANDING","GALLERY","COURT","PANTRY"]


def blank():
    g = [["#"] * W for _ in range(H)]
    for y in range(1, H - 1):
        for x in range(1, W - 1):
            g[y][x] = "."
    return g


def floors(g):
    return [(x, y) for y in range(H) for x in range(W) if g[y][x] != "#"]


def reachable(g, start):
    seen = {start}
    q = collections.deque([start])
    while q:
        x, y = q.popleft()
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            n = (x + dx, y + dy)
            if (0 <= n[0] < W and 0 <= n[1] < H and n not in seen
                    and g[n[1]][n[0]] != "#"):
                seen.add(n)
                q.append(n)
    return seen


def dists(g, start):
    d = {start: 0}
    q = collections.deque([start])
    while q:
        x, y = q.popleft()
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            n = (x + dx, y + dy)
            if (0 <= n[0] < W and 0 <= n[1] < H and n not in d
                    and g[n[1]][n[0]] != "#"):
                d[n] = d[(x, y)] + 1
                q.append(n)
    return d


def path_between(g, a, b):
    """The route a guard will actually walk, by the same downhill-from-BFS
    rule the game uses."""
    d = dists(g, b)
    if a not in d:
        return None
    p, cur = [a], a
    guard = 0
    while cur != b:
        guard += 1
        if guard > W * H:
            return None
        best = None
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            n = (cur[0] + dx, cur[1] + dy)
            if n in d and d[n] < d[cur] and (best is None or d[n] < d[best]):
                best = n
        if best is None:
            return None
        cur = best
        p.append(cur)
    return p


# GUARD_RANGE is 110px = 6.9 tiles. Keeping every tile of every patrol at
# least this far from the spawn means no guard can already see the player
# when the stage begins - being caught before you have moved is not a
# difficulty curve, it is a broken stage.
SPAWN_SAFE_TILES = 8.0


def patrol_is_fair(g, start, a, b):
    p = path_between(g, a, b)
    if p is None:
        return False
    for (x, y) in p:
        if ((x - start[0]) ** 2 + (y - start[1]) ** 2) < SPAWN_SAFE_TILES ** 2:
            return False
    return True


def carve(rng, g, blocks):
    """Drop wall blocks, reverting any that would split the map in two."""
    placed = 0
    for _ in range(blocks * 8):
        if placed >= blocks:
            break
        bw = rng.choice([2, 2, 3, 3, 4, 5])
        bh = rng.choice([2, 2, 3, 3, 4, 5])
        x = rng.randrange(1, W - bw - 1)
        y = rng.randrange(1, H - bh - 1)

        saved = [(xx, yy) for yy in range(y, y + bh) for xx in range(x, x + bw)
                 if g[yy][xx] == "."]
        if len(saved) < bw * bh:      # overlaps an existing block; skip
            continue
        for xx, yy in saved:
            g[yy][xx] = "#"

        fl = floors(g)
        if not fl or len(reachable(g, fl[0])) != len(fl):
            for xx, yy in saved:      # it split the map - undo
                g[yy][xx] = "."
            continue
        placed += 1
    return placed


def build(rng, difficulty):
    """difficulty: 0.0 (easiest) .. 1.0 (hardest)"""
    g = blank()
    blocks = int(7 + difficulty * 17)
    carve(rng, g, blocks)

    fl = floors(g)
    if len(fl) < 180:
        return None

    start = rng.choice(fl)
    d = dists(g, start)
    if len(d) != len(fl):
        return None

    far = max(d.values())
    # Harder stages put the exit further out.
    want = far * (0.55 + 0.4 * difficulty)
    exit_t = min(d, key=lambda t: abs(d[t] - want))
    if d[exit_t] < 12:
        return None

    n_host = min(MAX_HOSTAGES, 1 + int(difficulty * 3.2))
    n_guard = min(MAX_GUARDS, 1 + int(difficulty * 5.4))

    # Hostages: a reasonable walk away, and spread out from each other.
    cands = [t for t in fl if d.get(t, 0) >= 6 and t not in (start, exit_t)]
    rng.shuffle(cands)
    hostages = []
    for t in cands:
        if len(hostages) >= n_host:
            break
        if all(abs(t[0] - h[0]) + abs(t[1] - h[1]) >= 7 for h in hostages):
            hostages.append(t)
    if len(hostages) < n_host:
        return None

    # Guards: two waypoints that are far apart, and not sitting on the spawn.
    guards = []
    tries = 0
    while len(guards) < n_guard and tries < 900:
        tries += 1
        a = rng.choice(fl)
        da = dists(g, a)
        opts = [t for t, dd in da.items() if 7 <= dd <= 20]
        if not opts:
            continue
        b = rng.choice(opts)
        if any(abs(a[0] - q[0][0]) + abs(a[1] - q[0][1]) < 5 for q in guards):
            continue
        # The whole patrol must stay clear of the spawn, not just its ends.
        if not patrol_is_fair(g, start, a, b):
            continue
        guards.append(((a[0], a[1]), (b[0], b[1])))
    if len(guards) < n_guard:
        return None

    g[start[1]][start[0]] = "@"
    g[exit_t[1]][exit_t[0]] = "E"
    for hx, hy in hostages:
        g[hy][hx] = "H"

    return {
        "rows": ["".join(r) for r in g],
        "guards": guards,
        "bombs": 2 + int(difficulty * 2.2),
        "start": start, "exit": exit_t, "hostages": hostages,
    }


def validate(lv):
    rows = lv["rows"]
    assert len(rows) == H and all(len(r) == W for r in rows), "geometry"
    assert all(rows[0][x] == "#" and rows[H-1][x] == "#" for x in range(W)), "top/bottom"
    assert all(rows[y][0] == "#" and rows[y][W-1] == "#" for y in range(H)), "sides"
    assert sum(r.count("@") for r in rows) == 1, "spawn"
    assert sum(r.count("E") for r in rows) == 1, "exit"

    grid = [list(r) for r in rows]
    reach = reachable(grid, lv["start"])
    assert lv["exit"] in reach, "exit unreachable"
    for h in lv["hostages"]:
        assert h in reach, "hostage unreachable"
    free = sum(r.count(c) for r in rows for c in ".@HE")
    assert len(reach) == free, "sealed pocket"
    for a, b in lv["guards"]:
        assert grid[a[1]][a[0]] != "#" and grid[b[1]][b[0]] != "#", "waypoint in wall"
        assert a in reach and b in reach, "waypoint cut off"
        assert b in reachable(grid, a), "waypoints not mutually reachable"
        assert patrol_is_fair(grid, lv["start"], a, b), "patrol passes the spawn"


def main():
    rng = random.Random(SEED)
    names, used = [], set()
    while len(names) < COUNT:
        n = f"{rng.choice(ADJ)} {rng.choice(NOUN)}"
        if n not in used and len(n) <= 15:
            used.add(n)
            names.append(n)

    levels, attempts = [], 0
    while len(levels) < COUNT:
        attempts += 1
        if attempts > COUNT * 400:
            sys.exit("generator failed to converge")
        diff = len(levels) / (COUNT - 1)
        lv = build(rng, diff)
        if lv is None:
            continue
        try:
            validate(lv)
        except AssertionError:
            continue
        lv["name"] = names[len(levels)]
        levels.append(lv)

    out = ['// GENERATED by tools/gen_levels.py - do not edit by hand.',
           '//',
           '// 100 procedural stages of rising difficulty. Every one is validated at',
           '// generation time: sealed border, fully connected floor, and a reachable',
           '// exit, hostages and guard waypoints. Regenerate with:',
           '//     python3 tools/gen_levels.py',
           '#include "game.h"', '',
           'const level_def_t g_levels_gen[] = {']
    for i, lv in enumerate(levels):
        out.append('{')
        out.append('    .name = "%s",' % lv["name"])
        out.append('    .hint = "",')
        out.append('    .rows = {')
        for r in lv["rows"]:
            out.append('        "%s",' % r)
        out.append('    },')
        out.append('    .guards = {')
        for a, b in lv["guards"]:
            out.append('        { .wx = {%d, %d}, .wy = {%d, %d}, .wp_count = 2, '
                       '.dwell = %.1ff, .cycle = false },' %
                       (a[0], b[0], a[1], b[1], 0.6 + (i % 5) * 0.2))
        out.append('    },')
        out.append('    .guard_count = %d,' % len(lv["guards"]))
        out.append('    .bombs = %d,' % lv["bombs"])
        out.append('},')
    out.append('};')
    out.append('')
    out.append('const int g_level_gen_count = '
               '(int)(sizeof(g_levels_gen) / sizeof(g_levels_gen[0]));')
    out.append('')
    open('main/level_gen.c', 'w').write('\n'.join(out))

    print("generated %d stages in %d attempts" % (len(levels), attempts))
    for k in (0, 24, 49, 74, 99):
        lv = levels[k]
        print("  #%3d %-16s guards=%d hostages=%d bombs=%d walls=%d" %
              (k + 7, lv["name"], len(lv["guards"]), len(lv["hostages"]),
               lv["bombs"], sum(r.count("#") for r in lv["rows"])))


if __name__ == "__main__":
    main()
