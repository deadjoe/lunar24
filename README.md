# Lunar 24

Open-source ambient / drone instrument for Apple silicon Mac and Windows.
The panel and its functions follow the published ELTA Solar 42N manual.
Lunar 24 is not affiliated with ELTA Music. Sound is tuned by ear, not matched
to the hardware.

Standalone app, VST3 (Mac and Windows), and AU (Mac). No Intel Mac build.
Linux CI runs the portable tests only.

The Solar 42N manual and panel drawing are ELTA copyright and are not in this
repository (`design/reference/SOURCES.md`).

## License

Apache-2.0. See [LICENSE](LICENSE). Dependencies keep their own licenses
(`third_party/licenses/`).

## Build

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

VST3 needs the SDK at `third_party/iPlug2/Dependencies/IPlug/VST3_SDK`
(CI uses tag `v3.8.1_build_84`). Without it, the app still builds, and on macOS so does the AU.

CI attaches the app and plugins to each run. Clear quarantine on a downloaded
Mac build once: `xattr -cr Lunar24.app`.

Progress and how to play: [`design/STATUS.md`](design/STATUS.md).
