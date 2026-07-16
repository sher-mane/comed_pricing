# ComEd Pricing usermod for WLED

Polls the [ComEd hourly-pricing API](https://hourlypricing.comed.com/hp-api/) 5-minute feed and adds a 2D-only effect named **ComedPricing** that renders recent prices as a bar graph on an LED matrix:

- One column per 5-minute time slot; the newest price is the rightmost column, older prices push left. Up to 32 slots are shown (fewer if the matrix is narrower).
- If the feed skipped a 5-minute reading, that slot's column stays **unlit** (a gap).
- Bar height scales the price from 0¢ to the configured max (default 100¢ = $1); prices at or above the max clamp to full height. Negative prices render as a 1-pixel floor.
- Bar color: green at/below the green threshold (default 14¢), then ramps green → yellow → orange → red up to the max price, and **dark red** at/above the max.

Requires a 2D matrix setup (Config → LED Preferences → 2D). On a 1D segment the effect falls back to solid color.

## Installation

This is an external usermod (based on [wled-usermod-example](https://github.com/wled/wled-usermod-example)). Clone it next to your WLED checkout, then add it to your build environment's `lib_deps` in `platformio_override.ini`:

```ini
[env:esp32s3_4M_qspi]
lib_deps = ${esp32s3.lib_deps}
  symlink://C:/Users/you/path/to/wled-usermod-comed-pricing
```

Note: in current WLED nightlies, external paths in `custom_usermods` are **not** supported by `pio-scripts/load_usermods.py` — the `lib_deps` + `symlink://` form above is the working mechanism. The library name in `library.json` must keep its `wled-` prefix so the build treats it as a WLED module.

ESP32 (or newer variants) only: the ComEd API is HTTPS-only, which requires `WiFiClientSecure`.

## Configuration (Config → Usermods → "ComEd Pricing")

| Setting | Default | Meaning |
|---|---|---|
| `enabled` | off | Master switch for API polling. The effect is always registered but shows nothing until data arrives. |
| `updateIntervalSec` | 60 | Seconds between fetches (floor 30). |
| `greenThreshold` | 14 | Price in cents at/below which bars are pure green. |
| `maxPrice` | 100 | Price in cents mapped to full bar height / dark red. |
| `fetchOnlyWhenActive` | on | Only poll while the ComedPricing effect has rendered within the last 70 s. |

Current price and slot count are shown in the UI **Info** panel.

## Notes

- Each fetch performs a blocking TLS handshake (~1–3 s), which briefly freezes animations. Since the bar graph is static between updates, this is invisible while the effect itself is showing — `fetchOnlyWhenActive` keeps it that way. Raise `updateIntervalSec` if you run other animations alongside.
- Certificate validation is skipped (`setInsecure()`); the data is a public price feed.
- The response is parsed with a bounded stream scan (at most ~32 entries of the ~13 KB / 24 h body are read); no JSON document is allocated.

Data source: ComEd Hourly Pricing program, https://hourlypricing.comed.com/hp-api/ (5-minute feed, prices in ¢/kWh).
