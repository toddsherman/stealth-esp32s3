#!/usr/bin/env python3
"""Generates main/level_gen.c - 100 stages ordered by measured difficulty.

Difficulty is modelled, not guessed. The obvious knobs - guard count, hostage
count, wall density - turn out to be weak predictors on their own, so they are
inputs to generation but not to the score. What is scored is how hard the level
actually is to move through:

  TEMPORAL COVERAGE
    Each guard is walked around its full patrol cycle, including the dwell at
    each end where it stands and scans. At every step its real vision cone is
    computed - the game's FOV and range, clipped by line of sight - and the
    result accumulated per tile. A tile's coverage is the fraction of the cycle
    it is visible for. This is the key correction over a naive model: a tile
    glimpsed once is not the same as a tile watched constantly, but a static
    union of cone positions scores them identically.

  UNAVOIDABLE CHOKEPOINTS
    Tiles the player cannot route around - articulation points whose removal
    disconnects the spawn from the exit - that also lie on the required tour.
    A watched choke has to be timed rather than avoided, which is the single
    hardest thing this game asks of you. Scored by the coverage of the worst
    one.

  COVER
    How much of the map is never watched, and how far the route runs from the
    nearest unwatched tile. A chasing guard matches the player's speed, so
    breaking line of sight is the only escape; a route with no cover beside it
    is a route with no recovery from a mistake.

  RELIEF
    Sound bombs per guard, which is what lets you displace a patrol. Absolute
    bomb count means little - three bombs against seven guards is scarcity.

Candidates are generated across a wide parameter sweep, scored, sorted, and
sampled evenly across the range, so the ramp is monotonic by construction.

Fairness rules override difficulty: sealed border, fully connected floor,
reachable exit, hostages and waypoints, and no guard patrol may pass within
vision range of the spawn.

Deterministic: a fixed seed reproduces the same 100 stages.
"""
import random, collections, math, sys, heapq, itertools

W, H = 23, 28
MAX_GUARDS, MAX_HOSTAGES = 8, 4
WANT = 100
POOL = 260
SEED = 20260821

# Mirrors of the game's own constants (game.h).
VISION_TILES = 110.0 / 16.0      # GUARD_RANGE / TILE
HALF_FOV     = 1.20 / 2.0        # GUARD_FOV / 2
SPAWN_SAFE   = 8.0

# Every stage must force the player through watched ground. This is the worst
# coverage the *safest possible* route has to accept - if it were zero, the
# stage could be finished without ever entering a cone at all. A brief crossing
# still has to be timed, so even the opening stages teach the core loop.
MIN_CROSSING = 0.16
CYCLE_STEPS  = 4                 # dwell samples at each waypoint

NB4 = ((1, 0), (-1, 0), (0, 1), (0, -1))

ADJ = ["COLD","QUIET","LOW","LONG","DARK","THIN","BLIND","SILENT","SHORT","DEEP",
       "GLASS","IRON","PAPER","SALT","AMBER","SLATE","HOLLOW","NARROW","BITTER",
       "PALE","STILL","SHARP","BLACK","GREY","LAST","FIRST","OPEN","CLOSED",
       "BROKEN","EMPTY","WIDE","HIGH","OLD","NEW","RED","BLUE","SLOW","FAST",
       "LOST","QUIET","SPARE","BLANK","CLEAN","ROUGH","PLAIN","STEEP"]
NOUN = ["WATCH","YARD","HALL","LINE","GATE","VAULT","ROOM","WING","STAIR","DOCK",
        "MARKET","CHAPEL","OFFICE","GARDEN","TUNNEL","LOBBY","ANNEX","DEPOT",
        "STUDY","KITCHEN","CELLAR","ATRIUM","LANDING","GALLERY","COURT","PANTRY",
        "FOYER","ARCHIVE","BRIDGE","TOWER","CRYPT","MILL","FORGE","QUARRY",
        "LEDGE","SHAFT","CANAL","TERRACE","ALCOVE","ROTUNDA"]


# ---------------------------------------------------------------- geometry --
def blank():
    g = [["#"] * W for _ in range(H)]
    for y in range(1, H - 1):
        for x in range(1, W - 1):
            g[y][x] = "."
    return g


def floors(g):
    return [(x, y) for y in range(H) for x in range(W) if g[y][x] != "#"]


def reachable(g, start, blocked=None):
    seen = {start}
    q = collections.deque([start])
    while q:
        x, y = q.popleft()
        for dx, dy in NB4:
            n = (x + dx, y + dy)
            if n == blocked or n in seen:
                continue
            if 0 <= n[0] < W and 0 <= n[1] < H and g[n[1]][n[0]] != "#":
                seen.add(n); q.append(n)
    return seen


def dists(g, start):
    d = {start: 0}
    q = collections.deque([start])
    while q:
        x, y = q.popleft()
        for dx, dy in NB4:
            n = (x + dx, y + dy)
            if n not in d and 0 <= n[0] < W and 0 <= n[1] < H and g[n[1]][n[0]] != "#":
                d[n] = d[(x, y)] + 1
                q.append(n)
    return d


def path_between(g, a, b):
    """The route the game's guards actually walk: downhill through a BFS."""
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
    x0, y0 = a
    x1, y1 = b
    dx, dy = abs(x1 - x0), abs(y1 - y0)
    sx = 1 if x0 < x1 else -1
    sy = 1 if y0 < y1 else -1
    err = dx - dy
    while (x0, y0) != (x1, y1):
        e2 = 2 * err
        if e2 > -dy:
            err -= dy; x0 += sx
        if e2 < dx:
            err += dx; y0 += sy
        if g[y0][x0] == "#":
            return False
    return True


def visible_set(g, src, cache):
    """Tiles in range of src with clear line of sight, ignoring facing.
    Cached per tile - a patrol revisits the same tiles many times."""
    if src in cache:
        return cache[src]
    out = []
    sx, sy = src
    r = int(VISION_TILES)
    for y in range(max(1, sy - r), min(H - 1, sy + r + 1)):
        for x in range(max(1, sx - r), min(W - 1, sx + r + 1)):
            if g[y][x] == "#":
                continue
            ddx, ddy = x - sx, y - sy
            if ddx * ddx + ddy * ddy > VISION_TILES * VISION_TILES:
                continue
            if los(g, src, (x, y)):
                out.append(((x, y), math.atan2(ddy, ddx)))
    cache[src] = out
    return out


def articulation_points(g, cells):
    """Tarjan. Tiles whose removal splits the walkable graph."""
    index, low, parent = {}, {}, {}
    aps, counter = set(), [0]
    for root in cells:
        if root in index:
            continue
        stack = [(root, iter([(root[0] + d[0], root[1] + d[1]) for d in NB4]))]
        index[root] = low[root] = counter[0]; counter[0] += 1
        parent[root] = None
        root_children = 0
        while stack:
            node, it = stack[-1]
            advanced = False
            for nb in it:
                if not (0 <= nb[0] < W and 0 <= nb[1] < H) or g[nb[1]][nb[0]] == "#":
                    continue
                if nb not in index:
                    parent[nb] = node
                    index[nb] = low[nb] = counter[0]; counter[0] += 1
                    if node == root:
                        root_children += 1
                    stack.append((nb, iter([(nb[0] + d[0], nb[1] + d[1]) for d in NB4])))
                    advanced = True
                    break
                if nb != parent[node]:
                    low[node] = min(low[node], index[nb])
            if not advanced:
                stack.pop()
                if stack:
                    up = stack[-1][0]
                    low[up] = min(low[up], low[node])
                    if parent[up] is not None and low[node] >= index[up]:
                        aps.add(up)
        if root_children > 1:
            aps.add(root)
    return aps


# ------------------------------------------------------------- generation --
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


def tour(g, start, hostages, exit_t):
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


def temporal_coverage(g, paths, cache):
    """Per-tile probability that *some* guard can see it at a random moment.

    Each guard is normalised by its own cycle length, then the guards are
    combined as independent observers: 1 - prod(1 - c_i). Normalising by the
    summed length of every guard's cycle instead - the obvious mistake - makes
    a tile under one guard's constant watch score 1/N rather than 1, which
    flattens the very signal this is here to measure.

    The guard is walked out and back along its path, since patrols ping-pong,
    facing its direction of travel, and dwelling at each end where the game
    sweeps its facing rather than moving.
    """
    per_guard = []

    for p in paths:
        counts = collections.Counter()
        steps = 0
        seq = p + p[::-1][1:]
        for i, tile in enumerate(seq):
            nxt = seq[(i + 1) % len(seq)]
            if nxt == tile:
                headings = [0.0, math.pi / 2, math.pi, -math.pi / 2]
            else:
                headings = [math.atan2(nxt[1] - tile[1], nxt[0] - tile[0])]
            if i == 0 or i == len(p) - 1:
                base = headings[0]
                headings = [base + k * 0.5 for k in (-1, 0, 1)] * CYCLE_STEPS

            for facing in headings:
                steps += 1
                for (t, ang) in visible_set(g, tile, cache):
                    d = (ang - facing + math.pi) % (2 * math.pi) - math.pi
                    if abs(d) <= HALF_FOV:
                        counts[t] += 1

        if steps:
            per_guard.append({t: c / steps for t, c in counts.items()})

    combined = {}
    for cov in per_guard:
        for t, c in cov.items():
            combined[t] = 1.0 - (1.0 - combined.get(t, 0.0)) * (1.0 - c)
    return combined


def bottleneck_from(g, cov, src):
    """For every tile, the least-exposed route from src: the minimum over all
    paths of the *worst* coverage encountered along it.

    This is the number that matters for "must you cross a guard". Averaging
    coverage over one chosen route says nothing, because the player picks the
    route - and will pick the one that keeps its worst moment lowest. A
    max-metric Dijkstra answers exactly that.
    """
    best = {src: cov.get(src, 0.0)}
    pq = [(best[src], src)]
    while pq:
        c, t = heapq.heappop(pq)
        if c > best.get(t, 2.0):
            continue
        for dx, dy in NB4:
            n = (t[0] + dx, t[1] + dy)
            if not (0 <= n[0] < W and 0 <= n[1] < H) or g[n[1]][n[0]] == "#":
                continue
            nc = max(c, cov.get(n, 0.0))
            if nc < best.get(n, 2.0):
                best[n] = nc
                heapq.heappush(pq, (nc, n))
    return best


def safest_crossing(g, cov, start, hostages, exit_t):
    """The worst coverage the player must accept, assuming perfect play.

    Every ordering of the hostages is tried and the best one taken, because
    the player is free to choose. If this is zero there is a way through the
    stage that never enters a cone at all.
    """
    nodes = [start] + list(hostages) + [exit_t]
    table = {n: bottleneck_from(g, cov, n) for n in nodes}

    best = 2.0
    for order in itertools.permutations(range(len(hostages))):
        legs, cur = 0.0, start
        ok = True
        for idx in order:
            nxt = hostages[idx]
            v = table[cur].get(nxt)
            if v is None:
                ok = False; break
            legs = max(legs, v)
            cur = nxt
        if not ok:
            continue
        v = table[cur].get(exit_t)
        if v is None:
            continue
        legs = max(legs, v)
        best = min(best, legs)
    return None if best > 1.5 else best


def build(rng, d):
    g = blank()
    carve(rng, g, int(6 + d * 26))

    fl = floors(g)
    if len(fl) < 150:
        return None

    start = rng.choice(fl)
    dd = dists(g, start)
    if len(dd) != len(fl):
        return None

    far = max(dd.values())
    exit_t = min(dd, key=lambda t: abs(dd[t] - far * (0.6 + 0.35 * d)))
    if dd[exit_t] < 12:
        return None

    n_host = min(MAX_HOSTAGES, 1 + int(d ** 1.1 * 3.4))
    n_guard = min(MAX_GUARDS, 2 + int(d ** 0.7 * 6.2))

    cands = [t for t in fl if dd.get(t, 0) >= 6 and t not in (start, exit_t)]
    rng.shuffle(cands)
    hostages = []
    for t in cands:
        if len(hostages) >= n_host:
            break
        if all(abs(t[0] - h[0]) + abs(t[1] - h[1]) >= 7 for h in hostages):
            hostages.append(t)
    if len(hostages) < n_host:
        return None

    lo, hi = 5 + int(d * 5), 13 + int(d * 11)
    guards, paths, tries = [], [], 0
    while len(guards) < n_guard and tries < 1400:
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
        guards.append((a, b)); paths.append(p)
    if len(guards) < n_guard:
        return None

    rt = tour(g, start, hostages, exit_t)
    if rt is None:
        return None

    # ---- measure -------------------------------------------------------
    cache = {}
    cov = temporal_coverage(g, paths, cache)

    exposure = sum(cov.get(t, 0.0) for t in rt) / len(rt)

    # Hard requirement: there must be no way to collect every hostage and
    # reach the exit without crossing ground a guard watches. A stage that can
    # be completed without ever entering a cone is not a stealth stage.
    crossing = safest_crossing(g, cov, start, hostages, exit_t)
    if crossing is None or crossing < MIN_CROSSING:
        return None

    # Unavoidable tiles: articulation points on the route whose removal
    # actually severs the spawn from the exit.
    aps = articulation_points(g, fl)
    chokes = []
    for t in set(rt) & aps:
        if t in (start, exit_t) or t in hostages:
            continue
        if exit_t not in reachable(g, start, blocked=t):
            chokes.append(t)
    choke = max((cov.get(t, 0.0) for t in chokes), default=0.0)

    safe = [t for t in fl if cov.get(t, 0.0) < 0.02]
    safe_frac = len(safe) / len(fl)

    # How far the route runs from anywhere unwatched.
    if safe:
        sd = collections.deque([(t, 0) for t in safe])
        seen = {t: 0 for t in safe}
        while sd:
            (x, y), c = sd.popleft()
            for dx, dy in NB4:
                n = (x + dx, y + dy)
                if n not in seen and 0 <= n[0] < W and 0 <= n[1] < H and g[n[1]][n[0]] != "#":
                    seen[n] = c + 1
                    sd.append((n, c + 1))
        cover_dist = sum(seen.get(t, 12) for t in rt) / len(rt)
    else:
        cover_dist = 12.0

    bombs = max(1, 3 - int(d * 1.7))
    relief = min(1.0, bombs / max(1, len(guards)))

    score = (0.26 * min(1.0, crossing * 2.0) +
             0.16 * min(1.0, exposure * 1.6) +
             0.20 * choke +
             0.14 * (1.0 - safe_frac) +
             0.12 * min(1.0, cover_dist / 6.0) +
             0.10 * (len(guards) / MAX_GUARDS) +
             0.06 * min(1.0, len(rt) / 90.0) -
             0.10 * relief)

    g[start[1]][start[0]] = "@"
    g[exit_t[1]][exit_t[0]] = "E"
    for hx, hy in hostages:
        g[hy][hx] = "H"

    return {
        "rows": ["".join(r) for r in g], "guards": guards, "bombs": bombs,
        "start": start, "exit": exit_t, "hostages": hostages,
        "score": score, "exposure": exposure, "choke": choke,
        "crossing": crossing,
        "safe": safe_frac, "cover": cover_dist, "tour": len(rt),
        "chokes": len(chokes), "dwell": round(1.40 - d * 0.95, 2),
    }


def validate(lv):
    rows = lv["rows"]
    assert len(rows) == H and all(len(r) == W for r in rows), "geometry"
    assert all(rows[0][x] == "#" and rows[H - 1][x] == "#" for x in range(W)), "top/bottom"
    assert all(rows[y][0] == "#" and rows[y][W - 1] == "#" for y in range(H)), "sides"
    assert sum(r.count("@") for r in rows) == 1, "spawn"
    assert sum(r.count("E") for r in rows) == 1, "exit"
    assert sum(r.count("H") for r in rows) >= 1, "hostages"
    assert len(lv["guards"]) <= MAX_GUARDS, "too many guards"

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
    # The stage must not be completable without entering a cone.
    assert lv["crossing"] >= MIN_CROSSING, "a route avoids every guard"


def main():
    rng = random.Random(SEED)
    pool, attempts = [], 0
    while len(pool) < POOL:
        attempts += 1
        if attempts > POOL * 200:
            sys.exit("generator failed to converge")
        d = (len(pool) / (POOL - 1)) ** 0.85
        lv = build(rng, d)
        if lv is None:
            continue
        try:
            validate(lv)
        except AssertionError:
            continue
        pool.append(lv)
        if len(pool) % 40 == 0:
            print("  ...%d candidates" % len(pool), flush=True)

    pool.sort(key=lambda l: l["score"])
    step = (len(pool) - 1) / (WANT - 1)
    levels = [pool[int(round(i * step))] for i in range(WANT)]

    names, used = [], set()
    while len(names) < WANT:
        n = f"{rng.choice(ADJ)} {rng.choice(NOUN)}"
        if n not in used and len(n) <= 15:
            used.add(n); names.append(n)

    out = ['// GENERATED by tools/gen_levels.py - do not edit by hand.',
           '//',
           '// 100 stages ordered by a measured difficulty score built from temporal',
           '// guard coverage, unavoidable watched chokepoints, and how far the',
           '// required route runs from cover. Regenerate with:',
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

    print("\npool %d (%d attempts) -> %d stages" % (len(pool), attempts, len(levels)))
    print(" stage  score  cross  expo  choke  safe  guards  host  bombs  tour")
    for k in (0, 11, 24, 37, 49, 62, 74, 87, 99):
        l = levels[k]
        print("  %3d   %.3f   %.2f  %.2f   %.2f  %.2f      %d      %d      %d    %d" %
              (k + 1, l["score"], l["crossing"], l["exposure"], l["choke"],
               l["safe"], len(l["guards"]), len(l["hostages"]), l["bombs"],
               l["tour"]))
    mono = all(levels[i]["score"] <= levels[i + 1]["score"] for i in range(WANT - 1))
    worst = min(l["crossing"] for l in levels)
    print("monotonic:", mono)
    print("every stage forces a guard crossing: %s (weakest %.2f, floor %.2f)"
          % (worst >= MIN_CROSSING, worst, MIN_CROSSING))


if __name__ == "__main__":
    main()
