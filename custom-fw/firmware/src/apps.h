#pragma once

// Same keys, names and indices as APP_ITEMS in the stock web UI (app.json "theme" is the index).
struct AppItem {
  const char *key;   // app.json "apps" flag, /set?app_<key>=
  const char *name;  // /set?open_app=<name>
  const char *label;  // shown on the device screen (Ukrainian)
};

static const AppItem APP_ITEMS[] = {
    {"bitcoin", "BitCoin", "Біткоїн"},    {"coingecko", "CoinGecko", "CoinGecko"}, {"stock", "Stock", "Акції"},
    {"weather", "Weather", "Погода"},     {"picture", "Picture", "Картинки"},      {"monitor", "Monitor", "Монітор ПК"},
    {"clock", "Clock", "Годинник"},       {"idea", "Idea", "Ідеї"},                {"settings", "Settings", "Налаштування"},
    {"alerts", "Alerts", "Повітряні тривоги"},  // UA Smart Box addition, index 9
    {"outage", "Outage", "Світло"},             // UA Smart Box addition, index 10
};
static const int APP_COUNT = sizeof(APP_ITEMS) / sizeof(APP_ITEMS[0]);
// Order of the screens (tap = next): indices into APP_ITEMS. Same order in the web UI (tools/ui_patch.py).
static const int APP_ORDER[APP_COUNT] = {3 /*weather*/, 6 /*clock*/, 9 /*alerts*/, 10 /*outage*/, 0 /*bitcoin*/,
                                         2 /*stock*/, 1 /*coingecko*/, 4 /*picture*/, 5 /*monitor*/, 7 /*idea*/,
                                         8 /*settings*/};
static const int APP_BITCOIN = 0;
static const int APP_WEATHER = 3;
static const int APP_PICTURE = 4;
static const int APP_CLOCK = 6;
static const int APP_SETTINGS = 8;
static const int APP_ALERTS = 9;
static const int APP_OUTAGE = 10;
