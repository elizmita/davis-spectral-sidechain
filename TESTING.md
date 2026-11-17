# Testing DAVIS Spectral Sidechain

## Offline (before the device)
```bash
MV=../mpc-vst-plugins
g++ -std=c++17 -O2 -I$MV/wrapper tests/test.cpp src/engine.cpp -o /tmp/kd_test -lm && /tmp/kd_test     # engine: "all passed"
$MV/tools/test_port.sh vst/vst.json                                                                    # wrapper: must print PASSED
python3 $MV/tools/gen_vst.py vst/vst.json                                                              # skin: no "warning: skin:" lines
python3 $MV/tools/catalog_check.py dist/Spectral-Sidechain-<version>-mpc-armv7.zip --catalog         # catalog rules: OK
```
Engine checks: cut and lift at the band while 300 Hz stays put (either processing order of the two tracks), ratio,
threshold, Amount cap, -60 dB and 21:1 (full cut), knob ends reading real values, no clicks under a fast full cut, float path equal
to int16 and passing +6 dBFS, no kick / no SEND leaves the bass intact, a hi-hat lift on the kick that neither flutters nor clips, the graph ignoring touches,
curve / live curve / dot, the SC view (bars on the kick's body, none when off), and OUT (off draws nothing, on peaks on a tone's column at the right level, kept in the state,
cleared when off and in SEND).

## On the MPC
| # | Do | Expect |
|---|----|--------|
| 1 | Insert Spectral Sidechain on the kick track: SEND, A | Plugin loads, kick unchanged; the rest of the screen hidden, red frame round SEND and A-D |
| 2 | Insert it on the bass track: RECEIVE, A; play both | "SC OK" |
| 3 | SC on, Freq by the slider onto the grey bars, Amount -12 dB | GR dips on every kick, the red curve and the dot move |
| 4 | Touch and drag anywhere on the graph (OUT aside) | Nothing moves: the band stays where the slider and knobs put it |
| 5 | OUT on | Green bars of the output spectrum, a hole where the kick hits; CPU meter barely moves |
| 6 | Mute the kick track | "NO SC", the bass stays untouched, no grey bars |
| 7 | Two RECEIVEs on one track (A and B), a SEND B on another track | Both bands act independently |
| 8 | Save the project, reload it | Every setting (OUT too) comes back |
| 9 | SC button on, then off (OUT and SC independent) | Grey bars of the kick appear behind the curves on each hit and are down before the next one; off, none are drawn |
| 10 | Touch each Q-Link | Its name reads FREQ, Q, RATIO, THRESH, ATK, REL, AMOUNT |
| 11 | Amount at +6 dB on a hi-hat at 8 kHz, SC from the kick | The hat gets louder on each kick, clean, no chirp |

Tested on: _model, MPC OS version, date_ (fill in before asking for a catalog listing).
