/* ============================================================================
   PC Live Dashboard — LVGL v8 + TFT_eSPI prototype
   Board: ESP32-2432S028R (CYD, 320x240, XPT2046)

   Scope (step 1): header + slide PERFORMANCE (GPU/CPU/RAM cards)
   - glowing outline + shadow ตามระดับการใช้งาน
   - serial protocol เดิม (ไม่เปลี่ยนฝั่ง Go):
     CPU:x,CPUT:x,GPU:x,GPUT:x,RAM:x,DISK:x,Game:,Time:,Song:
   ============================================================================ */

#include <lvgl.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>

TFT_eSPI tft = TFT_eSPI();

#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

SPIClass touchSPI = SPIClass(HSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

#define SCR_W 320
#define SCR_H 240

/* ---------------- palette ---------------- */
#define HEX_BG      0x050506
#define HEX_SURFACE 0x101013
#define HEX_FG      0xEDEDEF
#define HEX_MUTED   0x8A8F98
#define HEX_HAIR    0x202026
#define HEX_ACCENT  0x5E6AD2
#define HEX_GREEN   0x34C759
#define HEX_YELLOW  0xFFCC00
#define HEX_ORANGE  0xFF9500
#define HEX_RED     0xFF3B30

/* ---------------- state ---------------- */
String cpuVal = "0", ramVal = "0", gpuVal = "--";
String cpuTemp = "--", gpuTemp = "--", diskVal = "--";
String gameName = "None", gameTime = "00:00", songTitle = "Idle";
bool signalOk = false, inGame = false;

uint32_t lastPacketMs = 0;
String rxBuf = "";

/* ---------------- LVGL buffers ---------------- */
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[SCR_W * 40];   // partial buffer ~25KB (ไม่มี PSRAM)

/* ---------------- metric card ---------------- */
struct MetricCard {
  lv_obj_t *card;
  lv_obj_t *value;
  lv_obj_t *sub;
  lv_obj_t *bar;
  String lastVal, lastSub;
};
MetricCard gpuCard, cpuCard, ramCard;
lv_obj_t *statusLabel, *statusDot;

/* ===========================================================================
   LVGL port
   =========================================================================== */
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&color_p->full, w * h, true);
  tft.endWrite();
  lv_disp_flush_ready(disp);
}

// ช่วงค่า raw ของ XPT2046 (rotation 1) — ปรับถ้าแตะเพี้ยน
const int TS_MINX = 200, TS_MAXX = 3700;
const int TS_MINY = 240, TS_MAXY = 3800;

void my_touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    data->point.x = constrain(map(p.x, TS_MINX, TS_MAXX, 0, SCR_W), 0, SCR_W - 1);
    data->point.y = constrain(map(p.y, TS_MINY, TS_MAXY, 0, SCR_H), 0, SCR_H - 1);
    data->state = LV_INDEV_STATE_PR;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

/* ===========================================================================
   Style helpers
   =========================================================================== */
uint32_t usageHex(int pct) {
  if (pct < 1)  return HEX_HAIR;
  if (pct < 25) return HEX_GREEN;
  if (pct < 50) return HEX_YELLOW;
  if (pct < 75) return HEX_ORANGE;
  return HEX_RED;
}

int pctOf(const String &v) {
  return (v == "--") ? 0 : constrain(v.toInt(), 0, 100);
}

String pctText(const String &v) { return (v == "--") ? String("--") : v + "%"; }
String tempText(const String &t) { return (t == "--") ? String("TEMP --") : "TEMP " + t + "C"; }
String diskText(const String &d) { return (d == "--") ? String("DISK --") : "DISK " + d + "%"; }

// ใช้สีกรอบ/เรืองแสงตามระดับการใช้งาน
void applyUsageColor(lv_obj_t *card, lv_obj_t *bar, int pct) {
  lv_color_t c = lv_color_hex(usageHex(pct));
  lv_obj_set_style_border_color(card, c, 0);
  lv_obj_set_style_outline_color(card, c, 0);
  lv_obj_set_style_shadow_color(card, c, 0);
  lv_obj_set_style_bg_color(bar, c, LV_PART_INDICATOR);
}

/* ===========================================================================
   UI builders
   =========================================================================== */
lv_obj_t *makeLabel(lv_obj_t *parent, const char *txt, const lv_font_t *font,
                    uint32_t color, lv_align_t align, int x, int y, int w) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  if (w > 0) lv_obj_set_width(l, w);
  lv_obj_align(l, align, x, y);
  return l;
}

void buildMetricCard(MetricCard &m, const char *label, int y) {
  lv_obj_t *card = lv_obj_create(lv_scr_act());
  m.card = card;
  lv_obj_set_size(card, 300, 54);
  lv_obj_set_pos(card, 10, y);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(card, 10, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(HEX_SURFACE), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(HEX_HAIR), 0);
  lv_obj_set_style_pad_all(card, 0, 0);
  // กรอบเรืองแสง (outline + soft shadow)
  lv_obj_set_style_outline_width(card, 2, 0);
  lv_obj_set_style_outline_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_outline_pad(card, 1, 0);
  lv_obj_set_style_shadow_width(card, 14, 0);
  lv_obj_set_style_shadow_spread(card, 1, 0);
  lv_obj_set_style_shadow_opa(card, LV_OPA_40, 0);

  makeLabel(card, label, &lv_font_montserrat_14, HEX_MUTED, LV_ALIGN_TOP_LEFT, 14, 8, 0);
  m.sub = makeLabel(card, "TEMP --", &lv_font_montserrat_14, HEX_MUTED, LV_ALIGN_TOP_LEFT, 14, 28, 0);
  m.value = makeLabel(card, "--", &lv_font_montserrat_28, HEX_FG, LV_ALIGN_TOP_RIGHT, -14, 4, 110);
  lv_obj_set_style_text_align(m.value, LV_TEXT_ALIGN_RIGHT, 0);

  m.bar = lv_bar_create(card);
  lv_obj_set_size(m.bar, 272, 6);
  lv_obj_align(m.bar, LV_ALIGN_BOTTOM_LEFT, 14, -12);
  lv_bar_set_range(m.bar, 0, 100);
  lv_obj_set_style_bg_color(m.bar, lv_color_hex(HEX_HAIR), LV_PART_MAIN);
  lv_obj_set_style_radius(m.bar, 3, LV_PART_MAIN);
  lv_obj_set_style_radius(m.bar, 3, LV_PART_INDICATOR);
}

void buildUI() {
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(HEX_BG), 0);

  // ---- header ----
  lv_obj_t *header = lv_obj_create(lv_scr_act());
  lv_obj_set_size(header, SCR_W, 30);
  lv_obj_set_pos(header, 0, 0);
  lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(header, 0, 0);
  lv_obj_set_style_border_width(header, 0, 0);
  lv_obj_set_style_bg_color(header, lv_color_hex(HEX_BG), 0);
  lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(header, 1, 0);
  lv_obj_set_style_border_color(header, lv_color_hex(HEX_HAIR), 0);

  makeLabel(header, "PC LIVE", &lv_font_montserrat_14, HEX_FG, LV_ALIGN_LEFT_MID, 30, 0, 0);
  lv_obj_t *logo = lv_obj_create(header);
  lv_obj_set_size(logo, 14, 14);
  lv_obj_set_style_radius(logo, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(logo, lv_color_hex(HEX_ACCENT), 0);
  lv_obj_set_style_border_width(logo, 0, 0);
  lv_obj_align(logo, LV_ALIGN_LEFT_MID, 10, 0);

  statusDot = lv_obj_create(header);
  lv_obj_set_size(statusDot, 7, 7);
  lv_obj_set_style_radius(statusDot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(statusDot, 0, 0);
  lv_obj_set_style_bg_color(statusDot, lv_color_hex(HEX_HAIR), 0);
  lv_obj_align(statusDot, LV_ALIGN_RIGHT_MID, -66, 0);

  statusLabel = makeLabel(header, "NO SIGNAL", &lv_font_montserrat_14, HEX_MUTED,
                          LV_ALIGN_RIGHT_MID, -12, 0, 0);

  // ---- slide 1: 3 การ์ด ----
  buildMetricCard(gpuCard, "GPU", 38);
  buildMetricCard(cpuCard, "CPU", 96);
  buildMetricCard(ramCard, "RAM", 154);
}

void setStatus() {
  uint32_t c = !signalOk ? HEX_MUTED : (inGame ? HEX_RED : HEX_ACCENT);
  lv_label_set_text(statusLabel, !signalOk ? "NO SIGNAL" : (inGame ? "GAME" : "LIVE"));
  lv_obj_set_style_text_color(statusLabel, lv_color_hex(c), 0);
  lv_obj_set_style_bg_color(statusDot, lv_color_hex(c), 0);
}

/* ===========================================================================
   Data -> UI
   =========================================================================== */
void updateCard(MetricCard &m, const String &val, const String &sub, int pct) {
  String sv = pctText(val);
  if (sv != m.lastVal) {
    lv_label_set_text(m.value, sv.c_str());
    lv_bar_set_value(m.bar, pct, LV_ANIM_ON);
    m.lastVal = sv;
  }
  if (sub != m.lastSub) {
    lv_label_set_text(m.sub, sub.c_str());
    m.lastSub = sub;
  }
  applyUsageColor(m.card, m.bar, pct);
}

void refreshPerf() {
  int g = pctOf(gpuVal), c = pctOf(cpuVal), r = pctOf(ramVal);
  updateCard(gpuCard, gpuVal, tempText(gpuTemp), g);
  updateCard(cpuCard, cpuVal, tempText(cpuTemp), c);
  updateCard(ramCard, ramVal, diskText(diskVal), r);
}

/* ===========================================================================
   Serial parsing (protocol เดิม)
   =========================================================================== */
String field(const String &d, const char *key, const char *nextKey) {
  int a = d.indexOf(key);
  if (a == -1) return "";
  a += strlen(key);
  int b = d.indexOf(nextKey, a);
  if (b == -1) return "";
  String v = d.substring(a, b);
  v.trim();
  return v;
}
String fieldRest(const String &d, const char *key) {
  int a = d.indexOf(key);
  if (a == -1) return "";
  String v = d.substring(a + strlen(key));
  v.trim();
  return v;
}

void processLine(const String &d) {
  String v;
  v = field(d, "CPU:",  ",CPUT:"); if (v.length()) cpuVal  = v;
  v = field(d, "CPUT:", ",GPU:");  if (v.length()) cpuTemp = v;
  v = field(d, "GPU:",  ",GPUT:"); if (v.length()) gpuVal  = v;
  v = field(d, "GPUT:", ",RAM:");  if (v.length()) gpuTemp = v;
  v = field(d, "RAM:",  ",DISK:"); if (v.length()) ramVal  = v;
  v = field(d, "DISK:", ",Game:"); if (v.length()) diskVal = v;
  v = field(d, "Game:", ",Time:"); if (v.length()) gameName = v;
  v = field(d, "Time:", ",Song:"); if (v.length()) gameTime = v;
  v = fieldRest(d, "Song:");       if (v.length()) songTitle = v;

  lastPacketMs = millis();
  bool wasSignal = signalOk;
  bool wasGame = inGame;
  signalOk = true;
  inGame = (gameName != "None");
  if (!wasSignal || wasGame != inGame) setStatus();
  refreshPerf();
}

/* ===========================================================================
   Setup / loop
   =========================================================================== */
void setup() {
  Serial.begin(115200);

  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);

  tft.init();
  tft.setRotation(1);
  tft.setSwapBytes(true);

  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf, NULL, SCR_W * 40);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCR_W;
  disp_drv.ver_res = SCR_H;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touch_read;
  lv_indev_drv_register(&indev_drv);

  buildUI();
  setStatus();
  refreshPerf();

  lastPacketMs = millis();
}

void loop() {
  static uint32_t lastTick = 0;
  uint32_t now = millis();
  lv_tick_inc(now - lastTick);
  lastTick = now;

  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n') { processLine(rxBuf); rxBuf = ""; }
    else if (ch != '\r' && rxBuf.length() < 180) rxBuf += ch;
  }

  if (signalOk && millis() - lastPacketMs > 15000) {
    signalOk = false;
    setStatus();
  }

  lv_timer_handler();
  delay(5);
}