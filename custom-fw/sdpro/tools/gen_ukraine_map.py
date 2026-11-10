#!/usr/bin/env python3
"""Generate src/ukraine_map.h: oblast and raion outlines projected to screen pixels.

Sources (neptun.in.ua, derived from OpenStreetMap, ODbL 1.0, (c) OpenStreetMap contributors):
  oblasts (27 incl. Kyiv city and Sevastopol): https://neptun.in.ua/oblasts.geojson
  raions (136, 2020 reform): https://neptun.in.ua/raions.geojson ("key" matches the NEPTUN alerts API)
  raions_meta.json: key -> current name (as used by Klymenko, incl. 2024 renames), old name, oblast index
Both layers come from the same source, so raion and oblast borders line up.

Projection: Web Mercator, like browser maps. Vertices are stored in 1/8 px fixed point so the
firmware can fill by pixel centres without rounding every vertex to a whole pixel.

Usage: gen_ukraine_map.py <oblasts.geojson> <raions.geojson> <raions_meta.json> <out.h>
Regions are indexed in alerts.in.ua IoT order, which the firmware uses everywhere.
"""
import json
import math
import sys

# alerts.in.ua IoT order -> name in neptun oblasts.geojson ("region", or "name" for Kyiv city)
REGIONS = [
    "Автономна Республіка Крим", "Волинська область", "Вінницька область", "Дніпропетровська область",
    "Донецька область", "Житомирська область", "Закарпатська область", "Запорізька область",
    "Івано-Франківська область", "м. Київ", "Київська область", "Кіровоградська область", "Луганська область",
    "Львівська область", "Миколаївська область", "Одеська область", "Полтавська область", "Рівненська область",
    "Севастополь", "Сумська область", "Тернопільська область", "Харківська область", "Херсонська область",
    "Хмельницька область", "Черкаська область", "Чернівецька область", "Чернігівська область",
]
# Screen labels: the oblast's administrative centre (shorter than the oblast name, so it fits on
# one line with the clock). Kyiv oblast is "Київщина" because "Київ" is the city region.
LABELS = [
    "Сімферополь", "Луцьк", "Вінниця", "Дніпро", "Донецьк", "Житомир", "Ужгород", "Запоріжжя",
    "Івано-Франківськ", "Київ", "Київщина", "Кропивницький", "Луганськ", "Львів", "Миколаїв", "Одеса",
    "Полтава", "Рівне", "Севастополь", "Суми", "Тернопіль", "Харків", "Херсон", "Хмельницький",
    "Черкаси", "Чернівці", "Чернігів",
]

# Screen box for the map (the rest of the 240x240 screen is header/footer).
BOX_X, BOX_Y, BOX_W, BOX_H = 2, 2, 236, 160
TOLERANCE_PX = 0.3
FIX = 8  # fixed point: 1/8 px


def rdp(points, eps):
    if len(points) < 3:
        return points
    (x1, y1), (x2, y2) = points[0], points[-1]
    dx, dy = x2 - x1, y2 - y1
    norm = math.hypot(dx, dy) or 1e-9
    idx, dmax = 0, 0.0
    for i in range(1, len(points) - 1):
        px, py = points[i]
        d = abs(dy * px - dx * py + x2 * y1 - y2 * x1) / norm if norm > 1e-9 else math.hypot(px - x1, py - y1)
        if d > dmax:
            idx, dmax = i, d
    if dmax <= eps:
        return [points[0], points[-1]]
    return rdp(points[: idx + 1], eps)[:-1] + rdp(points[idx:], eps)


def simplify_rings(rings_ll, proj):
    """Project, simplify and snap rings to 1/8 px; returns lists of fixed-point points (>= 3 each)."""
    out = []
    for ring in rings_ll:
        q = []
        for x, y in rdp([proj(p) for p in ring], TOLERANCE_PX):
            xy = (int(round(x * FIX)), int(round(y * FIX)))
            if not q or q[-1] != xy:
                q.append(xy)
        if len(q) >= 3:
            out.append(q)
    return out


def mercator(lon, lat):
    return math.radians(lon), -math.log(math.tan(math.pi / 4 + math.radians(lat) / 2))  # y grows southwards


def main(src, raions_src, meta_src, dst):
    feats = {}
    for f in json.load(open(src, encoding="utf-8"))["features"]:
        p = f["properties"]
        feats[p.get("region") or p.get("name")] = f["geometry"]
    rings_ll = []
    for name in REGIONS:
        g = feats[name]
        polys = g["coordinates"] if g["type"] == "MultiPolygon" else [g["coordinates"]]
        rings_ll.append([ring for poly in polys for ring in poly])

    xy = [mercator(*p) for r in rings_ll for ring in r for p in ring]
    x0, x1 = min(p[0] for p in xy), max(p[0] for p in xy)
    y0, y1 = min(p[1] for p in xy), max(p[1] for p in xy)
    scale = min(BOX_W / (x1 - x0), BOX_H / (y1 - y0))
    ox = BOX_X + (BOX_W - (x1 - x0) * scale) / 2
    oy = BOX_Y + (BOX_H - (y1 - y0) * scale) / 2

    def proj(p):
        x, y = mercator(*p)
        return (x - x0) * scale + ox, (y - y0) * scale + oy

    points, rings, regions = [], [], []
    for r in rings_ll:
        first_ring = len(rings)
        for q in simplify_rings(r, proj):
            rings.append((len(points), len(q)))
            points.extend(q)
        regions.append((first_ring, len(rings) - first_ring))

    # Raions, in the same projection. Outer rings only (raions have no holes worth drawing).
    meta = {m["key"]: m for m in json.load(open(meta_src, encoding="utf-8"))}
    raions = []
    for f in json.load(open(raions_src, encoding="utf-8"))["features"]:
        m = meta[f["properties"]["key"]]
        g = f["geometry"]
        polys = g["coordinates"] if g["type"] == "MultiPolygon" else [g["coordinates"]]
        first_ring = len(rings)
        for q in simplify_rings([poly[0] for poly in polys], proj):
            rings.append((len(points), len(q)))
            points.extend(q)
        if len(rings) > first_ring:
            raions.append((first_ring, len(rings) - first_ring, m))
    raions.sort(key=lambda r: (r[2]["oblast"], r[2]["name"]))

    with open(dst, "w", encoding="utf-8") as f:
        w = f.write
        w("// Generated by tools/gen_ukraine_map.py. Do not edit.\n")
        w("// Oblasts and raions: neptun.in.ua oblasts/raions.geojson, ODbL 1.0, (c) OpenStreetMap contributors.\n")
        w("// Web Mercator; MAP_POINTS are screen pixels in 1/%d px fixed point (MAP_FIX).\n" % FIX)
        w("// Data is defined in the one translation unit that defines UKRAINE_MAP_DEFINE.\n")
        w("#pragma once\n#include <stdint.h>\n\n")
        w("struct MapRing { uint16_t first, count; };\n")
        w("struct MapRegion { uint8_t firstRing, rings; const char *label; };\n")
        w("// key: NEPTUN raion key; name: current name (Klymenko); alt: old name or \"\"\n")
        w("struct MapRaion { uint16_t firstRing; uint8_t rings, oblast; const char *key, *name, *alt; };\n\n")
        w("static const int MAP_REGIONS = %d;\nstatic const int MAP_RAIONS = %d;\nstatic const int MAP_FIX = %d;\n\n" % (len(regions), len(raions), FIX))
        w("extern const int16_t MAP_POINTS[][2];\nextern const MapRing MAP_RINGS[];\n")
        w("extern const MapRegion MAP[MAP_REGIONS];  // index = alerts.in.ua IoT order\n")
        w("extern const MapRaion RAION[MAP_RAIONS];  // sorted by oblast, then name\n\n")
        w("#ifdef UKRAINE_MAP_DEFINE\nconst int16_t MAP_POINTS[][2] = {\n")
        for i in range(0, len(points), 10):
            w("  " + "".join("{%d,%d}," % p for p in points[i:i + 10]) + "\n")
        w("};\nconst MapRing MAP_RINGS[] = {\n")
        w("".join("  {%d,%d},\n" % r for r in rings))
        w("};\nconst MapRegion MAP[MAP_REGIONS] = {\n")
        for (fr, nr), label in zip(regions, LABELS):
            w('  {%d,%d,"%s"},\n' % (fr, nr, label))
        w("};\nconst MapRaion RAION[MAP_RAIONS] = {\n")
        for fr, nr, m in raions:
            w('  {%d,%d,%d,"%s","%s","%s"},\n' % (fr, nr, m["oblast"], m["key"], m["name"], m["alt"]))
        w("};\n#endif\n")
    print("%s: %d regions, %d raions, %d rings, %d points (%d bytes)" % (dst, len(regions), len(raions), len(rings), len(points), 4 * len(points)))


if __name__ == "__main__":
    main(*sys.argv[1:5])
