# vix-up

Standalone Vix toolchain manager written in Go. It does not require Vix or vixc to run.

## Build

Go 1.22 or newer is required:

    go build -o vix-up .

## Usage

    vix-up show
    vix-up toolchain list
    vix-up toolchain install stable
    vix-up default stable
    vix-up toolchain uninstall 0.5.1
    vix-up doctor

The default home is ~/.vixup on Unix and %LOCALAPPDATA%/Vixup on Windows. Set VIXUP_HOME for tests or custom installations. Set VIXUP_DIST_SERVER for a distribution mirror.

A distribution server is expected to provide <server>/<channel>/manifest.json. The manifest selects an artifact by target triple and includes a SHA-256 checksum.

Manifest example:

    {"schema":1,"version":"0.5.1","channel":"stable","artifacts":[{"target":"aarch64-apple-darwin","url":"https://example/vixc.tar.gz","sha256":"...","size":123,"format":"tar.gz"}]}

## Security

Downloads are checked with SHA-256 before extraction. Archive entries are confined to the staging directory, installation is staged before replacing a toolchain, and the active toolchain cannot be uninstalled.

## Current limitations

The release manifest and CI publication are not wired yet. Update and project-local toolchain overrides are planned after the MVP. The vix command is provided as a shim to the selected compiler.
