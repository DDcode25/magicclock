"""OpenFW additions to the stock web UI (web/stock_index.html).

Each patch replaces one exact anchor in the stock page; a missing anchor fails the build
instead of silently shipping a broken UI.
"""
import re

REGIONS_UK = [
    "АР Крим", "Волинська область", "Вінницька область", "Дніпропетровська область",
    "Донецька область", "Житомирська область", "Закарпатська область", "Запорізька область",
    "Івано-Франківська область", "м. Київ", "Київська область", "Кіровоградська область",
    "Луганська область", "Львівська область", "Миколаївська область", "Одеська область",
    "Полтавська область", "Рівненська область", "Севастополь", "Сумська область",
    "Тернопільська область", "Харківська область", "Херсонська область", "Хмельницька область",
    "Черкаська область", "Чернівецька область", "Чернігівська область",
]

_home_options = '<option value="-1">— none —</option>' + "".join(
    '<option value="%d">%s</option>' % (i, n) for i, n in sorted(enumerate(REGIONS_UK), key=lambda x: x[1])
)

ALERTS_VIEW = """
    <section class="view hidden" id="view-alerts">
      <div class="panel hero">
        <div>
          <div class="eyebrow">Air alerts</div>
          <h1>Ukraine Alert Map</h1>
          <p class="lead">Map of oblasts coloured by air raid alerts: red = whole oblast, orange = some districts.</p>
        </div>
        <div class="hero-side">
          <div class="chip-row">
            <div class="chip"><span>Source</span><b id="alertsSrcLabel">-</b></div>
            <div class="chip"><span>Refresh</span><b id="alertsIntervalLabel">30s</b></div>
          </div>
          <div class="chip-row">
            <button class="btn hero-open" type="button" data-open-app="Alerts" data-app-key="alerts" data-app-label="Air alerts">Open This App</button>
          </div>
        </div>
      </div>

      <div class="grid2">
        <div class="panel">
          <div class="section-head">
            <div>
              <div class="eyebrow">Data</div>
              <h2>Source</h2>
            </div>
          </div>
          <div class="stack">
            <label class="pair"><span>Source</span><select class="select" id="alertsSrc">
              <option value="klymenko">vadimklimenko.com (no token, oblast + districts)</option>
              <option value="neptun">neptun.in.ua (no token, oblast + districts)</option>
              <option value="ubilling">ubilling.net.ua (no token, oblast only)</option>
              <option value="alertsinua">alerts.in.ua (token required)</option>
            </select></label>
            <label class="pair"><span>alerts.in.ua token</span><input class="field" id="alertsToken" placeholder="only for alerts.in.ua" autocomplete="off"></label>
            <label class="pair"><span>Refresh Interval (sec, min 15)</span><input class="field" id="alertsInterval" inputmode="numeric" placeholder="30"></label>
            <div class="hint">Token: write to <b>api@alerts.in.ua</b>. Limit about 10 requests per minute.</div>
            <div class="hint">Дані: <a href="https://neptun.in.ua/" target="_blank" rel="noopener noreferrer">Карта повітряних тривог — NEPTUN</a>, vadimklimenko.com, ubilling.net.ua, alerts.in.ua.
              These are aggregators, not the official alert system: always follow official air raid signals.</div>
          </div>
        </div>

        <div class="panel">
          <div class="section-head">
            <div>
              <div class="eyebrow">Display</div>
              <h2>Home region</h2>
            </div>
          </div>
          <div class="stack">
            <label class="pair"><span>Highlight and show status for</span><select class="select" id="alertsHome">%s</select></label>
            <div class="hint">The region is outlined in white; its status and alert start time are shown under the map.</div>
          </div>
        </div>
      </div>

      <div class="panel actions">
        <p>Save alert map settings.</p>
        <div class="chip-row">
          <button class="ghost" id="reloadAlerts" type="button">Reload Alerts</button>
          <button class="btn" id="saveAlerts" type="button">Save Alerts</button>
        </div>
      </div>
    </section>
""" % _home_options

ALERTS_JS = """
    // ---- OpenFW: air alerts ----
    state.alerts={src:'klymenko',token:'',interval:30,home:'-1'};
    const ALERT_SRC_LABELS={klymenko:'Klymenko',neptun:'NEPTUN',ubilling:'ubilling',alertsinua:'alerts.in.ua'};
    function syncAlertsForm(){
      byId('alertsSrc').value=state.alerts.src;
      byId('alertsToken').value=state.alerts.token;
      byId('alertsInterval').value=state.alerts.interval;
      byId('alertsHome').value=state.alerts.home;
      byId('alertsSrcLabel').textContent=ALERT_SRC_LABELS[state.alerts.src]||state.alerts.src;
      byId('alertsIntervalLabel').textContent=state.alerts.interval+'s';
    }
    function loadAlertsConfig(force){
      return once('alerts',force,async()=>{
        const a=await getJson('/.sys/alerts.json');
        state.alerts.src=String(a.src??'klymenko');
        state.alerts.token=String(a.token??'');
        state.alerts.interval=Math.max(15,toInt(a.i,30));
        state.alerts.home=String(a.home??'-1');
        syncAlertsForm();
      });
    }
    async function saveAlerts(){
      state.alerts.src=byId('alertsSrc').value;
      state.alerts.token=byId('alertsToken').value.trim();
      state.alerts.interval=Math.max(15,toInt(byId('alertsInterval').value,30));
      state.alerts.home=byId('alertsHome').value;
      if(state.alerts.src==='alertsinua'&&!state.alerts.token){toast('alerts.in.ua needs a token.',0);return}
      setBusy(true);
      try{
        await send('/set?al_src='+encodeURIComponent(state.alerts.src)+'&al_token='+encodeURIComponent(state.alerts.token)+'&al_i='+state.alerts.interval+'&al_home='+encodeURIComponent(state.alerts.home));
        syncAlertsForm();
        toast('Alert settings saved.',1);
      }catch(err){toast('Alerts save failed: '+err.message,0)}
      setBusy(false);
    }
    setTimeout(()=>{
      byId('saveAlerts').addEventListener('click',saveAlerts);
      byId('reloadAlerts').addEventListener('click',()=>reloadView('alerts',false,true));
    },0);
"""

PATCHES = [
    # tab button
    (r'(<button class="tab" data-view="monitor">Monitor</button>)',
     r'\1\n        <button class="tab" data-view="alerts">Alerts</button>'),
    # view section, before the settings view
    (r'(\n\s*<section class="view hidden" id="view-settings">)', ALERTS_VIEW.replace('\\', r'\\') + r'\1'),
    # app list: toggle, boot app, open button
    (r"(\{key:'settings',label:'Settings',name:'Settings',index:'8'\})(\s*\])",
     r"\1,\n      {key:'alerts',label:'Air alerts',name:'Alerts',index:'9'}\2"),
    (r"const APP_VIEWS=\['btc','cg','stock','weather','time','image','monitor'\];",
     "const APP_VIEWS=['btc','cg','stock','weather','time','image','monitor','alerts'];"),
    (r"(const VIEW_LABELS=\{[^}]*monitor:'Monitor',)", r"\1alerts:'Alerts',"),
    (r"(\n(\s*)if\(view==='monitor'\) return loadMonitorConfig\(force\);)",
     r"\1\n\2if(view==='alerts') return loadAlertsConfig(force);"),
    # JS helpers, before loadView
    (r"(\n\s*function loadView\(view,force\)\{)", ALERTS_JS.replace('\\', r'\\') + r"\1"),
    # version chip shows OpenFW
    (r"<title>GeekMagic", "<title>OpenFW · GeekMagic"),
]


def apply(html):
    for pattern, repl in PATCHES:
        html, n = re.subn(pattern, repl, html, count=1)
        if n != 1:
            raise RuntimeError("ui_patch: anchor not found: " + pattern[:60])
    return html
