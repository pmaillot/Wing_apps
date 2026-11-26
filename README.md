# Wing_apps
**Applications for WING, using wapi or OSC**

This repository regroups a collection of applications aimed at remote-controlling a WING mixing console from Music-Group/Behringer, either using the **wapi** library (see https://github/pmaillot/wapi) or **OSC**. 
Both protocols are fully detailed in the **Wing Remote Protocols document** I write and maintain with the authorization of Music Group, and can be found at https://drive.google.com/file/d/1-iptgd2Uxw4qPEbmegG2Sqccf8AbRRfk/view?usp=drive_link

Compiling wapi-based application is typically done with the following script, assuming you are using gcc for compiler:
```
gcc -O3 -Wall -I<wapi include dir> -o <file>.exe <file>.c -L<wapi lib dir> -lwapi -lws2_32 -lgdi32 -mwindows
```

 --------------------------------------------------------------------------------------
# Applications:

## wpanel v 0.1.0 - (c) 2026 - Patrick-Gilles Maillot

A WING panel interface you can use as a remote surface or control tool for your mixing needs; It provides the standard WING strips in a computer window,  interfacing with a real console over the network (automatically finding the console), and providing pre/post fader meter/dyn/gate feedback as well as Select/Solo/Mute/Fader controls in both directions.
WING banks can be selected, adjusting the application window size to minimize screen space use. DCA and FX controls are not part of the app at this time.
Built on the C `wapi` library.

### Build (MinGW-w64)

`wpanel.c` includes `../wapi/wapi.h` and `../wapi/wext.h`, so keep `wapi.h`, `wext.h` and `libwapi.a` in a `wapi` folder next to the `wpanel` folder, then from inside `wpanel`:

```
gcc -O3 -Wall -o wpanel.exe wpanel.c -L../wapi -lwapi -lws2_32 -lgdi32 -mwindows
```

### Use

- **Finding the console:** leave the address box empty and press **Connect** (or **Find**; the app also does this at start-up). The app calls `wOpen("")`, which searches the local network, connects to the first WING that answers and returns its IP address, which is then shown in the box. You can also type an address yourself.
- **Banks:** the vertical buttons at the left choose what the strips show; the window width follows the number of strips: CH 1-12, CH 13-24, CH 25-36, CH 37-40, AUX 1-8, BUS 1-8, BUS 9-16, MAIN 1-4 or MTX 1-8. All have the same controls (select, solo, mute, fader, name, meters).
- **SELECT**: selects that source on the console. The source currently selected on the console is highlighted and named in the top bar, even if it is in another bank.
- **SOLO** / **MUTE**: click to toggle (MUTE turns red when muted).
- **Fader**: drag the handle, or click the track to jump there; double-click for 0 dB.
- **Name**: double-click the scribble strip to rename (Enter or click away to apply, Esc to cancel; 16 characters max).

Everything is two-way: changes made on the WING appear in the app, and changes made in the app are sent to the WING.

**Link to source:** channels and aux strips show a small padlock at the upper right of the scribble strip: closed = linked to the source, open = not linked (the console's `clink` setting; `CLINK_LINKED` in the source sets which value counts as linked). Double-click the padlock to toggle it. This has an impact on the color and name displayed in the scribble.

### Meters

Each strip shows input L/R level LEDs (CLIP, -6, -12, -18, -30, -60 dB) plus DYN and GATE gain-reduction indicators, fed by the wapi metering API (UDP, renewed every 4 s). Only the 12 strips on screen are requested. The **Meters: PRE / POST** button in the top bar switches between the input level (before the fader) and the output level (after it). The CLIP LED stays lit for 500 ms after a peak.

**DYN / GATE:** the DYN and GATE labels are yellow when that processor is switched on and grey when it is off; click a label to switch the processor (changes made on the WING appear here too). The red LED next to it lights while the console reports the gate closed / the dynamics acting (gate LED and dyn state meters), and the bar shows the gain reduction. Only input channels have a gate, so GATE is blank on AUX, BUS, MAIN and MTX strips.

### ClrSolo

A button that can be used to clear all currently enabled SOLO states

### Colours

The scribble-strip colours use the console' scribble LED state and 18 colour indices (RGB values sampled from the colour table in the WING Remote Protocols document, p. 249), in the `WCOL` table near the top of `wpanel.c`.

This app has been co-written with the help of ClaudeAI, as an exercise to show AI is fully capable of grasping the Wing Remote Protocols document, and effectively extract methods and API protocol for using wapi, creating a working application without having to own/use a compiler on your PC. The app development spreaded over several days of adjusting and tweaking, ensuring Claude had a correct understanding of what I wanted to achieve. Claude's approach to WIN32 (GUI portions of the application) is different from what I would have coded, but is quite effective and so much faster than me doing it!
I had to help Claude with fully understanding how to address metering, especially as it was originally using an older (and no longer accurate) version of the Wing Remote Protocols document.

--------------------------------------------------------------------------------------
