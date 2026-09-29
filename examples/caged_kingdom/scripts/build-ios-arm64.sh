#!/usr/bin/env bash
export JCE_GAME_PROJECT_DIR=examples/caged_kingdom
export JCE_GAME_TARGET=CagedKingdom
exec bash "$(dirname "$0")/../../../scripts/macos/build-ios-arm64.sh" "$@"
