askerdway

Windows x64 Dota 2 overlay with a C++ runtime and a Rust inference core.

![Askerdway Render menu](https://github.com/user-attachments/assets/de1befc1-ea0b-46e4-b472-2c6d8fd58e7a)

Features

- Particle observations, hero traces, teleport routes and channel cancellation tracking.
- Jungle camp state, stack estimates and lane creep wave forecasts.
- Enemy health, ability and item cooldown presentation.
- Armlet toggling with incoming damage checks and a user configured hotkey.
- Comfort actions and an in-game menu with persistent local settings.
- Typed Dota schema views and a versioned C API.

Requirements

Windows 10/11 x64, Visual Studio 2022 or newer with Desktop development with C++, a Windows SDK, CMake 3.24 or newer, Git and rustup. The pinned Rust toolchain is installed by rustup. CMake fetches Dear ImGui v1.92.8 and MinHook v1.3.4; the first build requires network access.

Build from an x64 Native Tools Command Prompt:

```text
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

Use the installed Visual Studio generator if its version differs. Output: build/askerdway.dll. Runtime assets are embedded in the DLL. No Python or installed Dota files are required to compile.

Run

The DLL requires the Microsoft Visual C++ x64 runtime. Start the x64 Dota 2 client in Direct3D 11 mode and load askerdway.dll with a compatible LoadLibraryW loader. A loader is not included. Restart the client before loading a new build. Press Insert to open the menu and configure features and hotkeys.

Schema offsets and particle signatures depend on the installed game version. A successful build does not establish compatibility with every Dota 2 update. Use a local demo or private lobby to validate the current game build.

Project code owned by qrexgr is licensed under the MIT License. Copyright (c) 2026 qrexgr. See LICENSE.

Third party terms are in THIRD_PARTY_NOTICES.txt and assets/fonts/LICENSE-FONT.txt.

Release verification

The repository owner can run the Build and scan release workflow on main and select an existing release tag. It builds that tag on a Windows GitHub runner, checks DLL loading and the ABI, updates ClamAV definitions, and scans the resulting DLL. Enable the publish input to replace the release assets; its default is verification only. Publication stops on a build, definition update or scan error, or any detection.

A successful run with publish enabled replaces the release archive with the checked DLL and includes build metadata, scan evidence, license notices and SHA256SUMS.txt. The release also provides an archive checksum, clamav-report.json, clamav.log and a link to the workflow run. Verify the downloaded archive checksum and the DLL hash against the report. No detections by ClamAV does not guarantee that software is safe.
