# Microsoft Store / MSIX packaging

This directory contains the packaging setup for the Microsoft Store build.

## What is already automated

The normal Windows GitHub Actions build creates the portable package and then creates an unsigned x64 MSIX package with MakeAppx.

If `packaging/msix/store-identity.json` does not exist, the workflow uses a development-only identity. That package validates the MSIX layout and manifest and must not be submitted to the Store.

When the Partner Center product is ready, copy `store-identity.json.example` to `store-identity.json` and replace all three values with the exact values from:

- Package/Identity/Name
- Package/Identity/Publisher
- Package/Properties/PublisherDisplayName

Commit that file. The next build will emit the artifact `CommunityClashStreamer-MSIX-store`.

## Versioning

The package version is derived from the CMake project version and converted from `Major.Minor.Patch` to `Major.Minor.Patch.0`.

## Signing

The workflow intentionally creates an unsigned MSIX for Microsoft Store submission. Microsoft Store re-signs accepted MSIX/AppX packages during publishing. Do not use the unsigned Store artifact for direct distribution outside the Microsoft Store.

## Restricted capability

The manifest declares `runFullTrust`, which is required for this packaged classic Win32 desktop app. Partner Center can ask for an explanation of this restricted capability during submission.
