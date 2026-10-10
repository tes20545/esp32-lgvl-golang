/* ============================================================================
   PC Live Dashboard — "Linear / Modern" design language
   Board: ESP32-2432S028R (Cheap Yellow Display, 320x240, XPT2046 touch)

   Mapping of the web design system onto TFT_eSPI primitives:
     - background-deep/base/elevated  -> layered near-black fills
     - accent (#5E6AD2)               -> indigo tokens + glow circles
     - 6-10% white hairlines          -> dim RGB565 borders on rounded cards
     - multi-layer shadow             -> card border + top highlight + ambient glow
     - bento grid                     -> asymmetric rounded-rect cards
     - "slideshow" navigation         -> 3 slides (PERF/GAMING/MEDIA),
                                         auto-loop + touch swipe/tap.
                                         Game mode (detectGame): red theme +
                                         locked to 2 slides (PERF/GAMING).

   NOTE (TFT_eSPI User_Setup.h must target the CYD):
     ILI9341_2_DRIVER, TFT 240x320, MISO12 MOSI13 SCLK14 CS15 DC2 RST -1,
     BL21. Touch (XPT2046): TOUCH_CS 33, IRQ 36. Without this, getTouch() fails.
   ============================================================================ */

#include <TFT_eSPI.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <math.h>

TFT_eSPI tft = TFT_eSPI();

// จอ CYD วาง XPT2046 ไว้คนละ SPI bus กับจอ จึงใช้ไลบรารีแยก
#define XPT2046_IRQ  36   // T_IRQ
#define XPT2046_MOSI 32   // T_DIN
#define XPT2046_MISO 39   // T_OUT
#define XPT2046_CLK  25   // T_CLK
#define XPT2046_CS   33   // T_CS

SPIClass touchSPI = SPIClass(HSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

/* ---------------------------------------------------------------------------
   Design tokens (RGB565)
   --------------------------------------------------------------------------- */
#define RGB565(r, g, b) ((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3)))

#define COL_BG          RGB565(5,   5,   6)     // #050506  page canvas
#define COL_SURFACE     RGB565(16,  16,  19)    // rgba(255,255,255,.05) over bg
#define COL_SURFACE_HI  RGB565(26,  26,  31)    // top inner highlight
#define COL_HAIRLINE    RGB565(32,  32,  38)    // ~6% white border
#define COL_HAIR_SOFT   RGB565(20,  20,  24)    // softer divider
#define COL_FG          RGB565(237, 237, 239)   // #EDEDEF  primary text
#define COL_FG_MUTED    RGB565(138, 143, 152)   // #8A8F98  secondary text
#define COL_ACCENT      RGB565(94,  106, 210)   // #5E6AD2  interactive
#define COL_ACCENT_HI   RGB565(104, 114, 217)   // #6872D9  accent hover
#define COL_ACCENT_DIM  RGB565(38,  43,  84)    // accent washed into bg
#define COL_ACCENT_GLOW RGB565(24,  28,  56)    // ambient light pool

// สีตามระดับการใช้งาน (usage level outline glow)
#define COL_GREEN       RGB565(52,  199, 89)    // #34C759   1-25%
#define COL_YELLOW      RGB565(255, 204, 0)     // #FFCC00  25-50%
#define COL_ORANGE      RGB565(255, 149, 0)     // #FF9500  50-75%
#define COL_RED         RGB565(255, 59,  48)    // #FF3B30  75%+
#define COL_RED_GLOW    RGB565(64,  16,  14)    // red light pool (game mode)

/* ---------------------------------------------------------------------------
   Layout
   --------------------------------------------------------------------------- */
#define SCR_W     320
#define SCR_H     240
#define HEADER_H  30
#define FOOTER_H  28
#define CARD_R    8

enum Slide { SLIDE_PERF, SLIDE_GAME, SLIDE_MEDIA, SLIDE_COUNT };
const char* slideNames[SLIDE_COUNT] = { "PERFORMANCE", "GAMING", "MEDIA" };

/* ---------------------------------------------------------------------------
   State
   --------------------------------------------------------------------------- */
String cpuVal    = "0";
String ramVal    = "0";
String gpuVal    = "--";
String cpuTemp   = "--";
String gpuTemp   = "--";
String diskVal   = "--";
String gameName  = "None";
String gameTime  = "00:00";
String songTitle = "Idle";

int  currentSlide = SLIDE_PERF;
bool autoSlide    = true;
bool inGame       = false;     // true = โหมดเกม (เหลือ 2 slide: PERF + GAMING)
const uint32_t AUTO_ADVANCE_MS = 8000;

// จำนวน slide ที่ใช้ได้จริง — โหมดเกมตัด MEDIA ออก
int slideCount() { return inGame ? 2 : SLIDE_COUNT; }

String   rxBuf       = "";
uint32_t lastPacketMs = 0;
uint32_t lastInputMs  = 0;
uint32_t lastSlideMs  = 0;
uint32_t lastAnimMs   = 0;
bool     signalOk     = false;

// แคชค่าที่เพิ่งวาด เพื่อกันไม่ให้วาดซ้ำซ้อน (ลดอาการตัวอักษรกระพริบ)
String   lastGameName, lastGameTime, lastSongTitle;
String   lastPerfGpu, lastPerfCpu, lastPerfRam;   // signature การ์ดแต่ละใบ
int      lastInSession = -1;   // -1 = ยังไม่รู้สถานะ ใช้บังคับให้วาดครั้งแรก

// Touch state
bool     touching = false;
int16_t  startX = 0, startY = 0, curX = 0, curY = 0;

// ช่วงค่า raw ของ XPT2046 (rotation 1) — ปรับตัวเลขนี้ถ้าแตะแล้วตำแหน่งเพี้ยน
const int TS_MINX = 200,  TS_MAXX = 3700;
const int TS_MINY = 240,  TS_MAXY = 3800;

/* ===========================================================================
   Color helpers
   =========================================================================== */
uint16_t blend(uint16_t a, uint16_t b, uint8_t t) {
  int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int r = ar + ((br - ar) * t) / 255;
  int g = ag + ((bg - ag) * t) / 255;
  int bl = ab + ((bb - ab) * t) / 255;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

// Soft radial "light pool" built from concentric fills (fakes a blur).
void glowCircle(int cx, int cy, int r, uint16_t col, int layers) {
  for (int i = 0; i < layers; i++) {
    int rr = r * (layers - i) / layers;
    uint16_t c = blend(COL_BG, col, (uint8_t)(255 * i / layers));
    tft.fillCircle(cx, cy, rr, c);
  }
}

/* ===========================================================================
   Primitives
   =========================================================================== */
String fitText(const String& s, int maxW) {
  if (tft.textWidth(s) <= maxW) return s;
  String t = s;
  while (t.length() > 0 && tft.textWidth(t + "..") > maxW) t.remove(t.length() - 1);
  return t + "..";
}

/* ===========================================================================
   Chrome: header + footer + ambient light
   =========================================================================== */
void drawHeaderStatus() {
  tft.fillRect(SCR_W - 104, 0, 104, HEADER_H, COL_BG);
  tft.setTextDatum(MR_DATUM);
  tft.setTextSize(1);

  if (!signalOk) {
    tft.setTextColor(COL_FG_MUTED, COL_BG);
    tft.drawString("NO SIGNAL", SCR_W - 12, HEADER_H / 2);
    return;
  }

  // โหมดเกม -> ธีมแดง (GAME) / ปกติ -> accent (LIVE)
  uint16_t col  = inGame ? COL_RED : COL_ACCENT_HI;
  uint16_t glow = inGame ? COL_RED_GLOW : COL_ACCENT_GLOW;
  const char* txt = inGame ? "GAME" : "LIVE";

  tft.setTextColor(col, COL_BG);
  tft.drawString(txt, SCR_W - 12, HEADER_H / 2);

  int w  = tft.textWidth(txt);
  int cx = SCR_W - 12 - w - 8;
  // จุดไฟแบบไล่ระดับ (gradient glow)
  tft.fillCircle(cx, HEADER_H / 2, 6, glow);
  tft.fillCircle(cx, HEADER_H / 2, 4, blend(COL_BG, col, 140));
  tft.fillCircle(cx, HEADER_H / 2, 2, col);
}

void drawFooter() {
  tft.fillRect(0, SCR_H - FOOTER_H, SCR_W, FOOTER_H, COL_BG);
  tft.drawFastHLine(0, SCR_H - FOOTER_H, SCR_W, COL_HAIRLINE);

  int n = slideCount();
  int cy = SCR_H - FOOTER_H / 2;
  int pitch = 18, total = (n - 1) * pitch, sx = SCR_W / 2 - total / 2;
  for (int i = 0; i < n; i++) {
    int cx = sx + i * pitch;
    if (i == currentSlide) {
      tft.fillCircle(cx, cy, 6, COL_ACCENT_GLOW);   // glow ring
      tft.fillCircle(cx, cy, 3, COL_ACCENT);
    } else {
      tft.fillCircle(cx, cy, 2, COL_HAIRLINE);
    }
  }

  tft.setTextDatum(MR_DATUM);
  tft.setTextSize(1);
  if (autoSlide) {
    tft.setTextColor(COL_FG_MUTED, COL_BG);
    tft.drawString("AUTO", SCR_W - 12, cy);
  } else {
    tft.setTextColor(COL_ACCENT_HI, COL_BG);
    tft.drawString("PAUSED", SCR_W - 12, cy);
  }
}

void drawChrome() {
  // Header bar
  tft.fillRect(0, 0, SCR_W, HEADER_H, COL_BG);
  tft.fillCircle(16, HEADER_H / 2, 7, COL_ACCENT_GLOW);
  tft.fillCircle(16, HEADER_H / 2, 4, COL_ACCENT_DIM);
  tft.fillCircle(16, HEADER_H / 2, 2, COL_ACCENT);

  tft.setTextDatum(ML_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(COL_FG, COL_BG);
  tft.drawString("PC LIVE", 30, HEADER_H / 2);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_FG_MUTED, COL_BG);
  tft.drawString(slideNames[currentSlide], SCR_W / 2 + 18, HEADER_H / 2);

  drawHeaderStatus();
  tft.drawFastHLine(0, HEADER_H, SCR_W, COL_HAIRLINE);
  drawFooter();
}

void drawAmbient() {
  glowCircle(SCR_W - 38, 6,   118, COL_ACCENT_GLOW, 6);  // top-right pool
  glowCircle(6, SCR_H - 6,    104, COL_ACCENT_GLOW, 6);  // bottom-left pool
}

/* ===========================================================================
   Slide 1 — PERFORMANCE (bento: GPU + CPU + RAM, glowing usage outline)
   =========================================================================== */

// สีกรอบตามระดับการใช้งาน
uint16_t usageColor(float pct) {
  if (pct < 1.0f)  return COL_HAIRLINE;   // idle (ไม่มีโหลด)
  if (pct < 25.0f) return COL_GREEN;      // 1-25%
  if (pct < 50.0f) return COL_YELLOW;     // 25-50%
  if (pct < 75.0f) return COL_ORANGE;     // 50-75%
  return COL_RED;                         // 75%+
}

// การ์ดพร้อมกรอบเรืองแสงไล่ระดับ (gradient) รอบ outline
void drawMetricCard(int x, int y, int w, int h, uint16_t col) {
  tft.drawRoundRect(x - 3, y - 3, w + 6, h + 6, CARD_R + 3, blend(COL_BG, col, 36));
  tft.drawRoundRect(x - 2, y - 2, w + 4, h + 4, CARD_R + 2, blend(COL_BG, col, 84));
  tft.drawRoundRect(x - 1, y - 1, w + 2, h + 2, CARD_R + 1, blend(COL_BG, col, 150));
  tft.fillRoundRect(x, y, w, h, CARD_R, COL_SURFACE);
  tft.drawRoundRect(x, y, w, h, CARD_R, col);
  tft.drawFastHLine(x + CARD_R, y + 2, w - 2 * CARD_R, COL_SURFACE_HI);
}

String pctText(const String& v)  { return (v == "--") ? String("--") : v + "%"; }
String tempText(const String& t) { return (t == "--") ? String("TEMP --") : "TEMP " + t + "C"; }
String diskText(const String& d) { return (d == "--") ? String("DISK --") : "DISK " + d + "%"; }

// การ์ดMetric: label (บนซ้าย) + subtext (ล่างซ้าย) + ค่า % (ขวา) + progress bar (ล่าง)
void drawMetric(int x, int y, int w, int h,
                const char* label, const String& val,
                const String& sub, float pct) {
  uint16_t col = usageColor(pct);
  drawMetricCard(x, y, w, h, col);

  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(COL_FG_MUTED, COL_SURFACE);
  tft.drawString(label, x + 14, y + 8);
  tft.drawString(sub,   x + 14, y + 24);

  tft.setTextDatum(TR_DATUM);
  tft.setTextSize(3);
  tft.setTextColor(COL_FG, COL_SURFACE);
  tft.drawString(fitText(val, w - 110), x + w - 14, y + 6);

  int bx = x + 14, by = y + h - 14, bw = w - 28, bh = 6;
  tft.fillRoundRect(bx, by, bw, bh, 3, COL_HAIRLINE);
  int fw = (int)(bw * constrain(pct, 0.0f, 100.0f) / 100.0f);
  if (fw > 0) tft.fillRoundRect(bx, by, fw, bh, 3, col);
}

void refreshPerf() {
  String gs = gpuVal + "|" + gpuTemp;
  if (gs != lastPerfGpu) {
    lastPerfGpu = gs;
    drawMetric(10, 36, 300, 50, "GPU", pctText(gpuVal), tempText(gpuTemp), gpuVal.toFloat());
  }
  String cs = cpuVal + "|" + cpuTemp;
  if (cs != lastPerfCpu) {
    lastPerfCpu = cs;
    drawMetric(10, 95, 300, 50, "CPU", pctText(cpuVal), tempText(cpuTemp), cpuVal.toFloat());
  }
  String rs = ramVal + "|" + diskVal;
  if (rs != lastPerfRam) {
    lastPerfRam = rs;
    drawMetric(10, 154, 300, 50, "RAM", pctText(ramVal), diskText(diskVal), ramVal.toFloat());
  }
}

/* ===========================================================================
   Slide 2 — GAMING (session detail)
   =========================================================================== */
void drawGameStatic() {
  tft.drawFastHLine(70, 116, 180, COL_HAIRLINE);
  tft.setTextDatum(TC_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(COL_FG_MUTED, COL_BG);
  tft.drawString("SESSION TIME", SCR_W / 2, 124);
}

void refreshGame(bool inSession, const String& name, const String& time) {
  int sess = inSession ? 1 : 0;
  bool sessionChanged = (sess != lastInSession);
  lastInSession = sess;

  // status pill — วาดเฉพาะเมื่อสถานะเปลี่ยน
  if (sessionChanged) {
    int pw = 150, ph = 26, px = SCR_W / 2 - pw / 2, py = 44;
    uint16_t pillBg = inSession ? COL_ACCENT_DIM : COL_SURFACE;
    uint16_t pillTx = inSession ? COL_ACCENT_HI : COL_FG_MUTED;
    uint16_t pillBd = inSession ? COL_ACCENT : COL_HAIRLINE;
    tft.fillRoundRect(px, py, pw, ph, ph / 2, pillBg);
    tft.drawRoundRect(px, py, pw, ph, ph / 2, pillBd);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(1);
    tft.setTextColor(pillTx, pillBg);
    tft.drawString(inSession ? "IN SESSION" : "IDLE", SCR_W / 2, py + ph / 2);
  }

  // game name
  if (sessionChanged || name != lastGameName) {
    lastGameName = name;
    tft.fillRect(10, 74, 300, 30, COL_BG);
    tft.setTextDatum(TC_DATUM);
    tft.setTextSize(3);
    tft.setTextColor(inSession ? COL_FG : COL_FG_MUTED, COL_BG);
    tft.drawString(fitText(name, 290), SCR_W / 2, 74);
  }

  // timer — MM:SS กว้างคงที่ วาดทับได้เลย ไม่ต้อง clear พื้นที่ใหญ่ (ลดกระพริบ)
  if (sessionChanged || time != lastGameTime) {
    lastGameTime = time;
    tft.setTextDatum(TC_DATUM);
    tft.setTextSize(4);
    tft.setTextColor(inSession ? COL_ACCENT_HI : COL_FG_MUTED, COL_BG);
    tft.drawString(time, SCR_W / 2, 140);
  }
}

void refreshGamePulse(bool inSession) {
  float b = 0.5f + 0.5f * sinf(millis() / 600.0f);
  uint16_t c = inSession ? blend(COL_BG, COL_ACCENT, (uint8_t)(120 + 120 * b)) : COL_HAIR_SOFT;
  tft.fillRoundRect(60, 192, 200, 4, 2, c);   // breathing underline
}

/* ===========================================================================
   Slide 3 — MEDIA (now playing + animated visualizer)
   =========================================================================== */
void drawAlbumTile(int x, int y, int w, int h) {
  glowCircle(x + w / 2, y + h / 2, w, COL_ACCENT_GLOW, 5);
  tft.fillRoundRect(x, y, w, h, 12, COL_SURFACE);
  tft.drawRoundRect(x, y, w, h, 12, COL_HAIRLINE);
  int cx = x + w / 2, cy = y + h / 2;
  int bars[4] = { 12, 26, 18, 30 };
  for (int i = 0; i < 4; i++) {
    int bx = cx - 18 + i * 12, bh = bars[i];
    tft.fillRoundRect(bx, cy - bh / 2, 6, bh, 3, (i % 2 == 0) ? COL_ACCENT : COL_ACCENT_DIM);
  }
}

void drawMediaStatic() {
  drawAlbumTile(24, 52, 80, 80);
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(COL_FG_MUTED, COL_BG);
  tft.drawString("NOW PLAYING", 118, 58);
  tft.drawString("SOURCE: DESKTOP", 118, 116);
  tft.drawFastHLine(10, 140, 300, COL_HAIRLINE);
}

void refreshMedia() {
  if (songTitle == lastSongTitle) return;
  lastSongTitle = songTitle;

  tft.fillRect(118, 74, 192, 38, COL_BG);   // clear 2-line title area
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(2);
  tft.setTextColor(COL_FG, COL_BG);

  int maxW = 186, splitAt = 0;
  for (unsigned i = 1; i <= songTitle.length(); i++) {
    if (tft.textWidth(songTitle.substring(0, i)) > maxW) { splitAt = i - 1; break; }
  }
  if (splitAt == 0 || splitAt >= (int)songTitle.length()) {
    tft.drawString(fitText(songTitle, maxW), 118, 74);
  } else {
    String l1 = songTitle.substring(0, splitAt); l1.trim();
    String rest = songTitle.substring(splitAt);  rest.trim();
    tft.drawString(l1, 118, 74);
    if (rest.length()) tft.drawString(fitText(rest, maxW), 118, 92);
  }
}

void refreshVisualizer() {
  const int x0 = 20, pitch = 14, N = 20, barw = 8, baseY = 200;
  tft.fillRect(x0 - 4, 150, N * pitch + 8, 56, COL_BG);   // clear track
  uint32_t t = millis();
  for (int i = 0; i < N; i++) {
    float a = 0.5f + 0.5f * sinf(t / 300.0f + i * 0.55f);
    float b = 0.5f + 0.5f * sinf(t / 520.0f + i * 0.90f);
    float v = a * 0.6f + b * 0.4f;
    int bh = 4 + (int)(v * 36);
    uint16_t c = blend(COL_ACCENT_DIM, COL_ACCENT, (uint8_t)(60 + 160 * v));
    tft.fillRoundRect(x0 + i * pitch, baseY - bh, barw, bh, 3, c);
  }
}

/* ===========================================================================
   Slide orchestration
   =========================================================================== */
void invalidateDynamicCache() {
  lastPerfGpu = lastPerfCpu = lastPerfRam = "";
  lastGameName = lastGameTime = lastSongTitle = "";
  lastInSession = -1;
}

void drawSlide(int idx) {
  currentSlide = idx;
  tft.fillScreen(COL_BG);
  drawAmbient();
  drawChrome();
  invalidateDynamicCache();
  switch (idx) {
    case SLIDE_PERF:
      refreshPerf();
      break;
    case SLIDE_GAME:
      drawGameStatic();
      refreshGame(gameName != "None", gameName, gameTime);
      refreshGamePulse(gameName != "None");
      break;
    case SLIDE_MEDIA:
      drawMediaStatic(); refreshMedia(); refreshVisualizer();
      break;
  }
}

void refreshDynamic() {
  switch (currentSlide) {
    case SLIDE_PERF:  refreshPerf();  break;
    case SLIDE_GAME:
      refreshGame(gameName != "None", gameName, gameTime);
      refreshGamePulse(gameName != "None");
      break;
    case SLIDE_MEDIA: refreshMedia(); break;
  }
}

void goTo(int n, bool forward) {
  if (millis() - lastSlideMs < 350) return;   // debounce touch
  lastSlideMs = millis();
  drawSlide(n);                                // เปลี่ยน slide ทันที (ไม่มี transition)
}

// เด้งไป slide ที่กำหนดแบบไม่สน debounce (ใช้ตอนเข้าโหมดเกมอัตโนมัติ)
void goToForce(int n) {
  lastSlideMs = millis();
  lastInputMs = millis();   // ให้ dwell เต็มรอบก่อน auto-advance
  drawSlide(n);
}

void nextSlide() { int n = slideCount(); goTo((currentSlide + 1) % n, true);  }
void prevSlide() { int n = slideCount(); goTo((currentSlide - 1 + n) % n, false); }

/* ===========================================================================
   Input & data
   =========================================================================== */
void handleTouch(uint32_t now) {
  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    int16_t x = constrain(map(p.x, TS_MINX, TS_MAXX, 0, SCR_W), 0, SCR_W - 1);
    int16_t y = constrain(map(p.y, TS_MINY, TS_MAXY, 0, SCR_H), 0, SCR_H - 1);
    curX = x; curY = y;
    if (!touching) { touching = true; startX = x; startY = y; lastInputMs = now; }
  } else if (touching) {
    touching = false;
    int dx = (int)curX - (int)startX;
    if (dx <= -45)         nextSlide();               // swipe left  -> next
    else if (dx >= 45)     prevSlide();               // swipe right -> prev
    else if (startX < 100) prevSlide();               // tap left third
    else if (startX > 220) nextSlide();               // tap right third
    else { autoSlide = !autoSlide; drawFooter(); }    // tap center -> pause/play
    lastInputMs = now;
  }
}

// ดึงค่าฟิลด์ระหว่าง marker `key` กับ `nextKey` (ทนต่อ comma ในชื่อเกม/เพลง)
String field(const String& d, const char* key, const char* nextKey) {
  int a = d.indexOf(key);
  if (a == -1) return "";
  a += strlen(key);
  int b = d.indexOf(nextKey, a);
  if (b == -1) return "";
  String v = d.substring(a, b);
  v.trim();
  return v;
}

String fieldRest(const String& d, const char* key) {
  int a = d.indexOf(key);
  if (a == -1) return "";
  String v = d.substring(a + strlen(key));
  v.trim();
  return v;
}

// รูปแบบ: CPU:xx.x,CPUT:xx,GPU:xx.x,GPUT:xx,RAM:xx.x,DISK:xx.x,Game:Name,Time:MM:SS,Song:Title
void parseData(const String& data) {
  String v;
  v = field(data, "CPU:",  ",CPUT:"); if (v.length()) cpuVal  = v;
  v = field(data, "CPUT:", ",GPU:");  if (v.length()) cpuTemp = v;
  v = field(data, "GPU:",  ",GPUT:"); if (v.length()) gpuVal  = v;
  v = field(data, "GPUT:", ",RAM:");  if (v.length()) gpuTemp = v;
  v = field(data, "RAM:",  ",DISK:"); if (v.length()) ramVal  = v;
  v = field(data, "DISK:", ",Game:"); if (v.length()) diskVal = v;
  v = field(data, "Game:", ",Time:"); if (v.length()) gameName = v;
  v = field(data, "Time:", ",Song:"); if (v.length()) gameTime = v;
  v = fieldRest(data, "Song:");       if (v.length()) songTitle = v;
}

void processLine(const String& line) {
  parseData(line);
  lastPacketMs = millis();

  bool wasSignal = signalOk;
  bool wasGame   = inGame;
  signalOk = true;
  inGame   = (gameName != "None");

  if (!wasSignal) drawHeaderStatus();

  if (wasGame != inGame) {
    drawHeaderStatus();      // LIVE <-> GAME theme
    drawFooter();            // จำนวน dot เปลี่ยนตามจำนวน slide
    if (inGame) {
      // เพิ่งเข้าเกม -> เด้งไปหน้า GAMING อัตโนมัติ (เหลือ 2 slide)
      goToForce(SLIDE_GAME);
    }
  }

  refreshDynamic();
}

/* ===========================================================================
   Setup / loop
   =========================================================================== */
void setup() {
  Serial.begin(115200);

  pinMode(21, OUTPUT);          // CYD backlight
  digitalWrite(21, HIGH);

  tft.init();
  tft.setRotation(1);           // landscape 320x240

  // เริ่มต้นทัชแยกบน bus ของตัวเอง
  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  tft.fillScreen(COL_BG);

  lastPacketMs = millis();
  lastInputMs  = millis();
  lastSlideMs  = millis();      // allow the first manual move soon after boot
  drawSlide(SLIDE_PERF);
}

void loop() {
  uint32_t now = millis();

  // Non-blocking serial line reader
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') { processLine(rxBuf); rxBuf = ""; }
    else if (c != '\r' && rxBuf.length() < 180) rxBuf += c;
  }

  handleTouch(now);

  // Signal timeout -> NO SIGNAL
  // ใช้ millis() ตรงนี้แทน `now` ที่จับไว้ก่อนอ่าน serial เพื่อกัน uint32_t underflow
  // (ตอนรับ packet สำเร็จ lastPacketMs จะ >= now ทำให้ now - lastPacketMs overflow เป็นค่ามหาศาล)
  if (signalOk && millis() - lastPacketMs > 15000) { signalOk = false; drawHeaderStatus(); }

  // Auto-loop slideshow
  if (autoSlide && now - lastInputMs > AUTO_ADVANCE_MS) {
    lastInputMs = now;
    nextSlide();
  }

  // Per-slide animation (~14fps visualizer, ~8fps pulse)
  if (currentSlide == SLIDE_MEDIA && now - lastAnimMs > 70) {
    lastAnimMs = now; refreshVisualizer();
  }
  if (currentSlide == SLIDE_GAME && now - lastAnimMs > 120) {
    lastAnimMs = now; refreshGamePulse(gameName != "None");
  }
}
