"""SD PRO changes to the patched web UI (after ui_patch.py and ui_i18n.py).

- only the apps this firmware has (weather, clock, alerts, outage, settings) in the app list,
  and no tabs for the others (Bitcoin, stocks, CoinGecko, pictures, PC monitor)
- the sections (views) of those apps are cut out of the page (~24 KB of HTML); byId() hands the
  remaining script a detached element of the same tag for an id that was cut, so it runs unchanged
- the device has no touch, so the apps take turns: a "seconds on screen" field per app,
  saved as /set?dur_<key>=<s> into /.sys/app.json {"dur":{key:s}} (0 = stays on that app)

Each patch replaces one exact anchor; a missing anchor fails the build.
"""

import json
import re

SDPRO_APPS = ["weather", "clock", "alerts", "outage", "settings"]
DEFAULT_SEC = 15

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
    # per-app seconds: field in each app card
    ('<div class="app-name">${esc(item.label)}</div></div>`).join(\'\');',
     '<div class="app-name">${esc(item.label)}</div>'
     '<label class="app-sec"><input class="field" id="app-sec-${item.key}" type="number" min="0" max="3600" step="1" '
     'data-app-sec="${esc(item.key)}"><span>с</span></label></div>`).join(\'\');'),
    ('    .app-name{grid-area:name;',
     '    .app-row{grid-template-areas:"toggle open" "name name" "sec sec"}\n'
     '    .app-sec{grid-area:sec;display:flex;align-items:center;gap:6px;color:var(--muted);font-size:13px}\n'
     '    .app-sec .field{width:84px;padding:6px 8px}\n'
     '    .app-name{grid-area:name;'),
    ('<div class="hint">Вимкнені застосунки пропускаються при запуску й перемиканні.</div>',
     '<div class="hint">Вимкнені застосунки пропускаються при запуску й перемиканні. Застосунки змінюють один одного '
     'самі: під кожним — скільки секунд його показувати (0 — лишатися на ньому).</div>'),
    # state: load "dur" with the app flags, show it, read it back, send it with the app settings
    ("        ensureValidBootTheme();\n        state.settings.ssid=String(config.a??'');",
     "        const appDur=app&&app.dur&&typeof app.dur==='object'?app.dur:{};\n"
     "        state.settings.dur=Object.fromEntries(APP_ITEMS.map(item=>[item.key,toInt(appDur[item.key],%d)]));\n"
     "        ensureValidBootTheme();\n        state.settings.ssid=String(config.a??'');" % DEFAULT_SEC),
    ("    function syncAppControls(){\n      APP_ITEMS.forEach(item=>{",
     "    function syncAppControls(){\n      APP_ITEMS.forEach(item=>{\n"
     "        const sec=byId('app-sec-'+item.key);\n"
     "        if(sec&&document.activeElement!==sec) sec.value=String((state.settings.dur||{})[item.key]??%d);"
     % DEFAULT_SEC),
    ("        if(input) state.settings.apps[item.key]=input.getAttribute('aria-pressed')==='true';\n      });",
     "        if(input) state.settings.apps[item.key]=input.getAttribute('aria-pressed')==='true';\n"
     "        const sec=byId('app-sec-'+item.key);\n"
     "        if(sec){state.settings.dur=state.settings.dur||{};"
     "state.settings.dur[item.key]=Math.max(0,Math.min(3600,toInt(sec.value,%d)));}\n      });" % DEFAULT_SEC),
    ("        APP_ITEMS.forEach(item=>query.push('app_'+item.key+'='+(state.settings.apps[item.key]?1:0)));",
     "        APP_ITEMS.forEach(item=>query.push('app_'+item.key+'='+(state.settings.apps[item.key]?1:0)));\n"
     "        APP_ITEMS.forEach(item=>query.push('dur_'+item.key+'='+((state.settings.dur||{})[item.key]??%d)));"
     % DEFAULT_SEC),
]

# Save when a seconds field changes.
LISTENER = """
    document.addEventListener('change',e=>{
      const sec=e.target.closest&&e.target.closest('[data-app-sec]');
      if(sec) persistAppSettings('Час показу збережено.');
    });
"""


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
    i = html.rfind("</script>")
    if i < 0:
        raise RuntimeError("ui_sdpro: no </script>")
    return html[:i] + LISTENER + html[i:]
