# Free The Wheel for macOS

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

## Important: Mouse behaviour

FreeTheWheel may make a pointing device that was already connected before launch temporarily unusable.

For a Bluetooth mouse or trackpad (for example, an Apple Magic Trackpad), you may be able to restore operation by switching the device off and back on after starting FreeTheWheel. This behaviour has been observed on macOS but may depend on the device.

If that does not work, connect a separate USB mouse after launching FreeTheWheel.

## How to use it

1. Connect the Logitech steering wheel.
2. Run `./FreeTheWheel` (or double-click the executable).
3. Leave the executable running.
4. If your Bluetooth mouse or trackpad stops responding, switch it off and back on. Alternatively, connect a USB mouse after launching FreeTheWheel.
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
