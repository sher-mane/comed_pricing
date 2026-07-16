#include "wled.h"
#include <SSLClient.h>
#include "trust_anchors.h"

/*
 * ComEd Pricing usermod
 *
 * Polls the ComEd hourly-pricing 5-minute feed (https://hourlypricing.comed.com/hp-api/)
 * and registers a 2D-only effect "ComedPricing" that renders the most recent prices as a
 * bar graph: one column per 5-minute slot, newest slot on the right. Slots with no
 * published price stay unlit. Bar color ramps green -> yellow -> orange -> red between
 * the configured green threshold and max price; prices at/above max clamp to full
 * height in dark red.
 */

#define COMED_MAX_SLOTS 32
#define COMED_SLOT_MS   300000ULL  // 5 minutes

// Shared between the usermod's loop() (writer) and the effect function (reader).
// Both run sequentially on the main loop task, so no locking is needed.
static float    s_prices[COMED_MAX_SLOTS];   // [COMED_MAX_SLOTS-1]=newest slot ... [0]=oldest; NAN = missing
static bool     s_haveData = false;
static float    s_greenThreshold = 14.0f;    // cents
static float    s_maxPrice = 100.0f;         // cents
static uint32_t s_lastEffectRender = 0;      // millis() of last effect frame

// Local copy of FX.cpp's non-exported static fallback
static void mode_static(void) {
  SEGMENT.fill(SEGCOLOR(0));
}
#define FX_FALLBACK_STATIC { mode_static(); return; }

static uint32_t priceColor(float price) {
  if (price >= s_maxPrice)       return RGBW32(128,   0, 0, 0); // darkest red
  if (price <= s_greenThreshold) return RGBW32(  0, 255, 0, 0); // green
  float t = (price - s_greenThreshold) / (s_maxPrice - s_greenThreshold); // (0,1)
  uint8_t r, g;
  if (t < 0.5f) { r = (uint8_t)(510.0f * t); g = 255; }                    // green -> yellow
  else { r = 255; g = (uint8_t)(255.0f - 510.0f * (t - 0.5f)); }           // yellow -> orange -> red
  return RGBW32(r, g, 0, 0);
}

static void mode_comed_pricing(void) {
  if (!strip.isMatrix || !SEGMENT.is2D()) FX_FALLBACK_STATIC; // not a 2D set-up
  s_lastEffectRender = millis();
  const int cols = SEG_W, rows = SEG_H;
  SEGMENT.fill(BLACK);
  if (!s_haveData) return;

  int shown = min((int)COMED_MAX_SLOTS, cols);
  for (int k = 0; k < shown; k++) {         // k=0 -> newest slot -> rightmost column
    float price = s_prices[COMED_MAX_SLOTS - 1 - k];
    if (isnan(price)) continue;             // missing 5-min price -> unlit gap column
    int x = cols - 1 - k;
    float frac = price / s_maxPrice;
    if (frac > 1.0f) frac = 1.0f;
    if (frac < 0.0f) frac = 0.0f;           // negative prices clamp to 0
    int h = (int)roundf(frac * rows);
    if (h < 1) h = 1;                       // 1px floor so the column stays visible
    uint32_t c = priceColor(price);
    for (int y = 0; y < h; y++) SEGMENT.setPixelColorXY(x, rows - 1 - y, c);
  }
}
static const char _data_FX_MODE_COMED_PRICING[] PROGMEM = "ComedPricing@;;;2;";

class ComedPricingUsermod : public Usermod {
  private:
    bool     enabled = false;
    uint16_t updateIntervalSec = 60;
    float    greenThresholdCents = 14.0f;
    float    maxPriceCents = 100.0f;
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
      s_haveData = true;
      return true;
    }

  public:
    void setup() override {
      strip.addEffect(255, &mode_comed_pricing, _data_FX_MODE_COMED_PRICING);
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
      char txt[32];
      snprintf_P(txt, sizeof(txt), PSTR("%.1f c (%u slots)"), s_prices[COMED_MAX_SLOTS-1], slots);
      arr.add(txt);
    }

    void addToConfig(JsonObject& root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top["enabled"]             = enabled;
      top["updateIntervalSec"]   = updateIntervalSec;
      top["greenThreshold"]      = greenThresholdCents;
      top["maxPrice"]            = maxPriceCents;
      top["fetchOnlyWhenActive"] = fetchOnlyWhenActive;
    }

    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      bool ok = !top.isNull();
      ok &= getJsonValue(top["enabled"], enabled, false);
      ok &= getJsonValue(top["updateIntervalSec"], updateIntervalSec, 60);
      ok &= getJsonValue(top["greenThreshold"], greenThresholdCents, 14.0f);
      ok &= getJsonValue(top["maxPrice"], maxPriceCents, 100.0f);
      ok &= getJsonValue(top["fetchOnlyWhenActive"], fetchOnlyWhenActive, true);
      if (updateIntervalSec < 30) updateIntervalSec = 30; // be kind to the API
      if (maxPriceCents <= greenThresholdCents + 1.0f) maxPriceCents = greenThresholdCents + 1.0f;
      s_greenThreshold = greenThresholdCents;
      s_maxPrice = maxPriceCents;
      return ok;
    }

    uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }
};
const char ComedPricingUsermod::_name[] PROGMEM = "ComEd Pricing";

static ComedPricingUsermod comed_pricing;
REGISTER_USERMOD(comed_pricing);
