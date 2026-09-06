# Unofficial Controller Input Community Fix

An unofficial controller, mouse-input, stability, and portability patch for
[BG3 Linux Native Camera Tweaks](https://www.nexusmods.com/baldursgate3/mods/23896).
Based on the original Version 1.0.21

The original mod and camera implementation were created by **Biiinks78 / 0x1496FD0**.
This fork focuses on making the native Linux version responsive, stable, and
portable across user installations.

![Version](https://img.shields.io/badge/version-1.0.21--controller--input--fix-blue)
![Platform](https://img.shields.io/badge/platform-Linux-green)
![Architecture](https://img.shields.io/badge/architecture-x86--64-lightgrey)

## What this patch changes

- Removes controller input delay caused by prematurely ending SDL event polling.
- Makes vertical controller rotation frame-rate independent and configurable.
- Adds L3 + right-stick vertical movement for smooth camera zoom.
- Prevents persistent zoom jitter
- Adds configurable vertical mouse-camera speed.
- Uses portable XDG paths and automatically creates a per-user configuration.
- Falls back to the middle mouse button when BG3 has not serialized its default
  mouse-rotate binding.
- Requires only GLIBC 2.17 or newer and has no runtime dependency beyond libc.

The controller response model is based on the deadzone and time-scaled pitch
formula used by
[BG3 Native Camera Tweaks for Windows](https://github.com/ersh1/BG3_NativeCameraTweaks).
Technical details and tests are documented in
[`CONTROLLER_INPUT_ANALYSIS.md`](CONTROLLER_INPUT_ANALYSIS.md).

## Supported game builds

This release supports the native Linux versions:

- `4.1.1.7209685`
- `4.1.1.7398727`

Unsupported executables are left untouched and the mod disables itself safely.

## Installation

1. Download `linux_native_camera_tweaks.so`. located in the zip folder in src folder
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
```

Close BG3 before editing the file and restart the game afterward.

- `controller_pitch_sensitivity`: vertical controller camera speed
- `controller_zoom_speed`: L3 + right-stick zoom speed
- `mouse_pitch_sensitivity`: vertical camera speed while holding mouse rotate
- `invert_controller_pitch`: `true` or `false`

Existing configuration files are preserved. Missing settings are added with
their default values.

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
- Windows implementation used as a technical reference: **ersh1**
- Community input and portability fixes: **JustLetMeInFSS**

The original author's stated permissions allow modified bug fixes and feature
improvements with credit. The original restrictions still apply: do not upload
the original mod to other sites, convert it for other games, sell this work, or
earn donation points from it.

This repository is an unofficial derivative fork and does not claim ownership
of the original mod.
