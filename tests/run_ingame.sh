#!/usr/bin/env bash
# Runs an in-game test script and takes the screenshots it asks for.
#   tests/run_ingame.sh <test.lua> <screenshot dir> [timeout seconds]
# The script is copied into <Game>\H7ModEngine\exec\; its log lines are printed. A line ending in
# "SHOT <name>" saves <screenshot dir>\<name>.png (tests\shot.ps1); a line ending in "DONE" ends the run.
set -u
GAME="/n/Ubisoft/games/Might and Magic Heroes VII/H7ModEngine"
LOG="$GAME/logs/engine.log"
TEST="$1"; SHOTS="$2"; TIMEOUT="${3:-90}"
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$SHOTS"
from=$(($(wc -l < "$LOG") + 1))
cp "$TEST" "$GAME/exec/"
end=$((SECONDS + TIMEOUT))
while [ $SECONDS -lt $end ]; do
    while IFS= read -r line; do
        from=$((from + 1))
        line=${line%$'\r'}
        echo "$line"
        case "$line" in
            *" SHOT "*)
                name=${line##* SHOT }
                powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$HERE/shot.ps1")" \
                    "$(cygpath -w "$SHOTS/$name.png")" && echo "  -> $SHOTS/$name.png";;
            *" DONE") exit 0;;
            *"exec: ERROR"*) exit 1;;
        esac
    done < <(tail -n +"$from" "$LOG")
    sleep 0.5
done
echo "timed out after ${TIMEOUT}s"
exit 1
