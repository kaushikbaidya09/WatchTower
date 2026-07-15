#!/usr/bin/env bash

# ==============================================================================
# ESP-IDF Build Script
#
# Builds the ESP-IDF project located two directories above this script.
#
# Usage:
#   ./build.sh --esp-idf-path <path> [--target <target>]
#
# Required Arguments:
#   --esp-idf-path <path>   Path to the ESP-IDF installation.
#
# Optional Arguments:
#   --target <target>       Set the ESP chip target before building.
#   -h, --help              Show this help message.
# ==============================================================================
# END_USAGE

set -euo pipefail

usage() {
    awk '
        /^# =/{printing=1}
        /^# END_USAGE/{exit}
        printing {
            sub(/^# ?/, "")
            print
        }
    ' "$0"
}

ESP_IDF_PATH=""
TARGET=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --esp-idf-path)
            [[ $# -ge 2 ]] || {
                echo "ERROR: Missing value for --esp-idf-path"
                echo
                usage
                exit 1
            }
            ESP_IDF_PATH="$2"
            shift 2
            ;;
        --target)
            [[ $# -ge 2 ]] || {
                echo "ERROR: Missing value for --target"
                echo
                usage
                exit 1
            }
            TARGET="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "ERROR: Unknown option: $1"
            echo
            usage
            exit 1
            ;;
    esac
done

if [[ -z "$ESP_IDF_PATH" ]]; then
    echo "ERROR: --esp-idf-path is required."
    echo
    usage
    exit 1
fi

# Configuration
# ==============================================================================

# Directory containing this script
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Project directory (two levels above this script)
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Build directory
BUILD_DIR="$PROJECT_DIR/build"

# Validate ESP-IDF installation
# ==============================================================================

if [[ ! -d "$ESP_IDF_PATH" ]]; then
    echo "ERROR: ESP-IDF not found at:"
    echo "  $ESP_IDF_PATH"
    exit 1
fi

if [[ ! -f "$ESP_IDF_PATH/export.sh" ]]; then
    echo "ERROR: export.sh not found:"
    echo "  $ESP_IDF_PATH/export.sh"
    exit 1
fi

# Load ESP-IDF Environment
# ==============================================================================

echo "Loading ESP-IDF environment..."
# shellcheck source=/dev/null
source "$ESP_IDF_PATH/export.sh"

cd "$PROJECT_DIR"

# Set target
# ==============================================================================

if [[ -n "$TARGET" ]]; then
    echo "Setting target: $TARGET"
    idf.py set-target "$TARGET"
fi

# Build
# ==============================================================================

echo "Building project..."
idf.py build

echo
echo "=============================="
echo "Build completed successfully!"
echo "=============================="
