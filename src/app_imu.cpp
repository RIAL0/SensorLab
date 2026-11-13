// IMU (BMI270 on the ADV): 3D attitude cube, XYZ bars, rolling graphs, CSV logging.
#include "apps.h"
#include "ui.h"
#include <math.h>

static void drawBar(int x, int y, int w, float v, float range, uint16_t col) {
  int mid = x + w / 2;
  cv.drawRect(x, y, w, 7, C_SEL);
  int len = (int)(v / range * (w / 2));
  if (len > w / 2) len = w / 2;
  if (len < -w / 2) len = -w / 2;
  if (len >= 0) cv.fillRect(mid, y + 1, len, 5, col); else cv.fillRect(mid + len, y + 1, -len, 5, col);
  cv.drawFastVLine(mid, y, 7, C_DIM);
}

void appImu() {
  if (!M5.Imu.isEnabled()) {
    ui::clear(); ui::header("IMU");
    cv.setTextColor(C_BAD, C_BG); cv.setCursor(4, 40); cv.print("No IMU detected on the internal I2C bus.");
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, 54); cv.print("The original Cardputer has none;");
    cv.setCursor(4, 64); cv.print("the ADV has a BMI270 at 0x68/0x69.");
    ui::footer("` back"); ui::push();
    while (!ui::poll().back) delay(20);
    return;
  }
  static const char* names[] = {"none", "unknown", "SH200Q", "MPU6050", "MPU6886", "MPU9250", "BMI270"};
  int t = (int)M5.Imu.getType();
  const char* chip = (t >= 0 && t < 7) ? names[t] : "IMU";

  const int HIST = 236;
  static float hist[6][HIST];
  memset(hist, 0, sizeof(hist));
  int hpos = 0, view = 0;
  float ax = 0, ay = 0, az = 1, gx = 0, gy = 0, gz = 0, temp = NAN;
  float roll = 0, pitch = 0, yaw = 0;
  float gox = 0, goy = 0, goz = 0;        // gyro offsets
  float maxG = 0;
  uint32_t lastUs = micros(), lastDraw = 0, samples = 0, rateAt = millis(), rate = 0, rateCnt = 0;
  bool logging = false;
  File logf;
  String logName;

  while (true) {
    ui::Key k = ui::poll();
    if (k.back) break;
    if (k.right || k.lc == 'v') view = (view + 1) % 3;
    if (k.left) view = (view + 2) % 3;
    if (k.ch == '0') { yaw = 0; maxG = 0; }
    if (k.lc == 'c') {            // gyro bias calibration: keep the device still
      ui::toast("Keep still...", C_WARN, 300);
      double sx = 0, sy = 0, sz = 0; int n = 0;
      uint32_t until = millis() + 1500;
      while (millis() < until) {
        if (M5.Imu.update()) { float x, y, z; M5.Imu.getGyro(&x, &y, &z); sx += x; sy += y; sz += z; n++; }
        delay(2);
      }
      if (n) { gox = sx / n; goy = sy / n; goz = sz / n; }
      yaw = 0;
      ui::toast("Gyro zeroed");
    }
    if (k.lc == 's') {
      if (logging) {
        logging = false; logf.close();
        ui::toast("Saved " + logName.substring(11));
      } else if (!ui::sdMount()) {
        ui::toast("No SD card", C_BAD);
      } else {
        logName = ui::nextFile("imu", "csv");
        logf = SD.open(logName, FILE_WRITE);
        if (logf) { logf.println("ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,roll,pitch,yaw"); logging = true; }
        else ui::toast("SD write failed", C_BAD);
      }
    }

    if (M5.Imu.update()) {
      M5.Imu.getAccel(&ax, &ay, &az);
      M5.Imu.getGyro(&gx, &gy, &gz);
      gx -= gox; gy -= goy; gz -= goz;
      uint32_t now = micros();
      float dt = (now - lastUs) * 1e-6f;
      lastUs = now;
      if (dt > 0.1f) dt = 0.1f;
      // complementary filter: gyro for fast motion, accelerometer for the gravity reference
      float accRoll = atan2f(ay, az) * 57.2958f;
      float accPitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * 57.2958f;
      roll = 0.96f * (roll + gx * dt) + 0.04f * accRoll;
      pitch = 0.96f * (pitch + gy * dt) + 0.04f * accPitch;
      yaw += gz * dt;
      if (yaw > 180) yaw -= 360;
      if (yaw < -180) yaw += 360;
      float g = sqrtf(ax * ax + ay * ay + az * az);
      if (g > maxG) maxG = g;
      samples++; rateCnt++;
      if ((samples & 3) == 0) {
        float v[6] = {ax, ay, az, gx, gy, gz};
        for (int i = 0; i < 6; i++) hist[i][hpos] = v[i];
        hpos = (hpos + 1) % HIST;
      }
      if (logging) {
        logf.printf("%lu,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f,%.1f,%.1f,%.1f\n", (unsigned long)millis(), ax, ay, az, gx, gy, gz, roll, pitch, yaw);
        if ((samples & 63) == 0) logf.flush();
      }
    }
    if (millis() - rateAt >= 1000) { rate = rateCnt; rateCnt = 0; rateAt = millis(); M5.Imu.getTemp(&temp); }
    if (millis() - lastDraw < 33) { delay(1); continue; }
    lastDraw = millis();

    ui::clear();
    char right[24];
    snprintf(right, sizeof(right), "%s%s %luHz", logging ? "LOG " : "", chip, (unsigned long)rate);
    ui::header(view == 0 ? "IMU Attitude" : view == 1 ? "IMU Bars" : "IMU Graph", right);
    static const uint16_t cols[3] = {C_BAD, C_OK, C_ACC};
    static const char* axn[3] = {"X", "Y", "Z"};
    float a[3] = {ax, ay, az}, gyr[3] = {gx, gy, gz};

    if (view == 0) {
      // wireframe box rotated by roll/pitch/yaw
      float cr = cosf(roll / 57.2958f), sr = sinf(roll / 57.2958f);
      float cp = cosf(pitch / 57.2958f), sp = sinf(pitch / 57.2958f);
      float cyw = cosf(yaw / 57.2958f), syw = sinf(yaw / 57.2958f);
      static const float V[8][3] = {{-1.6f, -1, -0.25f}, {1.6f, -1, -0.25f}, {1.6f, 1, -0.25f}, {-1.6f, 1, -0.25f},
                                    {-1.6f, -1, 0.25f},  {1.6f, -1, 0.25f},  {1.6f, 1, 0.25f},  {-1.6f, 1, 0.25f}};
      static const uint8_t E[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
      int px[8], py[8];
      const int cx = 62, cy = 68;
      for (int i = 0; i < 8; i++) {
        float x = V[i][0], y = V[i][1], z = V[i][2];
        float y1 = y * cr - z * sr, z1 = y * sr + z * cr;            // roll about X
        float x2 = x * cp + z1 * sp, z2 = -x * sp + z1 * cp;         // pitch about Y
        float x3 = x2 * cyw - y1 * syw, y3 = x2 * syw + y1 * cyw;    // yaw about Z
        float persp = 5.0f / (5.0f - z2);
        px[i] = cx + (int)(x3 * 24 * persp);
        py[i] = cy - (int)(y3 * 24 * persp * 0.9f) + (int)(z2 * 6);
      }
      for (int e = 0; e < 12; e++) cv.drawLine(px[E[e][0]], py[E[e][0]], px[E[e][1]], py[E[e][1]], e < 4 ? C_DIM : e < 8 ? C_ACC : C_FG);
      // bubble level
      const int bx = 62, by = 113, br = 9;
      cv.drawCircle(bx, by, br, C_DIM);
      int ox = constrain((int)(roll / 5), -br, br), oy = constrain((int)(pitch / 5), -br, br);
      cv.fillCircle(bx + ox, by + oy, 2, fabsf(roll) < 2 && fabsf(pitch) < 2 ? C_OK : C_WARN);
      int x0 = 128;
      cv.setTextColor(C_YEL, C_BG);
      cv.setCursor(x0, TOP + 1); cv.printf("roll  %7.1f", roll);
      cv.setCursor(x0, TOP + 10); cv.printf("pitch %7.1f", pitch);
      cv.setCursor(x0, TOP + 19); cv.printf("yaw   %7.1f", yaw);
      for (int i = 0; i < 3; i++) {
        cv.setTextColor(cols[i], C_BG);
        cv.setCursor(x0, TOP + 32 + i * 9); cv.printf("a%s %+6.3f g", axn[i], a[i]);
        cv.setCursor(x0, TOP + 62 + i * 9); cv.printf("g%s %+7.1f", axn[i], gyr[i]);
      }
      cv.setTextColor(C_DIM, C_BG);
      cv.setCursor(x0, TOP + 92);
      cv.printf("|a|%.2fg max%.2f", sqrtf(ax * ax + ay * ay + az * az), maxG);
      cv.setCursor(x0, TOP + 101);
      if (!isnan(temp)) cv.printf("chip temp %.1fC", temp);
    } else if (view == 1) {
      cv.setTextColor(C_DIM, C_BG);
      cv.setCursor(2, TOP + 1); cv.print("Accelerometer  (+/-2 g)");
      for (int i = 0; i < 3; i++) {
        int y = TOP + 12 + i * 11;
        cv.setTextColor(cols[i], C_BG); cv.setCursor(2, y); cv.print(axn[i]);
        drawBar(14, y, 150, a[i], 2.0f, cols[i]);
        cv.setCursor(170, y); cv.printf("%+6.3f g", a[i]);
      }
      cv.setTextColor(C_DIM, C_BG);
      cv.setCursor(2, TOP + 50); cv.print("Gyroscope  (+/-500 deg/s)");
      for (int i = 0; i < 3; i++) {
        int y = TOP + 61 + i * 11;
        cv.setTextColor(cols[i], C_BG); cv.setCursor(2, y); cv.print(axn[i]);
        drawBar(14, y, 150, gyr[i], 500.0f, cols[i]);
        cv.setCursor(170, y); cv.printf("%+7.1f", gyr[i]);
      }
      cv.setTextColor(C_DIM, C_BG);
      cv.setCursor(2, TOP + 98);
      cv.printf("roll %.0f pitch %.0f yaw %.0f", roll, pitch, yaw);
      if (!isnan(temp)) cv.printf("  %.1fC", temp);
    } else {
      // two stacked rolling plots
      for (int p = 0; p < 2; p++) {
        int y0 = TOP + 1 + p * 55, h = 52, mid = y0 + h / 2;
        float range = p == 0 ? 2.0f : 500.0f;
        cv.drawRect(0, y0, SCR_W, h, C_SEL);
        cv.drawFastHLine(1, mid, SCR_W - 2, C_SEL);
        for (int i = 0; i < 3; i++) {
          int prev = mid;
          for (int x = 0; x < HIST; x++) {
            float v = hist[p * 3 + i][(hpos + x) % HIST];
            int y = mid - (int)(v / range * (h / 2 - 1));
            y = constrain(y, y0 + 1, y0 + h - 2);
            if (x) cv.drawLine(x + 1, prev, x + 2, y, cols[i]);
            prev = y;
          }
        }
        cv.setTextColor(C_DIM);
        cv.setCursor(3, y0 + 2);
        cv.print(p == 0 ? "accel +/-2g" : "gyro +/-500dps");
      }
    }
    ui::footer(logging ? "S stop log  ,// view  0 zero  C calib" : "S log CSV  ,// view  0 zero  C calib");
    ui::push();
  }
  if (logging) logf.close();
}
