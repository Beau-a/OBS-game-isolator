# obs-pipewire-extended

An OBS Studio plugin for Arch Linux (and any PipeWire-based system) that exposes per-application audio streams as individual OBS sources.

Instead of capturing all desktop audio into a single track, this plugin lets you pick a specific running application from a drop-down and route its audio independently — useful for separating game audio, voice chat, music, or browser audio onto different OBS tracks.

## How it works

The plugin connects to PipeWire and watches the node registry for `Stream/Output/Audio` nodes — one is created by PipeWire for every application that plays audio. When you add a **PipeWire Extended** source in OBS and select a target application, the plugin creates a PipeWire capture stream linked to that application's node. The audio is captured as interleaved 32-bit float and fed directly into OBS without affecting the application's normal playback to speakers.

## Requirements

- OBS Studio (tested against obs-studio-git 32.1.1)
- PipeWire ≥ 0.3 (tested against 1.6.2)
- WirePlumber (handles automatic stream linking)
- CMake ≥ 3.16
- GCC or Clang with C11 support

On Arch:
```
obs-studio (or obs-studio-git from AUR)
pipewire
wireplumber
```

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Install

**System-wide** (requires sudo):
```bash
sudo cmake --install build
```

This places `libobs-pipewire-extended.so` in `/usr/lib/obs-plugins/`.

**Per-user** (no sudo):
```bash
mkdir -p ~/.config/obs-studio/plugins/obs-pipewire-extended/bin/64bit
cp build/libobs-pipewire-extended.so \
   ~/.config/obs-studio/plugins/obs-pipewire-extended/bin/64bit/obs-pipewire-extended.so
```

## Usage

1. Restart OBS after installing
2. In a scene, click **+** → **PipeWire Extended**
3. Open the source properties and select the application to capture from the drop-down
4. The list updates live as applications start and stop

Each **PipeWire Extended** source is independent — add one per application you want to isolate.

## License

GPL-2.0-or-later (matching OBS Studio)
