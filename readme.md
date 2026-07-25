# ComEd Pricing usermod for WLED

Polls the [ComEd hourly-pricing API](https://hourlypricing.comed.com/hp-api/) 5-minute feed and adds two 2D-only effects for LED matrices. Both color by the same three hard bands on absolute price:

| Price | Color |
|---|---|
| under 8¢ | 🟢 green |
| 8¢ – 15¢ | 🟠 orange |
| 15¢ and above | 🔴 red |

Both breakpoints are configurable. The bands are half-open, so exactly 8¢ is orange and exactly 15¢ is red.

## Effect: ComedPriceGraph

Bar graph of recent prices:

- One column per 5-minute time slot; the newest price is the rightmost column, older prices push left. Up to 32 slots are shown (fewer if the matrix is narrower).
- If the feed skipped a 5-minute reading, that slot's column stays **unlit** (a gap).
- The Y axis **auto-scales**: every frame the highest price currently on screen is found and bars are scaled against it, so the tallest column always reaches the top row and the graph fills the panel. The bottom stays at 0¢, so heights remain proportional to actual price (a 2.5¢ column next to a 5¢ peak is half height). Negative prices render as a 1-pixel floor.
- Because the scale is relative, the whole graph re-normalizes when the peak in view changes — use the colors, not the heights, to read absolute price.

## Effect: ComedPricingText

Scrolls the newest reading right-to-left like the built-in Scrolling Text effect, formatted as:

```
8:30am: $0.031
```

- The time is the reading's timestamp converted to the device's local timezone, price is in $/kWh with 3 decimals.
- The whole text takes the band color for the current price.
- Sliders: Effect speed (scroll rate), Y Offset, Trail, Font size — same behavior as the built-in Scrolling Text.

Requires a 2D matrix setup (Config → LED Preferences → 2D). On a 1D segment the effect falls back to solid color.

## Installation

Clone this repo into your WLED checkout's `usermods/` folder as `comed_pricing`, then add it to your build environment's usermod list in `platformio_override.ini` (or `platformio.ini`):

```ini
[env:esp32s3_4M_qspi]
custom_usermods = audioreactive user_fx comed_pricing
```

(List whatever usermods your environment already had, plus `comed_pricing` — the override replaces the base value.)

Alternative, if you prefer keeping the repo outside the WLED tree: external paths in `custom_usermods` are **not** supported by `pio-scripts/load_usermods.py`, but the build treats any `lib_deps` library whose name starts with `wled-` as a usermod, so this works instead:

```ini
[env:esp32s3_4M_qspi]
lib_deps = ${esp32s3.lib_deps}
  symlink://C:/Users/you/path/to/wled-usermod-comed-pricing
```

ESP32-family only. The ComEd API is HTTPS-only, and the Tasmota-forked Arduino core WLED builds with ships **no TLS** (no `WiFiClientSecure`; the mbedTLS SSL layer is stripped from the precompiled IDF libraries). This usermod therefore depends on [openslab-osu/SSLClient](https://registry.platformio.org/libraries/openslab-osu/SSLClient) (declared in `library.json`, auto-installed by PlatformIO), which compiles BearSSL from source (~50 KB flash) and runs over the framework's plain `WiFiClient`.

## Configuration (Config → Usermods → "ComEd Pricing")

| Setting | Default | Meaning |
|---|---|---|
| `enabled` | off | Master switch for API polling. The effect is always registered but shows nothing until data arrives. |
| `updateIntervalSec` | 60 | Seconds between fetches (floor 30). |
| `greenBelow` | 8 | Price in cents below which bars are green. |
| `orangeBelow` | 15 | Price in cents below which bars are orange; at/above this they are red. |
| `fetchOnlyWhenActive` | on | Only poll while the ComedPricing effect has rendered within the last 70 s. |

Current price and slot count are shown in the UI **Info** panel.

## Notes

- Each fetch performs a blocking TLS handshake (~1–3 s), which briefly freezes animations. Since the bar graph is static between updates, this is invisible while the effect itself is showing — `fetchOnlyWhenActive` keeps it that way. Raise `updateIntervalSec` if you run other animations alongside.
- The server certificate is validated against the root CAs embedded in `trust_anchors.h` (DigiCert Global Root G2 — ComEd's current root, valid to 2038 — plus DigiCert Global Root CA and ISRG Root X1 as fallbacks). If ComEd ever moves to a different CA, regenerate that header from the new root's PEM (same format as SSLClient's `pycert_bearssl` tool output).
- The response is parsed with a bounded stream scan (at most ~32 entries of the ~13 KB / 24 h body are read); no JSON document is allocated.

Data source: ComEd Hourly Pricing program, https://hourlypricing.comed.com/hp-api/ (5-minute feed, prices in ¢/kWh).
