"""Auto rotation in the web UI, the same on the SmallTV Pro and the SD PRO (after ui_patch.py and ui_i18n.py).

- a switch "Автоперемикання": the enabled apps take turns by themselves (/set?auto=1|0, app.json "auto")
- a "seconds on screen" field in each app card: /set?dur_<key>=<s> into app.json {"dur":{key:s}}
  (0 = stays on that app)

Each patch replaces one exact anchor; a missing anchor fails the build.
Keep the copies in firmware/tools and sdpro/tools identical.
"""

DEFAULT_SEC = 15

PATCHES = [
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
     '<div class="hint">Вимкнені застосунки пропускаються при запуску й перемиканні.</div>'
     '<label class="line"><div><strong>Автоперемикання</strong><span>Застосунки змінюють один одного самі: під кожним — '
     'скільки секунд його показувати (0 — лишатися на ньому).</span></div>'
     '<span class="switch"><input id="appAuto" type="checkbox"></span></label>'),
    # state: load "dur" with the app flags, show it, read it back, send it with the app settings
    ("        ensureValidBootTheme();\n        state.settings.ssid=String(config.a??'');",
     "        const appDur=app&&app.dur&&typeof app.dur==='object'?app.dur:{};\n"
     "        state.settings.dur=Object.fromEntries(APP_ITEMS.map(item=>[item.key,toInt(appDur[item.key],%d)]));\n"
     "        state.settings.auto=toInt(app&&app.auto,1)!==0;\n"
     "        ensureValidBootTheme();\n        state.settings.ssid=String(config.a??'');" % DEFAULT_SEC),
    ("    function syncAppControls(){\n      APP_ITEMS.forEach(item=>{",
     "    function syncAppControls(){\n"
     "      const auto=byId('appAuto');\n"
     "      if(auto) auto.checked=state.settings.auto!==false;\n"
     "      APP_ITEMS.forEach(item=>{\n"
     "        const sec=byId('app-sec-'+item.key);\n"
     "        if(sec&&document.activeElement!==sec) sec.value=String((state.settings.dur||{})[item.key]??%d);"
     % DEFAULT_SEC),
    ("        if(input) state.settings.apps[item.key]=input.getAttribute('aria-pressed')==='true';\n      });",
     "        if(input) state.settings.apps[item.key]=input.getAttribute('aria-pressed')==='true';\n"
     "        const sec=byId('app-sec-'+item.key);\n"
     "        if(sec){state.settings.dur=state.settings.dur||{};"
     "state.settings.dur[item.key]=Math.max(0,Math.min(3600,toInt(sec.value,%d)));}\n      });\n"
     "      if(byId('appAuto')) state.settings.auto=byId('appAuto').checked;" % DEFAULT_SEC),
    ("        APP_ITEMS.forEach(item=>query.push('app_'+item.key+'='+(state.settings.apps[item.key]?1:0)));",
     "        APP_ITEMS.forEach(item=>query.push('app_'+item.key+'='+(state.settings.apps[item.key]?1:0)));\n"
     "        APP_ITEMS.forEach(item=>query.push('dur_'+item.key+'='+((state.settings.dur||{})[item.key]??%d)));\n"
     "        query.push('auto='+(state.settings.auto===false?0:1));"
     % DEFAULT_SEC),
]

# Save when a seconds field or the switch changes.
LISTENER = """
    document.addEventListener('change',e=>{
      const sec=e.target.closest&&e.target.closest('[data-app-sec]');
      if(sec) persistAppSettings('Час показу збережено.');
      if(e.target.id==='appAuto') persistAppSettings(e.target.checked?'Автоперемикання ввімкнено.':'Автоперемикання вимкнено.');
    });
"""


def apply(html):
    for old, new in PATCHES:
        if html.count(old) != 1:
            raise RuntimeError("ui_rotation: anchor not found once: " + old[:70])
        html = html.replace(old, new)
    i = html.rfind("</script>")
    if i < 0:
        raise RuntimeError("ui_rotation: no </script>")
    return html[:i] + LISTENER + html[i:]
