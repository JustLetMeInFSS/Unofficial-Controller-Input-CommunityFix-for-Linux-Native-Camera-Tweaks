# Controller input parity analysis

Reference revisions:

- Windows: `ersh1/BG3_NativeCameraTweaks` at `862222b6e68d863841724cb86b2492a9071255a9` (2.4.5)
- Linux: `0x1496FD0/Baldur-s-Gate-3-Linux-Native-Camera-Tweaks` at `18ee4b53b15e2b5304bc2df76fe70e18718b09c9` (1.0.21)

## Root causes

### Premature SDL queue termination

The original Linux interposer returned `0` from `SDL_PollEvent` whenever it
consumed a right-stick-Y or mouse-wheel event. Applications conventionally use
`while (SDL_PollEvent(&event))`; therefore BG3 was told that the entire queue was
empty even when more events were waiting behind the consumed event. Frequent
axis-motion events could be left for later frames and accumulate into perceived
input lag.

The patched interposer keeps polling internally after a consumed event. It only
returns `0` when the real SDL queue is empty, or returns the next event that BG3
must receive.

### Different horizontal and vertical response models

The original Linux controller pitch used:

```text
pitch delta per camera call = raw axis / 32767 * 2 degrees
deadzone                    = 4000 / 32767 = 12.21%
```

That model did not rescale the useful stick range and did not use frame time. At
full deflection it produced approximately 120 degrees/second at 60 FPS, 240 at
120 FPS, and 480 at 240 FPS. Horizontal rotation remained in BG3's native,
time-scaled camera pipeline, so the axes could not feel equal across frame rates.

Windows 2.4.5 first removes and rescales its configured 15% deadzone:

```text
u = sign(x) * (abs(x) - 0.15) / (1 - 0.15)
```

It then calculates controller pitch as:

```text
pitch delta = u * ControllerCameraRotationMult
                * deltaTime * camera.rotationSpeed * ControllerPitchMult
```

With the Windows defaults (`2.0` and `0.5`), this simplifies to:

```text
pitch delta = u * deltaTime * camera.rotationSpeed
```

The patch uses this same base formula and reads `rotationSpeed` from
camera-object offset `0xC4`, matching the Windows 2.4.5 camera layout. Linux
already uses the same layout offsets for zoom (`0x58`) and current pitch
(`0x164`). A monotonic per-camera timer supplies frame time and clamps pauses to
100 ms to prevent a large jump after a stall.

In-game testing showed that this Windows-derived vertical base speed was still
several times faster than the native, unmodified Linux horizontal path. Patch
`input-fix.2` therefore adds `controller_pitch_sensitivity`, defaulting to
`0.25`. This multiplier is applied after the base formula and can be adjusted in
`~/.config/bg3-native-camera-tweaks.conf` without recompiling.

## Additional corrections

- Left-stick click (L3) + right-stick Y zoom is normalized and time-scaled. Its
  15 units/second default preserves the original full-stick speed at 60 FPS
  while removing FPS dependence. A normal L3 click is deferred until release;
  it is forwarded to BG3 if the click was not used as the zoom modifier.
- Controller movement no longer forces SDL relative-mouse mode.
- Multiple mouse-motion and wheel events are accumulated instead of overwriting
  all but the last event.
- Controller state is cleared on device removal or window focus loss.
- Binding access is guarded when the input config could not be loaded.

### Input Fix 3 state corrections

- The Windows camera layout identifies `currentZoomA`, `currentZoomB`, and
  `desiredZoom` at offsets `0x54`, `0x58`, and `0x5C`. Earlier Linux code only
  changed `currentZoomB`, allowing BG3's interpolation to repeatedly correct a
  mismatched target. Each custom zoom step now synchronizes all three values,
  including recovery from an already-jittering state.
- The magnitude of `rotationSpeed` is used for controller pitch. Stick direction
  and `invert_controller_pitch` are now the only sources of pitch direction, so
  a camera-mode sign change cannot invert the axis unexpectedly.
- Mouse Y motion is collected only while the configured mouse-rotate binding is
  active. A held mouse binding takes priority over stale controller-axis state.
- SDL relative-mouse mode is restored only if this mod enabled it. Pre-existing
  BG3 ownership is preserved, preventing changed mouse behavior after switching
  from controller to mouse and keyboard.

### Input Fix 4 portable release corrections

- Per-user paths no longer assume a particular profile name. The mod checks an
  explicit `BG3_INPUT_CONFIG_PATH`, then `XDG_DATA_HOME`, then the standard
  `$HOME/.local/share` BG3 profile tree.
- A missing input config uses middle mouse as the camera-rotate fallback instead
  of aborting initialization.
- The executable hash is now assigned and checked before pattern scanning or
  patching. Unsupported builds pass SDL events through unchanged.
- Compatibility symbol selection and executable mapping lower the binary's
  required glibc version from 2.34 to 2.17.

### Input Fix 5 live-state corrections

- Controller events are transition notifications. If a centering or L3-release
  event is lost during a focus/input-mode transition, an event-only cache can
  retain the previous right-stick value indefinitely. The camera hook now
  reconciles the cached values with SDL's live controller axis and button state.
- A zero mouse-wheel delta previously synchronized all three zoom fields on
  every camera call. That could cancel BG3's own in-progress interpolation even
  when the mod had no zoom input. Zoom fields are now changed only for a real
  custom zoom step.
- The SaveToInputConfigFile call-site hook was only needed for hot-reloading a
  changed mouse binding. It used a build-specific internal function ABI and is
  unnecessary for normal operation, so it has been removed. Bindings are loaded
  once at startup and a restart is required after changing them.

### Controller Input Fix release

- The experimental SDL tactical handoff was discarded because passing raw
  right-stick events back to BG3 broke the L3 modifier behavior.
- Controller pitch, L3 zoom, zoom-state synchronization, and device switching
  therefore remain identical to the tested Input Fix 5 implementation.
- Mouse vertical pitch is no longer hard-coded to `2.0`; the new
  `mouse_pitch_sensitivity` setting defaults to `1.5`.

## Verification

`tests/controller_input_test.c` verifies deadzone endpoints and equal movement
over one second at 60 and 120 FPS. Full validation still requires the supported
native BG3 build because the camera function and object are runtime hooks.

Recommended in-game A/B checks:

1. Hold the right stick at a fixed diagonal and compare the apparent horizontal
   and vertical speed.
2. Repeat at 60, 120, and the display's uncapped/high-refresh frame rate.
3. Flick Y repeatedly while also pressing buttons; button delivery must remain
   immediate instead of being delayed behind axis events.
4. Test left-stick-click + right-stick-Y zoom, a normal L3 click without zoom,
   and controller disconnect/reconnect.
