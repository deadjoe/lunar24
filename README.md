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

VST3 needs the Steinberg SDK inside the iPlug2 submodule. CI fetches it like this:

```sh
cd third_party/iPlug2/Dependencies/IPlug
rm -rf VST3_SDK
git clone --quiet --depth 1 --branch v3.8.1_build_84 https://github.com/steinbergmedia/vst3sdk.git VST3_SDK
git -C VST3_SDK submodule update --quiet --init --depth 1 base pluginterfaces public.sdk cmake
```

Without it the app still builds, and on macOS so does the AU.

CI attaches the app and plugins to each run. Clear quarantine on a downloaded Mac build
once: `xattr -cr Lunar24.app` (plugins: `xattr -dr com.apple.quarantine <plugin folder>`).

Progress and how to play: [`design/STATUS.md`](design/STATUS.md).
