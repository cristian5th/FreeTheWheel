# FreeTheWheel for macOS

A modified version of FreeTheWheel adapted to work on Apple Silicon Macs, including M1-based systems.

Logitech steering wheel enabler based on [Feral Interactive's FreeTheWheel](http://support.feralinteractive.com/en/faqs/free_the_wheel/).

## macOS Catalina and later

This is a fork of [jackhumbert's FreeTheWheel](https://github.com/jackhumbert/FreeTheWheel), modified to work on newer versions of macOS and Apple Silicon.

The following steering wheels are supported:

- Logitech G27
- Logitech Driving Force GT (DFGT)

## Features for the Logitech Driving Force GT

- Full 900° steering rotation.
- All steering wheel buttons enabled and working.
- Steering wheel buttons and pedal commands are passed through to the game while FreeTheWheel is running.

## Important: FreeTheWheel must remain running

**Do not close FreeTheWheel after launching it.**

FreeTheWheel must remain running in the background throughout the gaming session. If the process is terminated, the game will no longer receive the steering wheel commands.

The executable waits for you to press ENTER before exiting.

## Bluetooth Magic Trackpad reconnection

Starting FreeTheWheel can make the configured Bluetooth pointing device temporarily unresponsive on some macOS systems. To recover it, FreeTheWheel automatically disconnects and reconnects **only the configured Apple Magic Trackpad** after detecting the native Logitech Driving Force GT.

**Requirement: `blueutil` must be installed for automatic Trackpad reconnection to work.** Install it with Homebrew:

```sh
brew install blueutil
```

FreeTheWheel looks for `blueutil` at `/opt/homebrew/bin/blueutil` or `/usr/local/bin/blueutil`. If it is not installed, or is not found at either location, the Trackpad reconnection is skipped; wheel setup and the FFB service can still continue.

**Important limitation:** this feature is configured specifically for one Apple Magic Trackpad Bluetooth address (set in `main.cpp`). It is not a general Bluetooth mouse/trackpad recovery feature and will not reconnect other pointing devices. The Magic Keyboard and other Bluetooth devices are not intentionally disconnected or modified. Anyone using a different Trackpad must replace the configured address in `main.cpp`.

## How to use it

1. Connect the Logitech steering wheel.
2. Run `./FreeTheWheel` (or double-click the executable).
3. Allow the configured Magic Trackpad reconnection to complete, if enabled and blueutil is installed.
4. Leave FreeTheWheel running.
5. Start your game.

You must repeat this procedure whenever you restart your Mac or reconnect the steering wheel.

## How to compile

Assuming you have a development environment installed, run `make`.

## Credits

Based on [Feral Interactive's FreeTheWheel](http://support.feralinteractive.com/en/faqs/free_the_wheel/) and [jackhumbert's FreeTheWheel](https://github.com/jackhumbert/FreeTheWheel).

## Force-feedback integration (development)

The development branch includes an experimental synthetic force-feedback mode driven by ETS2/ATS telemetry over a same-user Unix-domain socket.

Socket path:
`~/Library/Application Support/FreeTheWheel/ffb.sock`

The IPC protocol supports `PING`, `STATUS`, `FORCE n`, `STOP`, and `AUTO_FFB ON|OFF`. Automatic force feedback currently derives a steering-centering effect from the game's steering telemetry; it is not yet a reproduction of the simulator's full tyre/road forces.

Automatic output is capped at `FORCE 28` (manual `FORCE` commands remain capped at 32). The synthetic model uses a deadband-compensated steering-centering spring, steering-rate damping, filtering, and a slew-rate limiter. Automatic FFB remains disabled by default and must be enabled explicitly with `AUTO_FFB ON`. This implementation is experimental and must be tested cautiously on the target wheel before wider use. Existing wheel configuration, 900-degree range, buttons, pedals, and process-lifetime behaviour are intended to remain unchanged.
