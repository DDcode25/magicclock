#pragma once
#include <TFT_eSPI.h>

// "Picture" app: JPG/GIF files from /image (stock: up to 240x240, centred).
// Settings in /.sys/album.json (stock format):
//   {"autoplay":0|1, "i_i":<JPG seconds>, "gif_loop":<GIF plays before next>, "path":"/image/x.gif"}
// autoplay=1: slideshow through all files; otherwise the selected `path` (or the first file) stays.
namespace pictures {

void reload();             // album.json or the file list changed
void show(TFT_eSPI &tft);  // app opened: draw the current picture
void loop(TFT_eSPI &tft);  // GIF frames and slideshow timing
void hide();               // app left: free the GIF decoder (~20 KB)
void next(TFT_eSPI &tft);  // long press: next file
String statsJson();        // playback timing and heap, for /pic.json
String checkJson(const String &path, int frames);  // decode without drawing: per-frame CRC (diagnostics)

}  // namespace pictures
