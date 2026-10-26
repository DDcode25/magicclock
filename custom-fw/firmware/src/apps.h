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
    {"alerts", "Alerts", "Повітряні тривоги"},  // Magic Box FW addition, index 9
};
static const int APP_COUNT = sizeof(APP_ITEMS) / sizeof(APP_ITEMS[0]);
static const int APP_ALERTS = 9;
