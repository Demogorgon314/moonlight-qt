#!/bin/bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <app-bundle>" >&2
    exit 2
fi

APP_BUNDLE=$1
if [ ! -d "$APP_BUNDLE" ]; then
    echo "Development signing skipped: app bundle not found at $APP_BUNDLE" >&2
    exit 1
fi

SIGNING_CERTIFICATE=${MOONLIGHT_MACOS_DEVELOPMENT_SIGNING_IDENTITY:-${SIGNING_IDENTITY:-}}
if [ -z "$SIGNING_CERTIFICATE" ]; then
    SIGNING_CERTIFICATE=$(
        /usr/bin/security find-identity -v -p codesigning |
            /usr/bin/awk '/"Apple Development:/{print $2; exit}'
    )
fi

if [ -z "$SIGNING_CERTIFICATE" ]; then
    echo "Development signing skipped: no Apple Development identity is available"
    exit 0
fi

# Keychain ACLs use the executable's designated requirement. A linker-created
# ad-hoc signature is only a CDHash, which changes after every rebuild and makes
# macOS request access to saved credentials again. A development certificate
# gives local builds a stable designated requirement without weakening the item.
/usr/bin/codesign \
    --force \
    --deep \
    --sign "$SIGNING_CERTIFICATE" \
    "$APP_BUNDLE"
