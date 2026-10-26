#pragma once

// Same keys, names and indices as APP_ITEMS in the stock web UI (app.json "theme" is the index).
struct AppItem {
  const char *key;   // app.json "apps" flag, /set?app_<key>=
  const char *name;  // /set?open_app=<name>
  const char *label;
};

static const AppItem APP_ITEMS[] = {
    {"bitcoin", "BitCoin", "Bitcoin"},  {"coingecko", "CoinGecko", "CoinGecko"}, {"stock", "Stock", "Stock"},
    {"weather", "Weather", "Weather"},  {"picture", "Picture", "Picture"},       {"monitor", "Monitor", "Monitor"},
    {"clock", "Clock", "Clock"},        {"idea", "Idea", "Idea"},                {"settings", "Settings", "Settings"},
    {"alerts", "Alerts", "Air alerts"},  // OpenFW addition, index 9
};
static const int APP_COUNT = sizeof(APP_ITEMS) / sizeof(APP_ITEMS[0]);
static const int APP_ALERTS = 9;
