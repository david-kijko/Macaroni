#!/usr/bin/env bash
# Build a local Mac application using Homebrew's Qt and OpenSSL runtime.
set -euo pipefail
[[ "$(uname -s)" == Darwin ]] || { echo 'Run this on macOS.' >&2; exit 1; }
root=$(cd "$(dirname "$0")/.." && pwd)
build=${BUILD_DIR:-"$root/build"}
prefix=$(brew --prefix)
qtbase=$(brew --prefix qtbase)
qtws=$(brew --prefix qtwebsockets)
git -C "$root" submodule update --init --recursive
cmake -S "$root/backend" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$prefix;$qtbase;$qtws" -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --build "$build" --target mw-server --parallel "${BUILD_JOBS:-4}"

app="$build/Macaroni.app"
resources="$app/Contents/Resources"
mkdir -p "$resources/frontend" "$app/Contents/MacOS"
install -m 755 "$build/MoonlightWeb" "$app/Contents/MacOS/Macaroni"
for item in assets css js locales index.html manifest.webmanifest; do
    ditto "$root/frontend/$item" "$resources/frontend/$item"
done
work=$(mktemp -d)
icons="$work/Macaroni.iconset"
mkdir "$icons"
for size in 16 32 128 256 512; do
    sips -z "$size" "$size" "$root/frontend/assets/logo-512.png" --out "$icons/icon_${size}x${size}.png" >/dev/null
    double=$((size * 2))
    sips -z "$double" "$double" "$root/frontend/assets/logo-512.png" --out "$icons/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$icons" -o "$resources/Macaroni.icns"
printf '[Paths]\nPlugins = %s\n' "$("$qtbase/bin/qtpaths" --query QT_INSTALL_PLUGINS)" > "$resources/qt.conf"
cp "$root/backend/packaging/Macaroni-Info.plist" "$app/Contents/Info.plist"
codesign --force --sign - --timestamp=none "$app"
codesign --verify --deep --strict "$app"
printf 'Built %s\n' "$app"
