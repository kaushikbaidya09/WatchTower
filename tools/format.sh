#!/usr/bin/env bash
#==============================================================================
# @file    format.sh
# @brief   Format the project's sources: C files with clang-format
#          (tools/config/.clang-format), and the web console in web/ with
#          Prettier (tools/config/.prettierrc).
#
# Usage:
#   tools/format.sh           Reformat every file in place
#   tools/format.sh --check   Change nothing; list files that need
#                                  formatting and exit with status 1 if any
#
# Prettier is taken from the PATH, the project's node_modules, or the VS Code
# Prettier extension. Without it the web files are skipped with a warning.
#==============================================================================

set -euo pipefail

# Project root is one level up from tools/.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STYLE="file:$ROOT/tools/config/.clang-format"
MODE="format"

case "${1:-}" in
    "")      ;;
    --check) MODE="check" ;;
    *)       echo "Usage: $0 [--check]" >&2; exit 2 ;;
esac

if ! command -v clang-format >/dev/null 2>&1; then
    echo "error: clang-format not found (e.g. 'sudo apt install clang-format')" >&2
    exit 2
fi

# Every .c and .h file in the project, skipping build output, generated docs
# and third-party code fetched by the ESP-IDF component manager.
mapfile -d '' FILES < <(find "$ROOT" \
    \( -path "$ROOT/build" -o -path "$ROOT/docs" -o -path "$ROOT/tools" -o -path "$ROOT/.git" \
       -o -path "$ROOT/managed_components" -o -path "$ROOT/.cache" \) -prune \
    -o -type f \( -name '*.c' -o -name '*.h' \) -print0 | sort -z)

if [ "${#FILES[@]}" -eq 0 ]; then
    echo "No source files found."
    exit 0
fi

CHANGED=0

for file in "${FILES[@]}"; do
    name="${file#"$ROOT"/}"

    if clang-format --style="$STYLE" "$file" | cmp -s - "$file"; then
        continue
    fi

    CHANGED=$((CHANGED + 1))

    if [ "$MODE" = "check" ]; then
        echo "needs formatting: $name"
    else
        clang-format --style="$STYLE" -i "$file"
        echo "formatted: $name"
    fi
done

# Web console
PRETTIER=()
EXT_PRETTIER="$(ls -d "$HOME"/.vscode/extensions/esbenp.prettier-vscode-*/node_modules/prettier/bin/prettier.cjs 2>/dev/null | sort -V | tail -n 1 || true)"
if command -v prettier >/dev/null 2>&1; then
    PRETTIER=(prettier)
elif [ -x "$ROOT/node_modules/.bin/prettier" ]; then
    PRETTIER=("$ROOT/node_modules/.bin/prettier")
elif [ -n "$EXT_PRETTIER" ] && command -v node >/dev/null 2>&1; then
    PRETTIER=(node "$EXT_PRETTIER")
fi

mapfile -d '' WEB < <(find "$ROOT/web" -maxdepth 1 -type f \
    \( -name '*.html' -o -name '*.css' -o -name '*.js' \) -print0 | sort -z)

if [ "${#PRETTIER[@]}" -eq 0 ]; then
    echo "warning: prettier not found, web/ not formatted (e.g. 'npm install -g prettier')" >&2
else
    PRETTIER+=(--config "$ROOT/tools/config/.prettierrc")
    for file in "${WEB[@]}"; do
        name="${file#"$ROOT"/}"
        if "${PRETTIER[@]}" --check "$file" >/dev/null 2>&1; then
            continue
        fi
        CHANGED=$((CHANGED + 1))
        if [ "$MODE" = "check" ]; then
            echo "needs formatting: $name"
        else
            "${PRETTIER[@]}" --write --log-level warn "$file"
            echo "formatted: $name"
        fi
    done
    FILES+=("${WEB[@]}")
fi

echo "Scanned ${#FILES[@]} file(s), $CHANGED $(
    [ "$MODE" = "check" ] && echo "need formatting" || echo "reformatted")."

if [ "$MODE" = "check" ] && [ "$CHANGED" -gt 0 ]; then
    exit 1
fi
