# Panner

**by Anti-Music.** Free and open source. Amazing shit should be free.

Panner is a sequenced, tempo-synced Atmos panner (AAX / VST3 / AU) built to close the gap the Dolby Atmos Music Panner leaves open: programmed movement that you can print with an **offline bounce**, without sitting through a real-time pass into the Dolby Renderer.

## Status: early prototype

What works today:

- Mono or stereo in, out to stereo, 5.1, 7.1, 5.1.2, 5.1.4, 7.1.2, 7.1.4, 7.1.6 or 9.1.6 (bed panning, constant power).
- Room-style X / Y / Z positioning with Size, Stereo Width and Gain.
- Up to 16 steps, each with its own position, at tempo-synced rates (straight, triplet, dotted) with Glide between steps.
- The sequence is computed from the host timeline, so offline bounces land on the same positions as realtime playback.

Not yet tested inside a DAW.

## Where it's going: bridging to the native panner

The pattern lives in the plugin while you work. When it's time to print, one button writes it to the track's **native** pan automation lanes (X / Y / Z / Size) as a real Atmos object, rendered by the DAW's own renderer.

Panner then keeps checking that the plugin and the DAW lanes still agree. If they ever differ, it throws a blocking prompt you can't dismiss until you choose which side wins. No silent drift.

Pro Tools comes first: its scripting API (PTSL) can write and read pan breakpoints.

## Building

Requirements: CMake 3.22+ and a C++20 compiler (Xcode or the Command Line Tools on macOS). JUCE is downloaded at configure time.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Plugins land in `build/Panner_artefacts/Release/`. The AAX build is unsigned, so it only loads in Pro Tools Developer builds until it is PACE-signed.

## License

[AGPLv3](LICENSE). Built on [JUCE](https://juce.com) under its AGPLv3 license.
