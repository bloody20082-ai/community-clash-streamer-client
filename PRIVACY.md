# Privacy

Community Clash Streamer does not contain analytics, advertising, or telemetry code.

To provide its streaming functionality, the client performs the following network operations:

- The streamer username and password entered by the user are sent over HTTPS to `community-clash.de` for authentication.
- The password is not written to disk by the client.
- After a successful login, the server returns LiveKit connection data for the stream session.
- The World of Warcraft video capture and, when enabled, system/game audio are published to the Community Clash LiveKit service at `wss://community-clash.de`.
- The microphone is not captured by this client.

The client does not intentionally transmit other desktop windows. It targets the detected World of Warcraft window using Windows Graphics Capture.

The Community Clash server infrastructure has its own operational logging and hosting requirements. Those server-side systems are not part of this source repository.
