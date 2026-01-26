#!/usr/bin/env bash
# Build SF2 module for Move Anything (ARM64)
#
# Automatically uses Docker for cross-compilation if needed.
# Set CROSS_PREFIX to skip Docker (e.g., for native ARM builds).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== SF2 Module Build (via Docker) ==="
    echo ""

    # Build Docker image if needed
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        echo ""
    fi

    # Run build inside container
    echo "Running build..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh

    echo ""
    echo "=== Done ==="
    exit 0
fi

# === Actual build (runs in Docker or with cross-compiler) ===
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"

echo "=== Building SF2 Module ==="
echo "Cross prefix: $CROSS_PREFIX"

# Create build directories
mkdir -p build
mkdir -p dist/sf2

# Compile FluidLite library
echo "Compiling FluidLite..."
FLUIDLITE_DIR="src/dsp/third_party/fluidlite"
FLUIDLITE_SRCS="
    $FLUIDLITE_DIR/src/fluid_chan.c
    $FLUIDLITE_DIR/src/fluid_chorus.c
    $FLUIDLITE_DIR/src/fluid_conv.c
    $FLUIDLITE_DIR/src/fluid_defsfont.c
    $FLUIDLITE_DIR/src/fluid_dsp_float.c
    $FLUIDLITE_DIR/src/fluid_gen.c
    $FLUIDLITE_DIR/src/fluid_hash.c
    $FLUIDLITE_DIR/src/fluid_init.c
    $FLUIDLITE_DIR/src/fluid_list.c
    $FLUIDLITE_DIR/src/fluid_mod.c
    $FLUIDLITE_DIR/src/fluid_ramsfont.c
    $FLUIDLITE_DIR/src/fluid_rev.c
    $FLUIDLITE_DIR/src/fluid_settings.c
    $FLUIDLITE_DIR/src/fluid_synth.c
    $FLUIDLITE_DIR/src/fluid_sys.c
    $FLUIDLITE_DIR/src/fluid_tuning.c
    $FLUIDLITE_DIR/src/fluid_voice.c
"

# Compile FluidLite objects
mkdir -p build/fluidlite
for src in $FLUIDLITE_SRCS; do
    obj="build/fluidlite/$(basename $src .c).o"
    ${CROSS_PREFIX}gcc -O3 -fPIC \
        -march=armv8-a -mtune=cortex-a72 \
        -DNDEBUG \
        -I$FLUIDLITE_DIR/include \
        -I$FLUIDLITE_DIR/src \
        -c "$src" -o "$obj"
done

# Compile DSP plugin
echo "Compiling DSP plugin..."
${CROSS_PREFIX}gcc -O3 -shared -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -DNDEBUG \
    src/dsp/sf2_plugin.c \
    build/fluidlite/*.o \
    -o build/dsp.so \
    -Isrc/dsp \
    -I$FLUIDLITE_DIR/include \
    -lm

# Copy files to dist (use cat to avoid ExtFS deallocation issues with Docker)
echo "Packaging..."
cat src/module.json > dist/sf2/module.json
cat src/ui.js > dist/sf2/ui.js
cat build/dsp.so > dist/sf2/dsp.so
chmod +x dist/sf2/dsp.so

# Create soundfonts directory for user-supplied SF2 files
mkdir -p dist/sf2/soundfonts

# Create tarball for release
cd dist
tar -czvf sf2-module.tar.gz sf2/
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output: dist/sf2/"
echo "Tarball: dist/sf2-module.tar.gz"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh"
