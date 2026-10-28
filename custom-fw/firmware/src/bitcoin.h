#pragma once
#include <TFT_eSPI.h>

// "Bitcoin" app: Binance spot prices, as in the stock firmware (same settings and web tab).
// Data: data-api.binance.vision (Binance public market data, no key): /api/v3/ticker/24hr for the
// prices and 24 h change of all pairs in one request, /api/v3/klines (30 candles) for the pairs on
// screen (chart, and the change against the previous closed candle).
// Settings (stock files):
//   /.sys/coin2.json  cin0..cin11 pair ("BTCUSDT"), cn0..cn11 optional name, c_i refresh s,
//                     c_l rotate pages, c_loading "loading" text, c_clock clock, c_pct_mode
//                     0 = 24 h change, 1 = against the previous closed candle; c_ani (ignored)
//   /.sys/cr.json rot_i rotation s; /.sys/k.json k candle interval ("1m".."1M");
//   /.sys/c_theme.json c_theme 0 one pair + chart, 1 one pair large, 2 two pairs, 3 three pairs;
//   /.sys/c_chart.json c_chart 0 candles, 1 line;
//   /.sys/ccolor.json up/down price, coin name and percent colours; /.sys/sep.json separators.
namespace bitcoin {

void begin();   // start the background fetch task
void reload();  // settings changed (/set)
void pause(bool on);  // stop fetching during OTA

void draw(TFT_eSPI &tft);  // full redraw
void tick(TFT_eSPI &tft);  // every second: new data, page rotation, clock
void next(TFT_eSPI &tft);  // long press: next page
void hide();               // app left: free the drawing buffer

String statusJson();  // for /btc.json

}  // namespace bitcoin
