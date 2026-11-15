# TPS43 touchpad on 0.7 mm glass

The Svalboard touchpad is an Azoteq TPS43 (IQS572-B000). Its stored settings assume a
1 mm plastic overlay. On the 0.7 mm glass it is laminated to, touch deltas roughly double
(700-800 counts for an 8 mm touch), so the stock touch thresholds fire on noise and thermal
drift. Azoteq applications engineering characterized a TPS43 bonded to 0.7 mm glass and
supplied these settings on 2026-04-24:

| Register | Setting | Stock | Glass | Why |
|---|---|---|---|---|
| 0x0596 | Global touch multiplier, set | ~16 (87 counts) | **32** (175 counts) | glass gives ~2x deltas |
| 0x0597 | Global touch multiplier, clear | ~12 (65 counts) | **24** (131 counts) | hysteresis scaled with it |
| 0x0580 | LP1 report rate | 80 ms | **50 ms** | the ALP channel tracks the environment better than the TP channels |
| 0x0586 | Idle mode timeout | 10 s | 2 s suggested; **255 (never) shipped** | power-saving modes are off by default, so the pad never enters LP1 |

## How the firmware applies them

`azoteq/azoteq.c` writes the four registers at run time after every start of the chip: at
boot, when the pointing search finds the pad later, and when the native touchpad path
(`azoteq/multitouch.c`) starts a chip again after it reported a reset. Each start resets the
chip, which returns its runtime registers to the stored settings, and the driver then sets the
idle timeout to 255 s, so the values are written after it every time.

The firmware never writes the chip's stored settings image.

Override any value in `config.h`: `SVAL_IQS_TOUCH_MULT_SET`, `SVAL_IQS_TOUCH_MULT_CLEAR`,
`SVAL_IQS_LP1_REPORT_RATE_MS`, `SVAL_IQS_IDLE_MODE_TIMEOUT_S` (default 255, the driver's
"never enter LP1"; 2 opts in to Azoteq's low-power timing).

## Pad identity

About 5 s after boot each half prints its pad's identity on the console once:

```
IQS5XX right pad: product <n> project <n> version <major>.<minor> settings export version <n>
```

The settings export version (register 0x0677) shows whether a pad's stored settings were ever
rewritten. Only the half with USB is readable on the console.
