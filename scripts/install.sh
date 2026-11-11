#!/usr/bin/env bash
# Installs the CppBoot SDK on Linux, with the compiler, CMake, Ninja and libraries from apt.
#
#   curl -fsSL https://raw.githubusercontent.com/mahirmovsumov/CppBoot/main/scripts/install.sh | bash
#
# To choose options, pass them after "bash -s --":
#
#   curl -fsSL .../install.sh | bash -s -- --version 0.1.0 --prefix ~/cppboot
#
# Downloaded SDKs are built on Ubuntu 24.04. Elsewhere, use --from-source, which builds the SDK
# with this machine's compiler.
set -euo pipefail

repository=mahirmovsumov/CppBoot
version=latest
prefix="$HOME/.cppboot"
archive=""
from_source=no
change_path=yes
install_packages=yes

usage() {
    cat <<'EOF'
Options:
  --version <x.y.z>   A release, or latest, which includes pre-releases
  --prefix <folder>   Where to install, by default ~/.cppboot
  --archive <file>    An SDK archive already downloaded
  --from-source       Build the SDK on this machine
  --skip-packages     Don't install packages with apt
  --no-path           Don't add CppBoot to PATH in your shell's startup files
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --version) version="$2"; shift 2 ;;
        --prefix) prefix="$2"; shift 2 ;;
        --archive) archive="$(realpath "$2")"; shift 2 ;;
        --from-source) from_source=yes; shift ;;
        --skip-packages) install_packages=no; shift ;;
        --no-path) change_path=no; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

step() { printf '\n==> %s\n' "$*"; }
fail() { printf 'Error: %s\n' "$*" >&2; exit 1; }

as_root() {
    if [ "$(id -u)" -eq 0 ]; then "$@"; else sudo "$@"; fi
}

. /etc/os-release 2>/dev/null || true
if [ "$from_source" = no ] && [ -z "$archive" ] && { [ "${ID:-}" != ubuntu ] || [ "${VERSION_ID:-}" != 24.04 ]; }; then
    fail "Downloaded SDKs are for Ubuntu 24.04, and this is ${PRETTY_NAME:-an unknown system}. Run this again with --from-source."
fi

if [ "$install_packages" = yes ]; then
    command -v apt-get >/dev/null || fail "This needs apt. Install the packages listed in the docs yourself, then use --skip-packages."
    step "Installing the compiler, CMake, Ninja and libraries with apt"
    as_root apt-get update -q
    as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y -q \
        g++ gdb cmake ninja-build curl ca-certificates tar gzip \
        libclang-dev libyaml-cpp-dev libboost-dev nlohmann-json3-dev \
        libpq-dev libsqlite3-dev libssl-dev libgtest-dev
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
staged="$prefix.new"
rm -rf "$staged"

find_release() {
    local releases tag
    releases="$(curl -fsSL -H 'User-Agent: cppboot-installer' "https://api.github.com/repos/$repository/releases")"
    if [ "$version" = latest ]; then
        tag="$(printf '%s' "$releases" | grep -m1 '"tag_name"' | sed -E 's/.*"tag_name": *"([^"]+)".*/\1/')"
        [ -n "$tag" ] || fail "CppBoot has no releases yet. Use --from-source."
    else
        tag="v$version"
        printf '%s' "$releases" | grep -q "\"tag_name\": *\"$tag\"" || fail "There's no CppBoot release $version."
    fi
    printf '%s' "$tag"
}

if [ "$from_source" = yes ]; then
    if [ "$version" = latest ]; then
        source_url="https://github.com/$repository/archive/refs/heads/main.tar.gz"
    else
        source_url="https://github.com/$repository/archive/refs/tags/v$version.tar.gz"
    fi
    step "Downloading the source"
    curl -fsSL "$source_url" | tar -xz -C "$work"
    tree="$(find "$work" -mindepth 1 -maxdepth 1 -type d | head -n 1)"
    step "Building the SDK, which takes several minutes"
    cmake -S "$tree" -B "$tree/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCPPBOOT_BUILD_TESTS=OFF -DCPPBOOT_BUILD_EXAMPLES=OFF
    cmake --build "$tree/build"
    cmake --install "$tree/build" --prefix "$staged"
else
    if [ -z "$archive" ]; then
        tag="$(find_release)"
        number="${tag#v}"
        name="cppboot-sdk-$number-linux-x64.tar.gz"
        base="https://github.com/$repository/releases/download/$tag"
        step "Downloading CppBoot $number"
        curl -fsSL -o "$work/$name" "$base/$name"
        curl -fsSL -o "$work/SHA256SUMS" "$base/SHA256SUMS"
        (cd "$work" && grep " $name\$" SHA256SUMS | sha256sum -c --quiet -) ||
            fail "The download doesn't match its checksum, so it wasn't installed."
        archive="$work/$name"
    fi
    step "Unpacking the SDK"
    mkdir -p "$work/unpacked"
    tar -xzf "$archive" -C "$work/unpacked"
    mv "$(find "$work/unpacked" -mindepth 1 -maxdepth 1 -type d | head -n 1)" "$staged"
fi

# The new version is complete before the old one goes.
rm -rf "$prefix"
mv "$staged" "$prefix"

line="export PATH=\"$prefix/bin:\$PATH\""
if [ "$change_path" = yes ]; then
    step "Adding CppBoot to PATH"
    for file in "$HOME/.bashrc" "$HOME/.profile" "$HOME/.zshrc"; do
        if [ -f "$file" ] || [ "$file" = "$HOME/.profile" ]; then
            grep -qxF "$line" "$file" 2>/dev/null || printf '\n# CppBoot\n%s\n' "$line" >>"$file"
        fi
    done
fi
export PATH="$prefix/bin:$PATH"

step "Checking the installation"
if "$prefix/bin/cppboot" doctor; then
    printf '\nCppBoot is installed in %s.\n' "$prefix"
    printf 'Open a new terminal, or run: %s\n' "$line"
    printf 'Then create your first project:\n  cppboot create hello-world\n  cd hello-world\n  cppboot run\n'
else
    printf '\nCppBoot is installed, but cppboot doctor found problems. Fix them, then run it again.\n'
    exit 1
fi
