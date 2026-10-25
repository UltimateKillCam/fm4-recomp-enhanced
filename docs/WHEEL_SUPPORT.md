# Wheel and controller support

Modified 2026-10-23. GPL-3.0-only. Original project notices are retained.

Both editions combine Windows DirectInput wheels, SDL gamepads and keyboard
menu input. Open **FM4 Launcher.cmd** to bind controls and adjust feedback.
**Save** retains settings; **Save and play** launches the default build.
For a named build workspace, save first and use the command in
[Build notes](../BUILD-NOTES.md#build-and-launch).

## Devices and bindings

Install the manufacturer's PC driver and use PC mode. Automatic discovery
prefers force-feedback driving devices, then devices with actuator axes.
Fanatec, Logitech, Thrustmaster, MOZA, Simucube, Simagic, Asetek and VRS devices
are interface targets, not a verified model list. Fanatec input ranges have
been measured; other models need their own calibration.

In **Controls**, select **Bind** and operate the requested control. Steering
calibration uses centre and right turn; pedals use full press and release.
Each action can select its own device, allowing separate USB pedals,
handbrakes, shifters and button boxes. Binding an already-used axis unbinds
the other action. The force-feedback device can be chosen separately.
The read-only `fm4-input.exe`, built with the game, supplies live values and
does not play effects or take devices exclusively.

Advanced settings are in `.local/user/wheel.ini`, created on first game launch.
The launcher retains custom fields. Restart the game after editing it.
Invalid settings disable the custom driver and report an error in `fm4.log`.

| Setting | Meaning |
| --- | --- |
| Device selector | Empty selects the wheel automatically; use a device name or instance GUID for a particular interface. |
| Axis selector | Driver name, zero-based `#N`, or `X`, `Y`, `Z`, `RX`, `RY`, `RZ`, `Slider0`, `Slider1`. |
| `minimum`, `center`, `maximum` | Raw calibration endpoints; pedals do not use `center`. |
| `invert`, `deadzone`, `saturation`, `gamma` | Direction, neutral range, full-output point and response curve. |
| `[buttons] bitN`, `deviceN` | One-based DirectInput button binding and optional per-button device; zero disables the binding. POV maps to D-pad. |
| `[shifter] enabled`, `device`, `gears` | Enables an H-pattern device and sets its gear count (1..8). Bind `reverse` and `gear1` through `gear8`; no or conflicting contacts report neutral. |
| `[wheel] rotation_degrees` | Physical range configured in the wheel driver; default 900. |
| `rotation_scale`, `full_rotation` | Steering scaling and optional 1:1 cockpit wheel animation. AI cars and replays retain game animation. |

The default McLaren map follows [Fanatec's PC button map](https://www.fanatec.com/eu/en/explorer/products/steering-wheel/csl-elite-steering-wheel-mclaren-qr2-lite/):
A/B/X/Y = 2/3/1/4, menu/view = 10/9, left/right paddles = 6/5,
LSB/RSB = 12/11. It is not a universal button layout.
Use the game's Difficulty settings to select Automatic, Manual or Manual with Clutch.
Keyboard menu fallback is WASD, Space to confirm, Shift to go back and Escape to pause.

## Force feedback

The bridge decodes FM4's spring, damper, inertia, friction and constant-force
effects. Wheel rumble is translated into steering-axis vibration. Focus loss,
device handoff, disconnect, stale updates and shutdown stop output. A worker
polls at 250 Hz; the default stale-output timeout is 150 ms.

| `[force]` setting | Range / effect |
| --- | --- |
| `enabled`, `gain` | Enable output and set overall strength (0..1); new profiles use 0.4 gain. |
| `mode` | `telemetry` reads full-precision car physics torque; `game` uses the original wheel signal. Telemetry falls back to game torque when unavailable. Neither changes car physics. |
| `backend` | `auto` prefers native effects and falls back to software constant force; `native` disables fallback; `software` selects it explicitly. |
| `invert` | Reverses constant steering torque only; condition coefficients retain their direction. |
| `steering_weight` | 1..5 nonlinear torque emphasis; 1 is linear. Higher values strengthen small forces while preserving full-scale torque. |
| `centering` | 0..1 multiplier for the artificial spring; default 0. |
| `damping` | 0..2 multiplier for the game's damper; default 0.25. |
| `road` | 0..1 front-wheel load-derived bump detail in telemetry mode; default 0.3. |
| `detail` | 0..1 strength of game motor signals as steering vibration; default 0.5. |
| `engine` | 0..1 sustained-vibration level relative to transient detail; default 0.1. Sustained kerb signals can also be reduced. |
| `velocity_scale`, `acceleration_scale` | Software condition normalization in (0,10]; defaults 0.1 and 0.01. Feel varies by driver. |
| `timeout_ms` | Maximum age of a force update before the watchdog stops output. |

The launcher expresses gains as percentages. Existing explicit settings take
precedence. Physical switches, power state and driver support can prevent output.
Vendor displays, LEDs, Fanatec FullForce and proprietary rim/pedal vibration
are not implemented.

## Controllers and switching

A deliberate button press or axis movement selects the device. Inactive held
controls are ignored; disconnect falls back to a remaining device. New input
is required after reconnect. Controllers are preferred at startup when present.
Switching refreshes FM4's wheel/controller layout and stops the previous device's feedback.

DualSense uses SDL's USB/Bluetooth HIDAPI backend without an Xbox emulation utility.
Cross/Circle/Square/Triangle map to A/B/X/Y, Options to Start, Create and
touchpad click to Back, and L2/R2 to brake/throttle. FM4 shows Xbox button labels.
Adaptive resistance maps FM4's original vibration signals to trigger resistance;
it is not PS5 game telemetry. The packet format follows
[SDL's controller example](https://github.com/libsdl-org/SDL/blob/main/test/testcontroller.c).

| `[controller]` setting | Effect |
| --- | --- |
| `rumble_gain` | Vibration strength, 0..1; default 1. |
| `stick_deadzone` | Drift suppression, 0..0.5; default 0.06. |
| `adaptive_triggers` | Enables trigger resistance; 0 restores normal travel. |
| `adaptive_gain` | Resistance strength, 0..1; default 0.35. |
| `touchpad_back` | Maps touchpad click to Back. |

Feedback clears on focus loss, handoff and shutdown, with a 250 ms controller
watchdog. Gyro steering and audio-driven PS5 HD haptics are not implemented.

## Diagnostics and limitations

`FM4_TRACE_INPUT=1` enables periodic input logging.

The run scripts support hidden, muted diagnostics with `-Background`,
`-DiagnosticInput <file>` and `-RunSeconds <limit>`. Background mode disables
physical wheel/controller output and omits keyboard/mouse capture.

Physical force direction and feel, physical device handoff, H-pattern hardware,
Bluetooth behavior and adaptive-trigger feel remain incompletely verified.
See [Current status](STATUS.md) for the game's rendering and frame-pacing limits.
