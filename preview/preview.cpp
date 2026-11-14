// PC preview: renders the menu with the real M5GFX drawing code and writes PPM images.
#include <M5GFX.h>
#include <stdio.h>
#include "../src/menu_draw.h"

static void save(M5Canvas& c, const char* path) {
  FILE* f = fopen(path, "wb");
  fprintf(f, "P6\n240 135\n255\n");
  for (int y = 0; y < 135; y++) for (int x = 0; x < 240; x++) {
    uint16_t p = c.readPixel(x, y);
    uint8_t rgb[3] = {(uint8_t)(((p >> 11) & 31) * 255 / 31), (uint8_t)(((p >> 5) & 63) * 255 / 63), (uint8_t)((p & 31) * 255 / 31)};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}

int main(int argc, char** argv) {
  M5Canvas cv;
  cv.setColorDepth(16);
  cv.createSprite(240, 135);
  cv.setFont(&fonts::Font0);
  int tops[] = {0, 3, 6, 9, 12, 13};          // six pages cover all 16 items
  int sels[] = {0, 4, 7, 10, 12, 15};
  for (int i = 0; i < 6; i++) {
    cv.fillSprite(0);
    cv.fillRect(0, 0, 240, 11, 0x18E3);
    cv.setTextSize(1); cv.setTextColor(0x05FF); cv.setCursor(2, 2); cv.print("SensorLab");
    cv.setTextColor(0xFFFF); cv.setCursor(196, 2); cv.print("SD 73%");
    drawMenuBody(cv, sels[i], tops[i]);
    cv.fillRect(0, 125, 240, 10, 0x18E3);
    cv.setTextColor(0x7BEF); cv.setCursor(2, 126); cv.print(";/. move   ,// page   ENTER open");
    char path[64]; snprintf(path, sizeof(path), "%s/menu%d.ppm", argc > 1 ? argv[1] : ".", i);
    save(cv, path);
  }
  return 0;
}
