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

While FreeTheWheel is running, the mouse already connected to the computer will become unusable.

To continue using your computer, you must connect a separate USB mouse after launching FreeTheWheel.

## How to use it

1. Connect the Logitech steering wheel.
2. Run `./FreeTheWheel` (or double-click the executable).
3. Leave the executable running.
4. Connect a USB mouse after launching FreeTheWheel.
5. Start your game.

You must repeat this procedure whenever you restart your Mac or reconnect the steering wheel.

## How to compile

Assuming you have a development environment installed, run `make`.

## Credits

Based on [Feral Interactive's FreeTheWheel](http://support.feralinteractive.com/en/faqs/free_the_wheel/) and [jackhumbert's FreeTheWheel](https://github.com/jackhumbert/FreeTheWheel).
