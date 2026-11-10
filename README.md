# GHL-PS4onPS3

A plugin for PS3 consoles running CFW that lets you play **Guitar Hero Live on
PS3** with the **PS4 guitar and dongle** (USB `1430:07bb`).

Without it the game stops at *"Press any button on guitar controller"*: it only
recognizes the PS3 dongle (`12ba:074b`).

## How it works

The game talks to the dongle directly through `libusbd`, registering its own
driver (`GH_Guitar`) for vendor `0x12BA`. That driver's probe and attach
callbacks never check the vendor or product id, only that the device is HID with
one IN and one OUT interrupt endpoint, which the PS4 dongle also is. Loaded into
the game, the plugin:

1. registers **the game's own callbacks** for `1430:07bb` as well;
2. replaces the PS3 keep-alive with the PS4 one (`SET_REPORT 0x30`) and repeats
   it every 8 s;
3. converts each 64-byte PS4 report into the 27-byte PS3 layout.

`cellUsbdOpenPipe`, `cellUsbdControlTransfer` and `cellUsbdInterruptTransfer`
are intercepted by swapping their function descriptors (OPD), the same technique
ds45pad uses.

## Compatibility

- **Game:** Guitar Hero Live, `BLUS31556`, with update 1.07. The plugin uses a
  fixed address in that executable and checks its signature first; on any other
  version it only writes `game_ops_not_found_wrong_eboot` to the log and does
  nothing.
- **Console:** CFW with Cobra and webMAN MOD (PS3MAPI). Tested on Evilnat 4.91.2.

## Mapping

| Guitar | PS4 dongle | PS3 dongle |
|---|---|---|
| White 1, 2, 3 | □, L1, R1 | □, L1, R1 |
| Black 1, 2, 3 | ×, ○, △ | ×, ○, △ |
| Strum bar | left stick Y | left stick Y |
| Whammy | right stick Y | right stick Y |
| Tilt | right stick X | accelerometer X |
| Hero Power | R3 | Select |
| Pause | Options | Start |
| GHTV | L3 | L3 |
| Center button | PS | PS |

Tested on hardware: frets, strum bar, tilt, Hero Power, Pause, GHTV and the PS
button (opens the XMB). The whammy follows the documented layout but could not
be tested: on the guitar used for testing the bar sends no signal, not even on a
PC.

## Installation

1. Download `ghl_ps4onps3.sprx` from [Releases](https://github.com/higorhgon/GHL-PS4onPS3/releases)
   and copy it to `/dev_hdd0/plugins/ghl/`.
2. Load the plugin **while the game is running**, either:
   - **Manually:** in a browser, `http://PS3-IP/gameplugin.ps3mapi` → the game's
     process → path `/dev_hdd0/plugins/ghl/ghl_ps4onps3.sprx` → **Load**.
   - **Automatically:** with [ds45pad 2.0.7](https://github.com/higorhgon/DualSense-PS3)
     or newer (this fork), create `/dev_hdd0/plugins/ds45pad/game_plugins.txt`:
     ```
     BLUS31556 /dev_hdd0/plugins/ghl/ghl_ps4onps3.sprx GHL PS4 guitar ready
     ```
     The plugin is loaded about 15 s after the game starts, and the (optional)
     message at the end of the line is shown as a notification.
3. A dongle that is already plugged in is picked up as soon as the plugin loads.

The log is written to `/dev_hdd0/tmp/ghl_ps4onps3.txt`.

## Building

Uses the Cell SDK (4.75) under Wine plus `scetool`, like ds45pad:

```
src/build_wine.sh          # normal build
src/build_wine.sh debug    # also logs unknown report bytes
```

The normal build writes nothing to the HDD during gameplay, only when it loads
and when the dongle connects.

## Credits and references

- [PlasticBand](https://github.com/TheNathannator/PlasticBand): documentation of
  the PS3 and PS4 dongle reports.
- [RPCS3](https://github.com/RPCS3/rpcs3): emulation of the PS3 dongle (`GHLtar.cpp`).
- Linux `hid-sony` driver: the dongles' keep-alive packets.
- [ds45pad / DualSense-PS3](https://github.com/domyno6773/DualSense-PS3) by domyno6773:
  the minimal `libc.c` and the hooking technique.

## License

GPL-2.0, the same as ds45pad, where `libc.c` comes from.
