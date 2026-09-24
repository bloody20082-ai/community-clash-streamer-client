# Security Policy

## Supported version

Security fixes are applied to the current release line.

## Reporting a vulnerability

Please do not publish passwords, access tokens, private keys, or other sensitive details in a public issue.

For security-sensitive reports, contact the Community Clash project maintainers privately through the contact method published on the Community Clash website.

## Security design

- LiveKit API keys and API secrets belong on the server and are not embedded in this client.
- Streamer authentication is performed over HTTPS.
- The build pins LiveKit C++ SDK 1.11.0 and verifies the official Windows x64 archive using SHA-256 before extraction.
- The Windows client only receives participant credentials required for its stream session.
