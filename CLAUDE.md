# Beamhound: notes for coding sessions

IR remote identifier + universal remote app for the M5Stack Cardputer Zero (LVGL 9.5, C++17,
320x170 framebuffer, LIRC IR, evdev keyboard). Published in the CardputerZero Application Store
as package `beamhound` (share code BEAM). Release process: `docs/RELEASING.md` (pushing a
`v*` tag builds, releases and submits to the store via CI; no manual portal upload).

## Build and test

- Dependencies: `git clone --depth 1 --branch v9.5.0 https://github.com/lvgl/lvgl.git deps/lvgl`.
  Tests and demos need an index: `make tools && ./build/host/build_index <Flipper-IRDB checkout> build/data`.
- `make tools test`: decoder tests + matcher simulation. The sim runs two variants, "with remote"
  (sniff POWER, answer questions) and "no remote" (brand code search: step through
  `Matcher::powerCodes()`, then test-send questions only). Keep both at 100% correct (CI enforces it).
- `make desktop` builds a headless preview. `BEAMHOUND_DATA=build/data HOME=<tmp> ./build/desktop/beamhound --demo <dir> [remote-path]`
  writes a PPM screenshot of every screen; `--setup-test <dir>` exercises the first-run download.
  Check UI changes this way (convert with ImageMagick) before deploying.
- `make cross` cross-compiles for the device with Zig (`tools/zig`, fetched via `uv`), targeting
  glibc 2.36 with static libc++. `make package` builds the store `.deb`; `make deploy` installs
  it on the device over ssh (`DEVICE=cardputer`) and restarts APPLaunch.
- Do NOT build on the Cardputer itself: it has 352 MB RAM and crashed twice compiling.

## Layout

- `src/core/`: no UI dependencies. `ir.*` protocol decode/encode (Flipper .ir address/command
  conventions; `canonicalKey` makes NEC/NECext/Pioneer etc. comparable), `irfile.*` parser and
  button-name classifier, `index.*` binary index, `builder.*` index builder (runs on the PC and
  on the device), `matcher.*` question selection.
- `src/platform/hw.*`: LIRC RX (`/dev/lirc1`, mode2, 25 ms timeout) and TX (`/dev/lirc0`),
  keyboard (`/dev/input/by-path/platform-3f804000.i2c-event`). Stubbed when `BH_DESKTOP`.
- `src/app/`: `app.*` screens and key handling; `setup.*` first-run database download.
- `tools/build_index.cpp`, `tests/test_core.cpp`, `tests/sim_matcher.cpp`.

## Conventions

- Version lives only in `app-builder.json`; change it with `tools/bump_version.sh X.Y.Z`.
- IR codes must never be bundled in the repo or the package (licensing): the app downloads
  Flipper-IRDB commit `kIrdbCommit` on first run. Keep it in sync with `IRDB_COMMIT` in CI.
- Data dir on device: `~/.local/share/beamhound/` (`index.bin`, `irdb/`, `profiles.tsv`).
- Any change to what the builder outputs must bump `kBuilderRev` (`src/core/builder.hpp`) so
  installed apps re-index from their local `irdb/` (`--update-test <dir>` checks that path).
- Keyboard: F/Z/X/C are the printed arrow keys (up/left/down/right) and work without Fn in
  lists and on the remote screen. Fn layer sends KEY_UP/LEFT/DOWN/RIGHT, KEY_MUTE/VOLUME*,
  KEY_PLAYPAUSE/PREVIOUSSONG/NEXTSONG, KEY_BRIGHTNESS*. Short Esc = back, long Esc = exit.
- Commit as `Bryan Feller <6282600+bfeller@users.noreply.github.com>` (repo-local git config);
  never put a personal email in commits or package metadata.
