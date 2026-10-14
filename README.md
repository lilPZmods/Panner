# Panner

**by Anti-Music.** Free and open source.

Panner is a sequenced, tempo-synced Atmos panner (AAX / VST3 / AU) built to close the gap the Dolby Atmos Music Panner leaves open: programmed movement that you can print with an **offline bounce**, without sitting through a real-time pass into the Dolby Renderer.

## Status: early prototype

Panner **bridges to Pro Tools' native object panner**. The pattern lives in the plugin while you work. **Push to Pro Tools** writes it, over your timeline selection, into the track's native X / Y / Z / Size pan automation. The track then plays and bounces as a real Atmos object through Pro Tools' own renderer, with no real-time pass.

### No silent drift

Once pushed, Panner reads those lanes back every couple of seconds and right after any change to Panner itself. If the lanes and the plugin ever disagree, from either side, it throws a flashing full-window prompt with no close button. You can't dismiss it until you choose:

- **Panner wins:** push again.
- **Undo Panner changes:** go back to what was last pushed.
- **Pro Tools wins:** unlink, so the lanes become the master.

The prompt gets its own always-on-top window when the plugin is closed. It also fires if you run an offline bounce while Panner can't confirm the lanes match.

The bridge uses Pro Tools' scripting API (PTSL) through a small built-in client, with no gRPC dependency. **Current blocker:** in Pro Tools 2026.4 the commands that write and read pan automation belong to Avid's *private* API. Pro Tools refuses them ("Private API has not been enabled in this session") unless the client presents a key issued by Avid. Everything else has been tested against a simulated PTSL server, and the connection and registration work against a real Pro Tools 2026.4.

### Also in the box

- **Bed mode:** Panner renders the movement itself, from mono or stereo into stereo, 5.1, 7.1, 5.1.2, 5.1.4, 7.1.2, 7.1.4, 7.1.6 or 9.1.6. This works in any DAW.
- **Positioning:** room-style X / Y / Z with Size, Stereo Width and Gain.
- **Sequencer:** up to 16 steps at tempo-synced rates (straight, triplet, dotted), with Glide between steps. Everything follows the host timeline, so offline bounces match realtime playback.

## Building

Requirements: CMake 3.22+ and a C++20 compiler (Xcode or the Command Line Tools on macOS). JUCE is downloaded at configure time.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Plugins land in `build/Panner_artefacts/Release/`. The AAX build is unsigned, so it only loads in Pro Tools Developer builds until it is PACE-signed.

## License

[AGPLv3](LICENSE). Built on [JUCE](https://juce.com) under its AGPLv3 license.
