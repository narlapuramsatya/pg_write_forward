#!/usr/bin/env bash
# pwf_demo_themed.sh -- launch the themed pg_write_forward demo.
#
# Usage:
#   scripts/pwf_demo_themed.sh                 # interactive (pauses between scenes)
#   scripts/pwf_demo_themed.sh --no-pause      # straight through
#   scripts/pwf_demo_themed.sh --no-color      # plain text (e.g. piping to file)
#   scripts/pwf_demo_themed.sh --tee out.log   # interactive + transcript
#
# Connection defaults match the pwf_demo cluster (standby on :55502).
set -euo pipefail

HOST="${PWF_HOST:-/home/azureuser/pg/pwf_demo/sockets}"
PORT="${PWF_PORT:-55502}"
USER="${PWF_USER:-postgres}"
DB="${PWF_DB:-postgres}"
PSQL="${PSQL:-/home/azureuser/pg/install/bin/psql}"
SQL="$(dirname "$0")/pwf_demo_themed.sql"

PAUSE=1
COLOR=1
TEEFILE=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-pause)  PAUSE=0 ;;
        --no-color)  COLOR=0 ;;
        --tee)       shift; TEEFILE="$1" ;;
        -h|--help)
            sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
    shift
done

if [[ $COLOR -eq 1 ]]; then
    ESC=$'\033'
    RST="${ESC}[0m"
    BLD="${ESC}[1m"
    DIM="${ESC}[2m"
    RED="${ESC}[31m"
    GRN="${ESC}[32m"
    YLW="${ESC}[33m"
    BLU="${ESC}[34m"
    MAG="${ESC}[35m"
    CYN="${ESC}[36m"
    BG_BLU="${ESC}[44;97m"
    BG_GRN="${ESC}[42;30m"
    BG_RED="${ESC}[41;97m"
else
    RST=""; BLD=""; DIM=""; RED=""; GRN=""; YLW=""; BLU=""; MAG=""; CYN=""
    BG_BLU=""; BG_GRN=""; BG_RED=""
fi

CMD=( "$PSQL" -h "$HOST" -p "$PORT" -U "$USER" -d "$DB"
      --no-psqlrc
      -v ON_ERROR_STOP=1
      -v pause="$PAUSE"
      -v RST="$RST" -v BLD="$BLD" -v DIM="$DIM"
      -v RED="$RED" -v GRN="$GRN" -v YLW="$YLW"
      -v BLU="$BLU" -v MAG="$MAG" -v CYN="$CYN"
      -v BG_BLU="$BG_BLU" -v BG_GRN="$BG_GRN" -v BG_RED="$BG_RED"
      -f "$SQL" )

if [[ -n "$TEEFILE" ]]; then
    "${CMD[@]}" 2>&1 | tee "$TEEFILE"
else
    "${CMD[@]}"
fi
