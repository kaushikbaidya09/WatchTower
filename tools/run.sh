#!/usr/bin/env bash
#==============================================================================
# @file    run.sh
# @brief   Build, flash and monitor the WatchTower firmware with ESP-IDF.
#
# Run it with --help for the options.
#==============================================================================

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: tools/run.sh [steps] [--board BOARD] [--port PORT]

Steps (combine freely; they always run in this order):
  --clean      Delete everything generated or downloaded: the build output,
               the fetched components and the editor's index cache
  --build      Build the firmware (GNU Make, not Ninja)
  --flash      Flash firmware and web console to the board
  --monitor    Open the serial monitor (exit with Ctrl+])
  --all        All four: clean, build, flash, monitor

Options:
  --board BOARD  Target board; asked for when left out:
                   s3-16mb  ESP32-S3, 16 MB flash
                   s3-8mb   ESP32-S3 QFN56, 8 MB flash
                 Each board builds in build/BOARD/ with sdkconfig.BOARD.
  --port PORT    Serial port (default: /dev/ttyACM0)
  --help         Show this help

Note: the web console is part of the firmware image. Settings and saved
WiFi networks live in NVS and are kept when flashing.
EOF
}

#------------------------------------------------------------------------------
# Read the command line
#------------------------------------------------------------------------------
PORT=/dev/ttyACM0
BOARD=
CLEAN=no
BUILD=no
FLASH=no
MONITOR=no

if [ "$#" -eq 0 ]; then
    usage
    exit 2
fi

while [ "$#" -gt 0 ]; do
    case "$1" in
        --clean)   CLEAN=yes ;;
        --build)   BUILD=yes ;;
        --flash)   FLASH=yes ;;
        --monitor) MONITOR=yes ;;
        --all)     CLEAN=yes; BUILD=yes; FLASH=yes; MONITOR=yes ;;
        --board)   BOARD="${2:?--board needs a value: s3-16mb or s3-8mb}"; shift ;;
        --port)    PORT="${2:?--port needs a value, e.g. --port /dev/ttyACM0}"; shift ;;
        --help)    usage; exit 0 ;;
        *)         echo "error: unknown option '$1' (try --help)" >&2; exit 2 ;;
    esac
    shift
done

#------------------------------------------------------------------------------
# Select the board
#------------------------------------------------------------------------------
if [ -z "$BOARD" ]; then
    if [ ! -t 0 ]; then
        echo "error: no board given - pass --board s3-16mb or --board s3-8mb" >&2
        exit 2
    fi
    echo "Select the board:"
    PS3="Board number: "
    select BOARD in s3-16mb s3-8mb; do
        [ -n "$BOARD" ] && break
    done
fi

case "$BOARD" in
    s3-16mb | s3-8mb) ;;
    *) echo "error: unknown board '$BOARD' (s3-16mb or s3-8mb)" >&2; exit 2 ;;
esac

# Each board has its own build directory and sdkconfig, so switching boards
# does not rebuild the other one. sdkconfig.defaults.BOARD (flash size,
# partition table) is layered on top of the shared sdkconfig.defaults.
BUILD_DIR="build/$BOARD"
BOARD_DEFINES=(-D "SDKCONFIG=sdkconfig.$BOARD"
               -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.$BOARD")
echo "==> Board $BOARD (build directory $BUILD_DIR/)"

#------------------------------------------------------------------------------
# Check the serial port and find ESP-IDF
#------------------------------------------------------------------------------
if [ "$FLASH" = yes ] || [ "$MONITOR" = yes ]; then
    if [ ! -e "$PORT" ]; then
        echo "error: serial port $PORT not found - is the board connected?" >&2
        exit 1
    fi
fi

# 'idf' runs idf.py. In a terminal that has the ESP-IDF environment loaded,
# idf.py is already on the PATH and is used as it is.
idf() { idf.py "$@"; }

# Otherwise load the environment from the ESP-IDF installer's activation
# script. That script cannot be sourced from inside another script, but run
# with -e it prints the variables it would set, one NAME=value per line.
if ! command -v idf.py >/dev/null 2>&1; then
    activate="$(ls "$HOME"/.espressif/tools/activate_idf_*.sh 2>/dev/null | tail -n 1)"
    if [ -z "$activate" ]; then
        echo "error: ESP-IDF not found - load its environment in this terminal first" >&2
        exit 2
    fi

    while IFS='=' read -r name value; do
        if [ "$name" = PATH ]; then
            export PATH="$value:$PATH"   # add the tools, keep the system PATH
        else
            export "$name=$value"
        fi
    done < <(sh "$activate" -e)

    idf() { "$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py" "$@"; }
fi

#------------------------------------------------------------------------------
# Run the steps
#------------------------------------------------------------------------------
# Work from the project root, one level up from tools/.
cd "$(dirname "${BASH_SOURCE[0]}")/.."

# Everything a build generates or downloads:
#   build/BOARD/         the selected board's compiled objects and images
#   managed_components/  components fetched by the ESP-IDF component manager
#                        (downloaded again on the next build, needs a network)
#   .cache/              the editor's (clangd) code index
# sdkconfig.BOARD is configuration, not a cache, and is kept.
if [ "$CLEAN" = yes ]; then
    echo "==> Deleting $BUILD_DIR/, fetched components and caches"
    rm -rf "$BUILD_DIR" managed_components .cache
fi

# -G "Unix Makefiles" selects GNU Make, which idf.py runs on every CPU core.
# It only matters for the first build; repeating it is harmless.
BUILD_OPTIONS=(-G "Unix Makefiles" -B "$BUILD_DIR")

# The board's -D values are kept in the CMake cache, so they are only passed
# when the build directory is first configured; idf.py would otherwise
# re-run CMake on every call.
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    BUILD_OPTIONS+=("${BOARD_DEFINES[@]}")
fi

if [ "$BUILD" = yes ]; then
    echo "==> Building on $(nproc) cores"
    idf "${BUILD_OPTIONS[@]}" build
fi

# 'flash' rebuilds first if anything is out of date.
if [ "$FLASH" = yes ]; then
    echo "==> Flashing to $PORT"
    idf "${BUILD_OPTIONS[@]}" -p "$PORT" flash
fi

if [ "$MONITOR" = yes ]; then
    echo "==> Serial monitor on $PORT (exit with Ctrl+])"
    idf -B "$BUILD_DIR" -p "$PORT" monitor
fi
