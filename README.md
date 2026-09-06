# Unofficial Controller Input Community Fix

An unofficial controller, mouse-input, stability, and portability patch for
[BG3 Linux Native Camera Tweaks](https://www.nexusmods.com/baldursgate3/mods/23896).

The original mod and camera implementation were created by **Biiinks78 / 0x1496FD0**.
This fork focuses on making the native Linux version responsive, stable, and
portable across user installations.

![Version](https://img.shields.io/badge/version-1.0.22--community--input--fix-blue)
![Platform](https://img.shields.io/badge/platform-Linux-green)
![Architecture](https://img.shields.io/badge/architecture-x86--64-lightgrey)

## What this patch changes

- Fixes controller input delay and unstable input switching.
- Adds frame-rate-independent controller pitch and L3 + right-stick zoom.
- Fixes persistent zoom jitter while retaining BG3’s native zoom limits.
- Supports configurable sensitivity, zoom direction, and mouse-camera speed.
- Reloads changed mouse-rotation bindings without restarting the game.
- Adds portable paths, safer pattern validation, and broader build compatibility.

The controller response model is based on the deadzone and time-scaled pitch
formula used by
[BG3 Native Camera Tweaks for Windows](https://github.com/ersh1/BG3_NativeCameraTweaks).
Technical details and tests are documented in
[`CONTROLLER_INPUT_ANALYSIS.md`](CONTROLLER_INPUT_ANALYSIS.md).

## Game-build compatibility

This release was tested against the native Linux versions:

- `4.1.1.7209685`
- `4.1.1.7398727`

Other builds are not rejected by hash alone. The mod continues only if each
camera pattern has exactly one match, the camera call is a valid `CALL rel32`,
and the pitch instruction has the expected opcode and object offset. If any
check fails, camera modifications remain inactive and SDL input is passed
through normally.

## Installation

1. Download `linux_native_camera_tweaks.so`.
2. Place it in a permanent location without spaces, for example:

   ```text
   /home/your-user/Mods/BG3/linux_native_camera_tweaks.so
   ```

3. Add the absolute path to BG3's Steam launch options:

   ```text
   LD_PRELOAD=/home/your-user/Mods/BG3/linux_native_camera_tweaks.so %command%
   ```

4. Start the native Linux version of BG3.

No installer, administrator access, or manual configuration is required.

## Configuration

On first launch, the mod creates:

```text
${XDG_CONFIG_HOME:-$HOME/.config}/bg3-native-camera-tweaks.conf
```

Default settings:

```ini
controller_pitch_sensitivity=0.25
controller_zoom_speed=15.00
mouse_pitch_sensitivity=1.50
invert_controller_pitch=false
invert_controller_zoom=false
```

Close BG3 before editing the file and restart the game afterward.

- `controller_pitch_sensitivity`: vertical controller camera speed
- `controller_zoom_speed`: L3 + right-stick zoom speed
- `mouse_pitch_sensitivity`: vertical camera speed while holding mouse rotate
- `invert_controller_pitch`: `true` or `false`
- `invert_controller_zoom`: `false` makes stick up zoom in; `true` reverses it

Existing configuration files are preserved. Missing settings are added with
their default values.

BG3 mouse-rotate bindings are read from the active profile's
`inputconfig_p1.json`. Changes made in BG3 are detected within about half a
second and do not require a restart. Configuration-file changes still require a
restart.

## Zoom stability and limits

The original patch disabled one of BG3's native zoom-state writes. Runtime
diagnostics showed that this lets the internal current and desired zoom values
diverge after zoom input, which produces the persistent micro-jitter. This
release leaves that native instruction intact and synchronizes all three known
zoom fields only when applying a custom mouse-wheel or L3 + right-stick step.

The tradeoff is intentional: BG3's native closest and farthest zoom limits are
active again. The earlier excessive near/far range is not part of this stable
fix because restoring it by disabling the native write reintroduces the jitter.

## Known limitation

The patch intentionally does not modify BG3's Tactical Camera command. Extended
top-down camera angles work, but the game's automatic tactical outlines may not
remain active after switching from mouse/keyboard to controller.

As a workaround, use BG3's outline key while using mouse/keyboard (`^` on a
German keyboard layout). A proper controller-compatible solution would require
a separate high-level game-action hook and is not included in this stable
release.

## Building and testing

SDL2 development headers and CMake 3.16 or newer are required:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Full verification of runtime camera hooks still requires a supported native BG3
build.

## Credits and permissions

- Original Linux mod: **Biiinks78 / 0x1496FD0**
- Additional safety and default-binding work: **Joegoldin**
- Windows implementation used as a technical reference: **ersh1**
- Community input and portability fixes: **JustLetMeInFSS**

The original author's stated permissions allow modified bug fixes and feature
improvements with credit. The original restrictions still apply: do not upload
the original mod to other sites, convert it for other games, sell this work, or
earn donation points from it.

This repository is an unofficial derivative fork and does not claim ownership
of the original mod.
