# Community Clash Streamer

Open-source Windows streamer client for Community Clash.

The client is intentionally limited to capturing the World of Warcraft game window. It connects to the Community Clash backend for streamer authentication and receives a LiveKit room token from the server. Server secrets are not included in this repository.

## Features

- Windows x64 desktop client
- World of Warcraft window capture via Windows Graphics Capture
- WoW remains in the stream while using Alt+Tab; other windows are not captured
- optional game/system audio capture, microphone stays disabled
- automatic team name from the Community Clash backend
- automatic stream shutdown when WoW closes
- clean LiveKit disconnect when the client exits
- LiveKit C++ SDK pinned to 1.11.0 with SHA-256 verification in CI

## Releases

The current public release is **v3.4.0**. It is currently unsigned while the project applies for free code signing through SignPath Foundation.

Download: https://github.com/bloody20082-ai/community-clash-streamer-client/releases/tag/v3.4.0

## Build

Requirements:

- Windows 10/11 x64
- Visual Studio 2022 with Desktop development with C++
- CMake 3.24+
- LiveKit C++ SDK 1.11.0 for Windows x64

The GitHub Actions workflow downloads the pinned LiveKit SDK and verifies this SHA-256 before building:

`d95d677c3ca7348e0af18ede713f7fe825c14a726296ff65524b933f64f965dc`

For a local build, extract LiveKit 1.11.0 and configure CMake with its root directory in `CMAKE_PREFIX_PATH`.

```powershell
cmake -S . -B out -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:\path\to\livekit-sdk-windows-x64-1.11.0"
cmake --build out --config Release --parallel
```

## Network connections

The application communicates with:

- `https://community-clash.de/api/token/streamer` for streamer authentication and room credentials
- `wss://community-clash.de` for the LiveKit connection and media stream

See [PRIVACY.md](PRIVACY.md) for details.

## Code signing policy

See [CODE_SIGNING_POLICY.md](CODE_SIGNING_POLICY.md).

## Security

Please see [SECURITY.md](SECURITY.md). Do not report credentials or other sensitive information in public GitHub issues.

## License

MIT. See [LICENSE](LICENSE).
