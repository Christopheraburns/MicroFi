// liveness_led.h
//
// Agent-liveness LED strobe: a tiny background task that blinks the onboard
// user LED (GPIO 21 on the XIAO ESP32-S3, active-low) for as long as the
// agent is up. Started last in app_main, after every fatal-init gate, so a
// blinking LED genuinely means "the agent booted all the way" — the factory
// firmware lit this LED and MicroFi historically left it dark (#171).
//
// The red LED next to it is the BQ25101 charge indicator with no MCU
// connection — hardware decides that one; firmware cannot.

#pragma once

#include "microfi/types.h"

namespace microfi {

// Spawn the strobe task. Returns OutOfMemory if the task can't be created.
// Failure should be logged as a warning, never treated as fatal — a missing
// blinker must not prevent boot.
Status liveness_led_start();

}  // namespace microfi
