#include "wled.h"
#include <SSLClient.h>
#include "trust_anchors.h"

/*
 * ComEd Pricing usermod
 *
 * Polls the ComEd hourly-pricing 5-minute feed (https://hourlypricing.comed.com/hp-api/)
 * and registers two 2D-only effects:
 *  - "ComedPriceGraph": bar graph of the most recent prices, one column per 5-minute
 *    slot, newest slot on the right. Slots with no published price stay unlit.
 *  - "ComedPricingText": scrolls the newest reading right-to-left like the built-in
 *    Scrolling Text effect, e.g. "8:30am: $0.031" (reading's timestamp in local time).
 * Colors are three hard bands on absolute price: green below the green threshold
 * (default 8c), orange up to the orange threshold (default 15c), red at/above it.
 * The graph's Y axis auto-scales to the highest price currently on screen, so the
 * tallest bar always reaches the top row; the bottom of the graph stays at 0c.
 */

#define COMED_MAX_SLOTS 32
#define COMED_SLOT_MS   300000ULL  // 5 minutes
#define COMED_MIN_SCALE_CENTS 1.0f // don't let auto-scale amplify sub-cent noise

// Shared between the usermod's loop() (writer) and the effect function (reader).
// Both run sequentially on the main loop task, so no locking is needed.
static float    s_prices[COMED_MAX_SLOTS];   // [COMED_MAX_SLOTS-1]=newest slot ... [0]=oldest; NAN = missing
static uint64_t s_newestMillis = 0;          // millisUTC of the newest reading
static bool     s_haveData = false;
static float    s_greenBelow = 8.0f;         // cents; below this -> green
static float    s_orangeBelow = 15.0f;       // cents; below this -> orange, at/above -> red
static uint32_t s_lastEffectRender = 0;      // millis() of last effect frame

// Local copy of FX.cpp's non-exported static fallback
static void mode_static(void) {
  SEGMENT.fill(SEGCOLOR(0));
}
#define FX_FALLBACK_STATIC { mode_static(); return; }

// Three hard bands, half-open: (-inf, green) green, [green, orange) orange, [orange, inf) red.
// The orange is deliberately redder than WLED's ORANGE (255,160,0), which reads too close to
// yellow when a green bar is right next to it.
static uint32_t priceColor(float price) {
  if (price >= s_orangeBelow) return RGBW32(255,   0, 0, 0); // red
  if (price >= s_greenBelow)  return RGBW32(255, 120, 0, 0); // orange
  return RGBW32(0, 255, 0, 0);                               // green
}

static void mode_comed_price_graph(void) {
  if (!strip.isMatrix || !SEGMENT.is2D()) FX_FALLBACK_STATIC; // not a 2D set-up
  s_lastEffectRender = millis();
  const int cols = SEG_W, rows = SEG_H;
  SEGMENT.fill(BLACK);
  if (!s_haveData) return;

  int shown = min((int)COMED_MAX_SLOTS, cols);

  // Pass 1: peak price in the visible window, so the tallest bar lands on the top row.
  float vmax = 0.0f;
  for (int k = 0; k < shown; k++) {
    float p = s_prices[COMED_MAX_SLOTS - 1 - k];
    if (!isnan(p) && p > vmax) vmax = p;
  }
  if (vmax < COMED_MIN_SCALE_CENTS) vmax = COMED_MIN_SCALE_CENTS;

  // Pass 2: draw one column per slot, heights proportional to price against that peak.
  for (int k = 0; k < shown; k++) {         // k=0 -> newest slot -> rightmost column
    float price = s_prices[COMED_MAX_SLOTS - 1 - k];
    if (isnan(price)) continue;             // missing 5-min price -> unlit gap column
    int x = cols - 1 - k;
    float frac = price / vmax;
    if (frac > 1.0f) frac = 1.0f;
    if (frac < 0.0f) frac = 0.0f;           // negative prices clamp to 0
    int h = (int)roundf(frac * rows);
    if (h < 1) h = 1;                       // 1px floor so the column stays visible
    uint32_t c = priceColor(price);
    for (int y = 0; y < h; y++) SEGMENT.setPixelColorXY(x, rows - 1 - y, c);
  }
}
static const char _data_FX_MODE_COMED_PRICE_GRAPH[] PROGMEM = "ComedPriceGraph@;;;2;";

// Scrolls the newest reading right-to-left like the built-in Scrolling Text effect
// (FX.cpp mode_2Dscrollingtext), e.g. "8:30am: $0.031". The time is the reading's
// millisUTC converted to local time; the text takes the price-ramp color.
static void mode_comed_pricing_text(void) {
  if (!strip.isMatrix || !SEGMENT.is2D()) FX_FALLBACK_STATIC; // not a 2D set-up
  s_lastEffectRender = millis();
  const int cols = SEG_W;
  const int rows = SEG_H;
  SEGMENT.fade_out(255 - (SEGMENT.custom1>>4)); // trail
  if (!s_haveData) return;

  unsigned letterWidth, letterHeight;
  switch (map(SEGMENT.custom2, 0, 255, 1, 5)) {
    default:
    case 1: letterWidth = 4; letterHeight =  6; break;
    case 2: letterWidth = 5; letterHeight =  8; break;
    case 3: letterWidth = 6; letterHeight =  8; break;
    case 4: letterWidth = 7; letterHeight =  9; break;
    case 5: letterWidth = 5; letterHeight = 12; break;
  }

  // WLED has no public UTC->local converter for arbitrary epochs (the Timezone object
  // is private to ntp.cpp), so apply the current offset: localTime - toki.second().
  updateLocalTime();
  time_t entryLocal = (time_t)(s_newestMillis / 1000ULL) + (localTime - (time_t)toki.second());
  float price = s_prices[COMED_MAX_SLOTS-1]; // anchor slot is always filled
  char pbuf[12];
  dtostrf(price / 100.0f, 0, 3, pbuf); // %f is unavailable (core built with NEWLIB_NANO_FORMAT)
  char text[36];
  snprintf_P(text, sizeof(text), PSTR("%d:%02d%s: $%s"),
             hourFormat12(entryLocal), minute(entryLocal),
             isPM(entryLocal) ? "pm" : "am", pbuf);

  const int numberOfLetters = strlen(text);
  const int width = numberOfLetters * (int)letterWidth;
  const int yoffset = map(SEGMENT.intensity, 0, 255, -rows/2, rows/2) + (rows - (int)letterHeight)/2;

  if (SEGENV.step < strip.now) {
    if (width > cols) ++SEGENV.aux0 %= width + cols;   // scroll right to left
    else              SEGENV.aux0 = (cols + width)/2;  // fits on screen: hold centered
    SEGENV.step = strip.now + map(SEGMENT.speed, 0, 255, 250, 50); // shift letters every ~250ms to ~50ms
  }

  uint32_t col = priceColor(price);
  for (int i = 0; i < numberOfLetters; i++) {
    int xoffset = cols - (int)SEGENV.aux0 + (int)letterWidth*i;
    if (xoffset + (int)letterWidth < 0) continue; // don't draw characters off-screen
    SEGMENT.drawCharacter(text[i], xoffset, yoffset, letterWidth, letterHeight, col, col, 0);
  }
}
static const char _data_FX_MODE_COMED_PRICING_TEXT[] PROGMEM = "ComedPricingText@!,Y Offset,Trail,Font size;;;2;ix=128,c1=0";

class ComedPricingUsermod : public Usermod {
  private:
    bool     enabled = false;
    uint16_t updateIntervalSec = 60;
    float    greenBelowCents = 8.0f;
    float    orangeBelowCents = 15.0f;
    bool     fetchOnlyWhenActive = true;
    unsigned long lastFetch = 0;
    bool     lastFetchOk = false;

    static const char _name[];

    // Fetch the 5-minute feed and fill the slot grid. The Tasmota Arduino core WLED
    // builds with ships no TLS (no WiFiClientSecure, mbedTLS SSL stripped from the
    // precompiled IDF), so HTTPS is done with SSLClient (BearSSL compiled from source)
    // over a plain WiFiClient, validating against the roots in trust_anchors.h.
    //
    // The feed is newest-first, so the first entry anchors the grid; every other entry
    // is placed by its timestamp distance from the anchor. Slots the feed skipped
    // remain NAN (unlit). The body is scanned as a raw stream (HTTP/1.0 = unchunked),
    // so only ~1.5KB of the ~13KB/24h response is ever read and no JSON document is
    // allocated. Note: the TLS handshake blocks the main loop for ~1-3s per fetch.
    bool fetchPrices() {
      // Static so the BearSSL buffers live in BSS (not the loop task stack) and the
      // session cache can speed up reconnects. Entropy pin: any ADC-capable pin works,
      // SSLClient only reads noise from it.
      static WiFiClient wifiClient;
      static SSLClient tls(wifiClient, TAs, (size_t)TAs_NUM, /*analog_pin=*/1);

      tls.setTimeout(5000);
      if (!tls.connect("hourlypricing.comed.com", 443)) { tls.stop(); return false; }
      tls.print(F("GET /api?type=5minutefeed HTTP/1.0\r\n"
                  "Host: hourlypricing.comed.com\r\n"
                  "User-Agent: WLED-ComedPricing\r\n"
                  "Connection: close\r\n\r\n"));

      String statusLine = tls.readStringUntil('\n');
      if (statusLine.indexOf(F(" 200")) < 0 || !tls.find("\r\n\r\n")) { tls.stop(); return false; }

      float tmp[COMED_MAX_SLOTS];
      for (int i = 0; i < COMED_MAX_SLOTS; i++) tmp[i] = NAN;
      uint64_t newestMillis = 0;
      uint8_t entries = 0;

      char buf[24];
      while (tls.find("\"millisUTC\":\"")) {
        size_t len = tls.readBytesUntil('"', buf, sizeof(buf) - 1);
        buf[len] = '\0';
        uint64_t entryMillis = strtoull(buf, nullptr, 10);
        if (!tls.find("\"price\":\"")) break;
        len = tls.readBytesUntil('"', buf, sizeof(buf) - 1);
        buf[len] = '\0';
        float price = atof(buf);

        if (entries == 0) newestMillis = entryMillis; // first entry = newest, anchors the grid
        if (entryMillis > newestMillis) break;        // out-of-order safety; keep what we have
        uint32_t slotBack = (uint32_t)((newestMillis - entryMillis + COMED_SLOT_MS/2) / COMED_SLOT_MS);
        if (slotBack >= COMED_MAX_SLOTS) break;       // older than the grid; done
        tmp[COMED_MAX_SLOTS - 1 - slotBack] = price;
        entries++;
      }
      tls.stop(); // abort the rest of the 24h body

      if (entries == 0) return false;
      memcpy(s_prices, tmp, sizeof(s_prices));
      s_newestMillis = newestMillis;
      s_haveData = true;
      return true;
    }

  public:
    void setup() override {
      strip.addEffect(255, &mode_comed_price_graph, _data_FX_MODE_COMED_PRICE_GRAPH);
      strip.addEffect(255, &mode_comed_pricing_text, _data_FX_MODE_COMED_PRICING_TEXT);
    }

    void loop() override {
      if (!enabled || !WLED_CONNECTED) return;
      if (strip.isUpdating()) return;
      if (fetchOnlyWhenActive && millis() - s_lastEffectRender > 70000UL) return;
      if (lastFetch != 0 && millis() - lastFetch < updateIntervalSec * 1000UL) return;
      lastFetch = millis();
      lastFetchOk = fetchPrices();
      DEBUG_PRINTF_P(PSTR("[ComEd] fetch %s\n"), lastFetchOk ? "ok" : "FAILED");
    }

    void addToJsonInfo(JsonObject& root) override {
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");
      JsonArray arr = user.createNestedArray(FPSTR(_name));
      if (!enabled)          { arr.add(F("disabled")); return; }
      if (!s_haveData)       { arr.add(F("no data")); return; }
      uint8_t slots = 0;
      for (int i = 0; i < COMED_MAX_SLOTS; i++) if (!isnan(s_prices[i])) slots++;
      char pbuf[12];
      dtostrf(s_prices[COMED_MAX_SLOTS-1], 0, 1, pbuf); // %f unavailable (NEWLIB_NANO_FORMAT core)
      char txt[32];
      snprintf_P(txt, sizeof(txt), PSTR("%s c (%u slots)"), pbuf, slots);
      arr.add(txt);
    }

    void addToConfig(JsonObject& root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top["enabled"]             = enabled;
      top["updateIntervalSec"]   = updateIntervalSec;
      top["greenBelow"]          = greenBelowCents;
      top["orangeBelow"]         = orangeBelowCents;
      top["fetchOnlyWhenActive"] = fetchOnlyWhenActive;
    }

    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      bool ok = !top.isNull();
      ok &= getJsonValue(top["enabled"], enabled, false);
      ok &= getJsonValue(top["updateIntervalSec"], updateIntervalSec, 60);
      ok &= getJsonValue(top["greenBelow"], greenBelowCents, 8.0f);
      ok &= getJsonValue(top["orangeBelow"], orangeBelowCents, 15.0f);
      ok &= getJsonValue(top["fetchOnlyWhenActive"], fetchOnlyWhenActive, true);
      if (updateIntervalSec < 30) updateIntervalSec = 30; // be kind to the API
      if (orangeBelowCents <= greenBelowCents) orangeBelowCents = greenBelowCents + 1.0f;
      s_greenBelow = greenBelowCents;
      s_orangeBelow = orangeBelowCents;
      return ok;
    }

    uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }
};
const char ComedPricingUsermod::_name[] PROGMEM = "ComEd Pricing";

static ComedPricingUsermod comed_pricing;
REGISTER_USERMOD(comed_pricing);
