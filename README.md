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

## Bluetooth pointing-device recovery

On some macOS systems, starting FreeTheWheel can temporarily make a previously connected Bluetooth mouse or trackpad unresponsive.

For Cristian's setup, FreeTheWheel can automatically reconnect the configured Apple Magic Trackpad after the native DFGT has been detected. It uses `blueutil` and targets only the paired Trackpad address configured in `main.cpp`; it does not disconnect the Magic Keyboard or toggle Bluetooth globally.

Install blueutil with Homebrew if it is not already installed:

```sh
brew install blueutil
```

The program checks the standard Homebrew locations `/opt/homebrew/bin/blueutil` and `/usr/local/bin/blueutil`. If it cannot find blueutil, wheel setup and the FFB service continue normally and the Trackpad reconnection is skipped.

This automatic reconnection is specific to the configured Trackpad address. Other users should change that address in `main.cpp` or leave the feature disabled.

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

Automatic output is deliberately limited to `FORCE 16`, with filtering and a slew-rate limiter. This implementation is experimental and must be tested on the target wheel before wider use. Existing wheel configuration, 900-degree range, buttons, pedals, and the process-lifetime behaviour are intended to remain unchanged.
