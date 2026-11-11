#include "apps.h"

// Same keys, names and indices as APP_ITEMS in the stock web UI (app.json "theme" is the index).
static const char K0[] PROGMEM = "bitcoin", N0[] PROGMEM = "BitCoin", L0[] PROGMEM = "Біткоїн";
static const char K1[] PROGMEM = "coingecko", N1[] PROGMEM = "CoinGecko", L1[] PROGMEM = "CoinGecko";
static const char K2[] PROGMEM = "stock", N2[] PROGMEM = "Stock", L2[] PROGMEM = "Акції";
static const char K3[] PROGMEM = "weather", N3[] PROGMEM = "Weather", L3[] PROGMEM = "Погода";
static const char K4[] PROGMEM = "picture", N4[] PROGMEM = "Picture", L4[] PROGMEM = "Картинки";
static const char K5[] PROGMEM = "monitor", N5[] PROGMEM = "Monitor", L5[] PROGMEM = "Монітор ПК";
static const char K6[] PROGMEM = "clock", N6[] PROGMEM = "Clock", L6[] PROGMEM = "Годинник";
static const char K7[] PROGMEM = "idea", N7[] PROGMEM = "Idea", L7[] PROGMEM = "Ідеї";
static const char K8[] PROGMEM = "settings", N8[] PROGMEM = "Settings", L8[] PROGMEM = "Налаштування";
static const char K9[] PROGMEM = "alerts", N9[] PROGMEM = "Alerts", L9[] PROGMEM = "Повітряні тривоги";
static const char K10[] PROGMEM = "outage", N10[] PROGMEM = "Outage", L10[] PROGMEM = "Світло";

const AppItem APP_ITEMS[APP_COUNT] PROGMEM = {
    {K0, N0, L0},
    {K1, N1, L1},
    {K2, N2, L2},
    {K3, N3, L3},
    {K4, N4, L4},
    {K5, N5, L5},
    {K6, N6, L6},
    {K7, N7, L7},
    {K8, N8, L8},
    {K9, N9, L9},
    {K10, N10, L10},
};
