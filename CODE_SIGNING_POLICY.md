# Code signing policy

Free code signing provided by SignPath.io, certificate by SignPath Foundation.

## Project roles

- Committers and reviewers: GitHub user `bloody20082-ai` and future maintainers explicitly granted write/review access to this repository.
- Approvers: GitHub user `bloody20082-ai` and future project owners explicitly assigned the SignPath approver role.

Changes from contributors who are not committers must be reviewed by a project maintainer before merge. Every release signing request requires manual approval by an authorized approver.

## Build and signing

Release binaries are built from this public repository using GitHub Actions on GitHub-hosted Windows runners. The build downloads the pinned LiveKit SDK from the official LiveKit GitHub release and verifies the expected SHA-256 digest before compilation.

After SignPath Foundation approval, the unsigned GitHub Actions artifact will be submitted to SignPath using origin verification. The signed artifact will only be published after the signing request has been manually approved.

## Privacy policy

See [PRIVACY.md](PRIVACY.md).
