"""SD PRO changes to the patched web UI (after ui_patch.py and ui_i18n.py).

- only the apps this firmware has (weather, clock, alerts, outage, settings) in the app list,
  and no tabs for the others (Bitcoin, stocks, CoinGecko, pictures, PC monitor)
- the sections (views) of those apps are cut out of the page (~24 KB of HTML); byId() hands the
  remaining script a detached element of the same tag for an id that was cut, so it runs unchanged
(per-app seconds and the auto-rotation switch: ui_rotation.py, shared with the SmallTV Pro)

Each patch replaces one exact anchor; a missing anchor fails the build.
"""

import json
import re

SDPRO_APPS = ["weather", "clock", "alerts", "outage", "settings"]

PATCHES = [
    # app list: only the apps built here
    ("APP_ITEMS.sort((a,b)=>APP_ORDER.indexOf(a.key)-APP_ORDER.indexOf(b.key));",
     "APP_ITEMS.splice(0,APP_ITEMS.length,...APP_ITEMS.filter(i=>%s.includes(i.key)));\n"
     "    APP_ITEMS.sort((a,b)=>APP_ORDER.indexOf(a.key)-APP_ORDER.indexOf(b.key));"
     % ("[" + ",".join("'%s'" % k for k in SDPRO_APPS) + "]")),
    # tabs of the apps that are not here
    ('        <button class="tab" data-view="btc">Біткоїн</button>\n', ""),
    ('        <button class="tab" data-view="stock">Акції</button>\n', ""),
    ('        <button class="tab" data-view="cg">CoinGecko</button>\n', ""),
    ('        <button class="tab" data-view="image">Картинки</button>\n', ""),
    ('        <button class="tab" data-view="monitor">Монітор</button>\n', ""),
]

GONE_VIEWS = ["btc", "cg", "stock", "image", "monitor"]
GONE_SLOT_IDS = ["btc-id", "btc-name", "cg-id", "stock-id", "stock-name"]
BYID = "const byId=id=>document.getElementById(id);"


def cut_views(html):
    """Remove the GONE_VIEWS sections; byId() returns a stand-in for the ids that were in them."""
    gone = {}
    for v in GONE_VIEWS:
        start = html.find('<section class="view hidden" id="view-%s"' % v)
        if start < 0:
            raise RuntimeError("ui_sdpro: no view " + v)
        end = html.index("</section>", start) + len("</section>")
        part = html[start:end]
        if part.count("<section") != 1:
            raise RuntimeError("ui_sdpro: nested section in view " + v)
        for tag, i in re.findall(r'<(\w+)[^>]*\bid="([\w-]+)"', part):
            gone[i] = tag
        html = html[:start] + html[end:]
    if html.count(BYID) != 1:
        raise RuntimeError("ui_sdpro: byId not found once")
    # ids the script builds itself inside the cut sections (symbol slots): inputs
    slots = "^(%s)-\\d+$" % "|".join(GONE_SLOT_IDS)
    stub = ("const GONE_IDS=%s,GONE_SLOTS=/%s/,goneEls={};\n"
            "    const byId=id=>document.getElementById(id)||(GONE_IDS[id]||GONE_SLOTS.test(id)?"
            "(goneEls[id]=goneEls[id]||document.createElement(GONE_IDS[id]||'input')):null);"
            % (json.dumps(gone, separators=(",", ":")), slots))
    return html.replace(BYID, stub)


def apply(html):
    for old, new in PATCHES:
        if html.count(old) != 1:
            raise RuntimeError("ui_sdpro: anchor not found once: " + old[:70])
        html = html.replace(old, new)
    html = cut_views(html)
    return html
