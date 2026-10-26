#!/usr/bin/env python3
"""Generate src/ukraine_map.h: oblast and raion outlines projected to screen pixels.

Sources (both derived from OpenStreetMap, ODbL 1.0, (c) OpenStreetMap contributors):
  oblasts: geoBoundaries UKR ADM1 (simplified)
    https://github.com/wmgeolab/geoBoundaries/raw/9469f09/releaseData/gbOpen/UKR/ADM1/geoBoundaries-UKR-ADM1_simplified.geojson
  raions (136, 2020 reform): https://neptun.in.ua/raions.geojson ("key" matches the NEPTUN alerts API)
  raions_meta.json: key -> current name (as used by Klymenko, incl. 2024 renames), old name, oblast index

Usage: gen_ukraine_map.py <oblasts.geojson> <raions.geojson> <raions_meta.json> <out.h>
Regions are indexed in alerts.in.ua IoT order, which the firmware uses everywhere.
"""
import json
import math
import sys

# alerts.in.ua IoT order -> geoBoundaries shapeName
REGIONS = [
    "Autonomous Republic of Crimea", "Volyn Oblast", "Vinnytsia Oblast", "Dnipropetrovsk Oblast",
    "Donetsk Oblast", "Zhytomyr Oblast", "Zakarpattia Oblast", "Zaporizhia Oblast",
    "Ivano-Frankivsk Oblast", "Kyiv", "Kyiv Oblast", "Kirovohrad Oblast", "Luhansk Oblast",
    "Lviv Oblast", "Mykolaiv Oblast", "Odessa Oblast", "Poltava Oblast", "Rivne Oblast",
    "Sevastopol", "Sumy Oblast", "Ternopil Oblast", "Kharkiv Oblast", "Kherson Oblast",
    "Khmelnytskyi Oblast", "Cherkasy Oblast", "Chernivtsi Oblast", "Chernihiv Oblast",
]
# Short Ukrainian labels for the screen (drawn with the Cyrillic smooth fonts).
LABELS = [
    "АР Крим", "Волинська", "Вінницька", "Дніпропетровська", "Донецька", "Житомирська", "Закарпатська",
    "Запорізька", "Івано-Франківська", "м. Київ", "Київська", "Кіровоградська", "Луганська", "Львівська",
    "Миколаївська", "Одеська", "Полтавська", "Рівненська", "Севастополь", "Сумська", "Тернопільська",
    "Харківська", "Херсонська", "Хмельницька", "Черкаська", "Чернівецька", "Чернігівська",
]

# Screen box for the map (the rest of the 240x240 screen is header/footer).
BOX_X, BOX_Y, BOX_W, BOX_H = 2, 22, 236, 160
TOLERANCE_PX = 0.7


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
    """Project, simplify and pixel-snap rings; returns list of point lists (>= 3 points each)."""
    out = []
    for ring in rings_ll:
        q = []
        for x, y in rdp([proj(p) for p in ring], TOLERANCE_PX):
            xy = (int(round(x)), int(round(y)))
            if not q or q[-1] != xy:
                q.append(xy)
        if len(q) >= 3:
            out.append(q)
    return out


def main(src, raions_src, meta_src, dst):
    feats = {f["properties"]["shapeName"]: f["geometry"] for f in json.load(open(src))["features"]}
    rings_ll = []
    for name in REGIONS:
        g = feats[name]
        polys = g["coordinates"] if g["type"] == "MultiPolygon" else [g["coordinates"]]
        rings_ll.append([ring for poly in polys for ring in poly])

    lons = [p[0] for r in rings_ll for ring in r for p in ring]
    lats = [p[1] for r in rings_ll for ring in r for p in ring]
    lon0, lon1, lat0, lat1 = min(lons), max(lons), min(lats), max(lats)
    kx = math.cos(math.radians((lat0 + lat1) / 2))
    scale = min(BOX_W / ((lon1 - lon0) * kx), BOX_H / (lat1 - lat0))
    ox = BOX_X + (BOX_W - (lon1 - lon0) * kx * scale) / 2
    oy = BOX_Y + (BOX_H - (lat1 - lat0) * scale) / 2

    def proj(p):
        return ((p[0] - lon0) * kx * scale + ox, (lat1 - p[1]) * scale + oy)

    points, rings, regions = [], [], []
    for r in rings_ll:
        first_ring = len(rings)
        sx = sy = n = 0
        for q in simplify_rings(r, proj):
            rings.append((len(points), len(q)))
            points.extend(q)
            for x, y in q:
                sx, sy, n = sx + x, sy + y, n + 1
        regions.append((first_ring, len(rings) - first_ring, round(sx / n), round(sy / n)))

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
        w("// Oblasts: geoBoundaries UKR ADM1; raions: neptun.in.ua raions.geojson. ODbL 1.0, (c) OpenStreetMap contributors.\n")
        w("// Data is defined in the one translation unit that defines UKRAINE_MAP_DEFINE.\n")
        w("#pragma once\n#include <stdint.h>\n\n")
        w("struct MapRing { uint16_t first, count; };\n")
        w("struct MapRegion { uint8_t firstRing, rings; uint8_t cx, cy; const char *label; };\n")
        w("// key: NEPTUN raion key; name: current name (Klymenko); alt: old name or \"\"\n")
        w("struct MapRaion { uint16_t firstRing; uint8_t rings, oblast; const char *key, *name, *alt; };\n\n")
        w("static const int MAP_REGIONS = %d;\nstatic const int MAP_RAIONS = %d;\n\n" % (len(regions), len(raions)))
        w("extern const uint8_t MAP_POINTS[][2];\nextern const MapRing MAP_RINGS[];\n")
        w("extern const MapRegion MAP[MAP_REGIONS];  // index = alerts.in.ua IoT order\n")
        w("extern const MapRaion RAION[MAP_RAIONS];  // sorted by oblast, then name\n\n")
        w("#ifdef UKRAINE_MAP_DEFINE\nconst uint8_t MAP_POINTS[][2] = {\n")
        for i in range(0, len(points), 10):
            w("  " + "".join("{%d,%d}," % p for p in points[i:i + 10]) + "\n")
        w("};\nconst MapRing MAP_RINGS[] = {\n")
        w("".join("  {%d,%d},\n" % r for r in rings))
        w("};\nconst MapRegion MAP[MAP_REGIONS] = {\n")
        for (fr, nr, cx, cy), label in zip(regions, LABELS):
            w('  {%d,%d,%d,%d,"%s"},\n' % (fr, nr, cx, cy, label))
        w("};\nconst MapRaion RAION[MAP_RAIONS] = {\n")
        for fr, nr, m in raions:
            w('  {%d,%d,%d,"%s","%s","%s"},\n' % (fr, nr, m["oblast"], m["key"], m["name"], m["alt"]))
        w("};\n#endif\n")
    print("%s: %d regions, %d raions, %d rings, %d points (%d bytes)" % (dst, len(regions), len(raions), len(rings), len(points), 2 * len(points)))


if __name__ == "__main__":
    main(*sys.argv[1:5])
