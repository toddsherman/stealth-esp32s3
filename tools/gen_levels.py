#!/usr/bin/env python3
"""Generates main/level_gen.c - 94 procedural stages of rising difficulty.

Difficulty is *measured*, not assumed. Each candidate is scored by how exposed
the route you are forced to walk actually is:

  - the required tour (spawn -> every hostage -> exit) is pathfound,
  - every guard's patrol is walked and the tiles it can see are accumulated,
  - the score is dominated by what fraction of that tour sits under guard
    vision, and by how many guards watch each of those tiles.

Secondary terms cover guard count, tour length, corridor tightness and how many
sound bombs you are given. Candidates are then sorted by score and sampled
evenly across the range, which makes the ramp monotonic by construction rather
than by hoping the parameters behave.

Fairness rules that override difficulty:
  - the map is sealed and fully connected,
  - the exit, every hostage and every waypoint is reachable,
  - no guard's patrol passes within vision range of the spawn.

Deterministic: a fixed seed reproduces the same stages.
"""
import random, collections, math, sys

W, H = 23, 28
MAX_GUARDS, MAX_HOSTAGES = 8, 4
WANT = 94              # 6 hand-built stages precede these, for 100 total
POOL = 150             # candidates generated, then sampled down to WANT
SEED = 20260820

VISION_TILES = 7       # GUARD_RANGE 110px / 16px per tile, rounded up
SPAWN_SAFE   = 8.0     # no patrol may come this close to the spawn

ADJ = ["COLD","QUIET","LOW","LONG","DARK","THIN","BLIND","SILENT","SHORT","DEEP",
       "GLASS","IRON","PAPER","SALT","AMBER","SLATE","HOLLOW","NARROW","BITTER",
       "PALE","STILL","SHARP","BLACK","GREY","LAST","FIRST","OPEN","CLOSED",
       "BROKEN","EMPTY","WIDE","HIGH","OLD","NEW","RED","BLUE","SLOW","FAST"]
NOUN = ["WATCH","YARD","HALL","LINE","GATE","VAULT","ROOM","WING","STAIR","DOCK",
        "MARKET","CHAPEL","OFFICE","GARDEN","TUNNEL","LOBBY","ANNEX","DEPOT",
        "STUDY","KITCHEN","CELLAR","ATRIUM","LANDING","GALLERY","COURT","PANTRY",
        "FOYER","ARCHIVE","BRIDGE","TOWER","CRYPT","MILL","FORGE","QUARRY"]

NB4 = ((1, 0), (-1, 0), (0, 1), (0, -1))


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
        for dx, dy in NB4:
            n = (x + dx, y + dy)
            if (0 <= n[0] < W and 0 <= n[1] < H and n not in seen
                    and g[n[1]][n[0]] != "#"):
                seen.add(n); q.append(n)
    return seen


def dists(g, start):
    d = {start: 0}
    q = collections.deque([start])
    while q:
        x, y = q.popleft()
        for dx, dy in NB4:
            n = (x + dx, y + dy)
            if (0 <= n[0] < W and 0 <= n[1] < H and n not in d
                    and g[n[1]][n[0]] != "#"):
                d[n] = d[(x, y)] + 1
                q.append(n)
    return d


def path_between(g, a, b):
    """The route the game's own guards will walk: downhill through a BFS."""
    d = dists(g, b)
    if a not in d:
        return None
    p, cur = [a], a
    for _ in range(W * H):
        if cur == b:
            return p
        best = None
        for dx, dy in NB4:
            n = (cur[0] + dx, cur[1] + dy)
            if n in d and d[n] < d[cur] and (best is None or d[n] < d[best]):
                best = n
        if best is None:
            return None
        cur = best
        p.append(cur)
    return None


def los(g, a, b):
    """True if nothing solid sits between two tile centres."""
    x0, y0 = a
    x1, y1 = b
    dx, dy = abs(x1 - x0), abs(y1 - y0)
    sx = 1 if x0 < x1 else -1
    sy = 1 if y0 < y1 else -1
    err = dx - dy
    while True:
        if (x0, y0) == (x1, y1):
            return True
        e2 = 2 * err
        if e2 > -dy:
            err -= dy; x0 += sx
        if e2 < dx:
            err += dx; y0 += sy
        if g[y0][x0] == "#":
            return False


def visible_from(g, src, r):
    """Tiles a guard standing here could see. FOV is ignored deliberately: a
    guard sweeps as it turns and dwells, so the reachable-and-in-line set is
    the honest measure of ground it covers over time."""
    out = set()
    sx, sy = src
    for y in range(max(1, sy - r), min(H - 1, sy + r + 1)):
        for x in range(max(1, sx - r), min(W - 1, sx + r + 1)):
            if g[y][x] == "#":
                continue
            if (x - sx) ** 2 + (y - sy) ** 2 > r * r:
                continue
            if los(g, src, (x, y)):
                out.add((x, y))
    return out


def tour(g, start, hostages, exit_t):
    """Greedy nearest-first tour: spawn -> all hostages -> exit."""
    route, cur, left = [], start, list(hostages)
    while left:
        d = dists(g, cur)
        left.sort(key=lambda t: d.get(t, 10 ** 6))
        nxt = left.pop(0)
        seg = path_between(g, cur, nxt)
        if seg is None:
            return None
        route += seg if not route else seg[1:]
        cur = nxt
    seg = path_between(g, cur, exit_t)
    if seg is None:
        return None
    route += seg if not route else seg[1:]
    return route


def carve(rng, g, blocks):
    placed = 0
    for _ in range(blocks * 10):
        if placed >= blocks:
            break
        bw = rng.choice([2, 2, 3, 3, 4, 5, 6])
        bh = rng.choice([2, 2, 3, 3, 4, 5, 6])
        x = rng.randrange(1, W - bw - 1)
        y = rng.randrange(1, H - bh - 1)
        cells = [(xx, yy) for yy in range(y, y + bh) for xx in range(x, x + bw)]
        if any(g[yy][xx] != "." for xx, yy in cells):
            continue
        for xx, yy in cells:
            g[yy][xx] = "#"
        fl = floors(g)
        if not fl or len(reachable(g, fl[0])) != len(fl):
            for xx, yy in cells:
                g[yy][xx] = "."
            continue
        placed += 1
    return placed


def patrol_is_fair(g, start, a, b):
    p = path_between(g, a, b)
    if p is None:
        return None
    for (x, y) in p:
        if (x - start[0]) ** 2 + (y - start[1]) ** 2 < SPAWN_SAFE ** 2:
            return None
    return p


def build(rng, d):
    """d: 0.0 .. 1.0 nominal difficulty, shaping the parameters."""
    g = blank()
    carve(rng, g, 8 + int(d * 21))

    fl = floors(g)
    if len(fl) < 150:
        return None

    start = rng.choice(fl)
    dd = dists(g, start)
    if len(dd) != len(fl):
        return None

    far = max(dd.values())
    want = far * (0.6 + 0.35 * d)
    exit_t = min(dd, key=lambda t: abs(dd[t] - want))
    if dd[exit_t] < 14:
        return None

    n_host = min(MAX_HOSTAGES, 1 + int(d ** 1.1 * 3.4))
    n_guard = min(MAX_GUARDS, 2 + int(d ** 0.75 * 6.2))

    cands = [t for t in fl if dd.get(t, 0) >= 8 and t not in (start, exit_t)]
    rng.shuffle(cands)
    hostages = []
    for t in cands:
        if len(hostages) >= n_host:
            break
        if all(abs(t[0] - h[0]) + abs(t[1] - h[1]) >= 8 for h in hostages):
            hostages.append(t)
    if len(hostages) < n_host:
        return None

    lo = 6 + int(d * 5)
    hi = 14 + int(d * 10)
    guards, paths, tries = [], [], 0
    while len(guards) < n_guard and tries < 1200:
        tries += 1
        a = rng.choice(fl)
        da = dists(g, a)
        opts = [t for t, v in da.items() if lo <= v <= hi]
        if not opts:
            continue
        b = rng.choice(opts)
        if any(abs(a[0] - q[0][0]) + abs(a[1] - q[0][1]) < 4 for q in guards):
            continue
        p = patrol_is_fair(g, start, a, b)
        if p is None:
            continue
        guards.append((a, b))
        paths.append(p)
    if len(guards) < n_guard:
        return None

    rt = tour(g, start, hostages, exit_t)
    if rt is None:
        return None

    # --- measure how exposed that tour actually is -----------------------
    watch = collections.Counter()
    for p in paths:
        seen = set()
        for i in range(0, len(p), 2):          # sample every other tile
            seen |= visible_from(g, p[i], VISION_TILES)
        for t in seen:
            watch[t] += 1

    covered = sum(1 for t in rt if watch.get(t, 0) > 0)
    exposure = covered / len(rt)
    density = (sum(watch.get(t, 0) for t in rt) / len(rt)) / MAX_GUARDS

    opens = sum(sum(1 for dx, dy in NB4 if g[t[1] + dy][t[0] + dx] != "#")
                for t in fl) / len(fl)
    tightness = 1.0 - (opens / 4.0)

    bombs = max(1, 3 - int(d * 1.9))

    score = (0.42 * exposure +
             0.18 * min(1.0, density) +
             0.14 * (len(guards) / MAX_GUARDS) +
             0.12 * min(1.0, len(rt) / 90.0) +
             0.09 * min(1.0, tightness * 2.2) -
             0.05 * (bombs / 3.0))

    g[start[1]][start[0]] = "@"
    g[exit_t[1]][exit_t[0]] = "E"
    for hx, hy in hostages:
        g[hy][hx] = "H"

    return {
        "rows": ["".join(r) for r in g],
        "guards": guards, "bombs": bombs,
        "start": start, "exit": exit_t, "hostages": hostages,
        "score": score, "exposure": exposure, "tour": len(rt),
        "dwell": round(1.30 - d * 0.85, 2),
    }


def validate(lv):
    rows = lv["rows"]
    assert len(rows) == H and all(len(r) == W for r in rows), "geometry"
    assert all(rows[0][x] == "#" and rows[H - 1][x] == "#" for x in range(W)), "top/bottom"
    assert all(rows[y][0] == "#" and rows[y][W - 1] == "#" for y in range(H)), "sides"
    assert sum(r.count("@") for r in rows) == 1, "spawn"
    assert sum(r.count("E") for r in rows) == 1, "exit"
    assert sum(r.count("H") for r in rows) >= 1, "hostages"

    grid = [list(r) for r in rows]
    reach = reachable(grid, lv["start"])
    assert lv["exit"] in reach, "exit unreachable"
    for h in lv["hostages"]:
        assert h in reach, "hostage unreachable"
    free = sum(r.count(c) for r in rows for c in ".@HE")
    assert len(reach) == free, "sealed pocket"
    for a, b in lv["guards"]:
        assert grid[a[1]][a[0]] != "#" and grid[b[1]][b[0]] != "#", "waypoint in wall"
        assert b in reachable(grid, a), "waypoints not mutually reachable"
        assert patrol_is_fair(grid, lv["start"], a, b), "patrol passes the spawn"


def main():
    rng = random.Random(SEED)

    pool, attempts = [], 0
    while len(pool) < POOL:
        attempts += 1
        if attempts > POOL * 300:
            sys.exit("generator failed to converge")
        d = (len(pool) / (POOL - 1)) ** 0.9
        lv = build(rng, d)
        if lv is None:
            continue
        try:
            validate(lv)
        except AssertionError:
            continue
        pool.append(lv)

    # Sorting by the measured score is what makes the ramp monotonic; the
    # parameter sweep only ensures the pool spans a wide enough range.
    pool.sort(key=lambda l: l["score"])

    # Drop the softest tail. These stages follow six hand-built tutorials, so
    # the first generated one should already have teeth rather than repeating
    # what the player just learned.
    floor = int(len(pool) * 0.14)
    usable = pool[floor:]
    step = (len(usable) - 1) / (WANT - 1)
    levels = [usable[int(round(i * step))] for i in range(WANT)]

    names, used = [], set()
    while len(names) < WANT:
        n = f"{rng.choice(ADJ)} {rng.choice(NOUN)}"
        if n not in used and len(n) <= 15:
            used.add(n); names.append(n)

    out = ['// GENERATED by tools/gen_levels.py - do not edit by hand.',
           '//',
           '// 94 procedural stages, following the 6 hand-built ones for 100 total.',
           '// Ordered by a measured difficulty score dominated by how much of the',
           '// route you must walk sits under guard vision. Regenerate with:',
           '//     python3 tools/gen_levels.py',
           '#include "game.h"', '',
           'const level_def_t g_levels_gen[] = {']
    for i, lv in enumerate(levels):
        out.append('{')
        out.append('    .name = "%s",' % names[i])
        out.append('    .hint = "",')
        out.append('    .rows = {')
        for r in lv["rows"]:
            out.append('        "%s",' % r)
        out.append('    },')
        out.append('    .guards = {')
        for a, b in lv["guards"]:
            out.append('        { .wx = {%d, %d}, .wy = {%d, %d}, .wp_count = 2, '
                       '.dwell = %.2ff, .cycle = false },' %
                       (a[0], b[0], a[1], b[1], lv["dwell"]))
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

    print("pool %d (%d attempts) -> %d stages" % (len(pool), attempts, len(levels)))
    print(" stage  score  exposure  guards  hostages  bombs  tour")
    for k in (0, 15, 31, 47, 63, 79, 93):
        lv = levels[k]
        print("  %3d   %.3f    %.2f       %d        %d       %d     %d" %
              (k + 7, lv["score"], lv["exposure"], len(lv["guards"]),
               len(lv["hostages"]), lv["bombs"], lv["tour"]))
    mono = all(levels[i]["score"] <= levels[i + 1]["score"] for i in range(WANT - 1))
    print("monotonic difficulty:", mono)


if __name__ == "__main__":
    main()
