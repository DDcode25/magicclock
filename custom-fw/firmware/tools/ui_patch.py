"""Magic Box FW additions to the stock web UI (web/stock_index.html).

Each patch replaces one exact anchor in the stock page; a missing anchor fails the build
instead of silently shipping a broken UI.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
from gen_ukraine_map import LABELS as CITIES  # noqa: E402  screen labels: oblast centres

REGIONS_UK = [
    "АР Крим", "Волинська область", "Вінницька область", "Дніпропетровська область",
    "Донецька область", "Житомирська область", "Закарпатська область", "Запорізька область",
    "Івано-Франківська область", "м. Київ", "Київська область", "Кіровоградська область",
    "Луганська область", "Львівська область", "Миколаївська область", "Одеська область",
    "Полтавська область", "Рівненська область", "Севастополь", "Сумська область",
    "Тернопільська область", "Харківська область", "Херсонська область", "Хмельницька область",
    "Черкаська область", "Чернівецька область", "Чернігівська область",
]

_home_options = '<option value="-1">— немає —</option>' + "".join(
    '<option value="%d">%s</option>' % (i, n if n.endswith(CITIES[i]) else "%s — %s" % (n, CITIES[i]))
    for i, n in sorted(enumerate(REGIONS_UK), key=lambda x: x[1])
)

ALERTS_VIEW = """
    <section class="view hidden" id="view-alerts">
      <div class="panel hero">
        <div>
          <div class="eyebrow">Повітряні тривоги</div>
          <h1>Мапа тривог України</h1>
          <p class="lead">Області на мапі: червона — тривога в усій області (або в усіх її районах), помаранчева — в частині районів, сіра — джерело не дало даних.</p>
        </div>
        <div class="hero-side">
          <div class="chip-row">
            <div class="chip"><span>Джерело</span><b id="alertsSrcLabel">-</b></div>
            <div class="chip"><span>Оновлення</span><b id="alertsIntervalLabel">30 с</b></div>
          </div>
          <div class="chip-row">
            <button class="btn hero-open" type="button" data-open-app="Alerts" data-app-key="alerts" data-app-label="Повітряні тривоги">Відкрити на пристрої</button>
          </div>
        </div>
      </div>

      <div class="grid2">
        <div class="panel">
          <div class="section-head">
            <div>
              <div class="eyebrow">Дані</div>
              <h2>Джерело</h2>
            </div>
          </div>
          <div class="stack">
            <label class="pair"><span>Джерело</span><select class="select" id="alertsSrc">
              <option value="klymenko">vadimklimenko.com (без токена, області й райони)</option>
              <option value="neptun">neptun.in.ua (без токена, області й райони)</option>
              <option value="ubilling">ubilling.net.ua (без токена, лише області, без Криму; найменш точне)</option>
              <option value="alertsinua">alerts.in.ua (потрібен токен)</option>
            </select></label>
            <label class="pair"><span>Токен alerts.in.ua</span><input class="field" id="alertsToken" placeholder="лише для alerts.in.ua" autocomplete="off"></label>
            <label class="pair"><span>Інтервал оновлення (с, мінімум 15)</span><input class="field" id="alertsInterval" inputmode="numeric" placeholder="30"></label>
            <div class="hint">Токен: напишіть на <b>api@alerts.in.ua</b>. Ліміт — близько 10 запитів на хвилину.</div>
            <div class="hint">Дані: <a href="https://neptun.in.ua/" target="_blank" rel="noopener noreferrer">Карта повітряних тривог — NEPTUN</a>, vadimklimenko.com, ubilling.net.ua, alerts.in.ua.
              Це агрегатори, а не офіційна система оповіщення: завжди орієнтуйтеся на офіційні сигнали тривоги.</div>
          </div>
        </div>

        <div class="panel">
          <div class="section-head">
            <div>
              <div class="eyebrow">Екран</div>
              <h2>Мій регіон</h2>
            </div>
          </div>
          <div class="stack">
            <label class="pair"><span>Виділяти й показувати статус для</span><select class="select" id="alertsHome">%s</select></label>
            <label class="pair"><span>Колір годинника під мапою</span><input class="field" type="color" id="alertsClockColor" value="#ffffff"></label>
            <div class="hint">Регіон обведено білим на мапі.</div>
            <div class="hint">Під мапою замість назви області показано її обласний центр (для Київської — «Київщина»):
              екран малий, і лише коротка назва вміщується в один рядок із годинником. Колір назви — статус регіону:
              зелений — тривоги немає, помаранчевий — тривога в частині районів, червоний — в усій області.
              Розмір шрифту підібрано для кожного міста окремо — найбільший, за якого місто й годинник уміщуються в рядок.</div>
            <div class="hint">Коли в регіоні починається тривога, пристрій відкриває мапу й 10 секунд блимає кольором тривоги.</div>
          </div>
        </div>
      </div>

      <div class="panel actions">
        <p>Збереження налаштувань мапи тривог.</p>
        <div class="chip-row">
          <button class="ghost" id="testAlertRed" type="button">Перевірити сигнал</button>
          <button class="ghost" id="testAlertOrange" type="button">Перевірити часткову</button>
          <button class="ghost" id="reloadAlerts" type="button">Оновити</button>
          <button class="btn" id="saveAlerts" type="button">Зберегти</button>
        </div>
      </div>
    </section>
""" % _home_options

ALERTS_JS = """
    // ---- Magic Box FW: air alerts ----
    state.alerts={src:'klymenko',token:'',interval:30,home:'-1',cc:'#ffffff'};
    const ALERT_SRC_LABELS={klymenko:'Klymenko',neptun:'NEPTUN',ubilling:'ubilling',alertsinua:'alerts.in.ua'};
    function syncAlertsForm(){
      byId('alertsSrc').value=state.alerts.src;
      byId('alertsToken').value=state.alerts.token;
      byId('alertsInterval').value=state.alerts.interval;
      byId('alertsHome').value=state.alerts.home;
      byId('alertsClockColor').value=state.alerts.cc;
      byId('alertsSrcLabel').textContent=ALERT_SRC_LABELS[state.alerts.src]||state.alerts.src;
      byId('alertsIntervalLabel').textContent=state.alerts.interval+' с';
    }
    function loadAlertsConfig(force){
      return once('alerts',force,async()=>{
        const a=await getJson('/.sys/alerts.json');
        state.alerts.src=String(a.src??'klymenko');
        state.alerts.token=String(a.token??'');
        state.alerts.interval=Math.max(15,toInt(a.i,30));
        state.alerts.home=String(a.home??'-1');
        state.alerts.cc=/^#[0-9a-f]{6}$/i.test(String(a.cc??''))?String(a.cc).toLowerCase():'#ffffff';
        syncAlertsForm();
      });
    }
    async function saveAlerts(){
      state.alerts.src=byId('alertsSrc').value;
      state.alerts.token=byId('alertsToken').value.trim();
      state.alerts.interval=Math.max(15,toInt(byId('alertsInterval').value,30));
      state.alerts.home=byId('alertsHome').value;
      state.alerts.cc=byId('alertsClockColor').value;
      if(state.alerts.src==='alertsinua'&&!state.alerts.token){toast('Для alerts.in.ua потрібен токен.',0);return}
      setBusy(true);
      try{
        await send('/set?al_src='+encodeURIComponent(state.alerts.src)+'&al_token='+encodeURIComponent(state.alerts.token)+'&al_i='+state.alerts.interval+'&al_home='+encodeURIComponent(state.alerts.home)+'&al_cc='+encodeURIComponent(state.alerts.cc));
        syncAlertsForm();
        toast('Налаштування тривог збережено.',1);
      }catch(err){toast('Не вдалося зберегти тривоги: '+err.message,0)}
      setBusy(false);
    }
    setTimeout(()=>{
      byId('saveAlerts').addEventListener('click',saveAlerts);
      byId('reloadAlerts').addEventListener('click',()=>reloadView('alerts',false,true));
      byId('testAlertRed').addEventListener('click',()=>send('/alerts/test?level=2').then(()=>toast('Сигнал надіслано.',1)).catch(e=>toast(e.message,0)));
      byId('testAlertOrange').addEventListener('click',()=>send('/alerts/test?level=1').then(()=>toast('Сигнал надіслано.',1)).catch(e=>toast(e.message,0)));
    },0);
"""

PATCHES = [
    # tab button
    (r'(<button class="tab" data-view="monitor">Monitor</button>)',
     r'\1\n        <button class="tab" data-view="alerts">Тривоги</button>'),
    # view section, before the settings view
    (r'(\n\s*<section class="view hidden" id="view-settings">)', ALERTS_VIEW.replace('\\', r'\\') + r'\1'),
    # app list: toggle, boot app, open button
    (r"(\{key:'settings',label:'Settings',name:'Settings',index:'8'\})(\s*\])",
     r"\1,\n      {key:'alerts',label:'Повітряні тривоги',name:'Alerts',index:'9'}\2"),
    (r"const APP_VIEWS=\['btc','cg','stock','weather','time','image','monitor'\];",
     "const APP_VIEWS=['btc','cg','stock','weather','time','image','monitor','alerts'];"),
    (r"(const VIEW_LABELS=\{[^}]*monitor:'Monitor',)", r"\1alerts:'Alerts',"),
    (r"(\n(\s*)if\(view==='monitor'\) return loadMonitorConfig\(force\);)",
     r"\1\n\2if(view==='alerts') return loadAlertsConfig(force);"),
    # JS helpers, before loadView
    (r"(\n\s*function loadView\(view,force\)\{)", ALERTS_JS.replace('\\', r'\\') + r"\1"),
    # firmware name in the tab title and the page header
    (r"<title>GeekMagic PRO</title>", "<title>Magic Box FW</title>"),
]


def apply(html):
    for pattern, repl in PATCHES:
        html, n = re.subn(pattern, repl, html, count=1)
        if n != 1:
            raise RuntimeError("ui_patch: anchor not found: " + pattern[:60])
    return html
