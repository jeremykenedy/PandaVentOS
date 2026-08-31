#!/usr/bin/env bash
#
# PandaVent OS installer.
#
# Takes a full 4 MB backup of your device FIRST, verifies it, and only then
# writes anything. The backup is not optional and there is no flag to skip it:
# BIQU publishes the application image and nothing else, so a lost bootloader
# has no public replacement, and the only thing that recovers one is an image
# taken off your own device before it was lost.
#
#   tools/install.sh                      build from source, back up, flash
#   tools/install.sh --image FILE.bin     flash a downloaded release instead
#   tools/install.sh --port /dev/cu.xxx   skip port detection
#   tools/install.sh --network 192.168.x.y  update over the network, no cable
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
BACKUP_DIR="${PV_BACKUP_DIR:-$ROOT/private/backups}"
IMAGE=""
PORT=""
HOST=""
BAUD="${PV_BAUD:-115200}"

say()  { printf '\n\033[1m%s\033[0m\n' "$*"; }
info() { printf '  %s\n' "$*"; }
die()  { printf '\n\033[1;31mstopped:\033[0m %s\n\n' "$*" >&2; exit 1; }

ask() {
    # $1 prompt. Returns 0 on yes. Anything other than an explicit yes is no.
    local reply
    printf '\n%s [y/N] ' "$1"
    read -r reply </dev/tty || true
    [[ "$reply" == "y" || "$reply" == "Y" || "$reply" == "yes" ]]
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --image)   IMAGE="${2:-}"; shift 2 ;;
        --port)    PORT="${2:-}";  shift 2 ;;
        --network) HOST="${2:-}";  shift 2 ;;
        --baud)    BAUD="${2:-}";  shift 2 ;;
        -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)         die "unknown option: $1" ;;
    esac
done

# ---------------------------------------------------------------------------
# The network path. No cable, and no full backup is possible without one, so
# it insists on a network backup instead and will not run without it.
# ---------------------------------------------------------------------------
if [[ -n "$HOST" ]]; then
    command -v curl >/dev/null || die "curl not found"
    [[ -n "$IMAGE" ]] || IMAGE="$ROOT/firmware/build/panda_vent.bin"
    [[ -f "$IMAGE" ]] || die "no image at $IMAGE. Build first, or pass --image"

    say "Reaching the device"
    curl -fsS -m 10 -o /dev/null "http://$HOST/" || die "no answer from http://$HOST/"
    info "http://$HOST/ answered"

    mkdir -p "$BACKUP_DIR"
    STAMP="$(date +%Y%m%d-%H%M%S)"
    OUT="$BACKUP_DIR/pandavent-$HOST-$STAMP-full-4MB.bin"

    say "Backing up the whole 4 MB over the network"
    info "this takes about twenty seconds"
    curl -fsS -m 180 -o "$OUT" "http://$HOST/backup" || die "backup failed, nothing was written to the device"
    SIZE=$(wc -c < "$OUT" | tr -d ' ')
    [[ "$SIZE" == "4194304" ]] || die "backup is $SIZE bytes, expected 4194304. Nothing was written to the device"
    info "saved $OUT"
    info "4194304 bytes, verified"
    printf '  \033[1;33mthis file contains your Wi-Fi password and printer access code in plain text\033[0m\n'

    say "Uploading $(basename "$IMAGE")"
    info "$(wc -c < "$IMAGE" | tr -d ' ') bytes"
    curl -fsS -m 300 -X POST -H 'X-OTA-Type: ota_fw' \
         --data-binary "@$IMAGE" "http://$HOST/ota" || die "upload failed. The device is still running the old image"

    say "Waiting for it to come back"
    info "it reboots itself, which takes about eight seconds"
    for i in $(seq 1 40); do
        sleep 2
        if curl -fsS -m 4 -o /dev/null "http://$HOST/" 2>/dev/null; then
            say "Done"
            info "http://$HOST/ is up"
            info "mDNS takes about a minute to re-register, so use the IP until then"
            exit 0
        fi
    done
    die "it did not come back within eighty seconds. Check http://$HOST/ by hand"
fi

# ---------------------------------------------------------------------------
# The cable path.
# ---------------------------------------------------------------------------
say "Checking the toolchain"
if ! python3 -c 'import esptool' 2>/dev/null; then
    info "esptool is not installed"
    ask "install it with pip?" || die "esptool is required"
    python3 -m pip install --user esptool || die "esptool install failed"
fi
info "esptool $(python3 -m esptool version 2>/dev/null | head -1 || echo present)"

if [[ -z "$PORT" ]]; then
    say "Looking for the device"
    # The Panda Vent uses a CH34x bridge. On macOS that is a cu.wchusbserial
    # node and needs the vendor VCP driver; on Linux it is ttyUSB.
    mapfile -t PORTS < <(ls /dev/cu.wchusbserial* /dev/cu.usbserial* \
                            /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || true)
    if [[ ${#PORTS[@]} -eq 0 ]]; then
        die "no serial port found.
  The Panda Vent uses a CH34x USB serial bridge. On macOS you may need the
  vendor VCP driver before a /dev/cu.wchusbserial* node appears.
  Plug it in, or pass --port explicitly."
    elif [[ ${#PORTS[@]} -eq 1 ]]; then
        PORT="${PORTS[0]}"
        info "found $PORT"
    else
        info "more than one candidate:"
        for i in "${!PORTS[@]}"; do printf '    %d) %s\n' "$((i+1))" "${PORTS[$i]}"; done
        printf '\n  which one? '
        read -r n </dev/tty
        PORT="${PORTS[$((n-1))]}" || die "no such choice"
    fi
fi
[[ -e "$PORT" ]] || die "$PORT does not exist"

# ---------------------------------------------------------------------------
# The backup. Before anything is written. Always.
# ---------------------------------------------------------------------------
mkdir -p "$BACKUP_DIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$BACKUP_DIR/pandavent-$STAMP-full-4MB.bin"

say "Backing up the whole 4 MB first"
info "offset 0 to 0x400000: bootloader, partition table, otadata, both app"
info "slots and NVS. This is the only thing that recovers a lost bootloader."
info "about three and a half minutes at $BAUD, and the vent is off the network"
info "for all of it."
ask "start the backup?" || die "nothing was written"

python3 -m esptool --chip esp32 --port "$PORT" -b "$BAUD" \
    read-flash 0 0x400000 "$OUT" || die "backup failed, nothing was written to the device"

SIZE=$(wc -c < "$OUT" | tr -d ' ')
[[ "$SIZE" == "4194304" ]] || die "backup is $SIZE bytes, expected 4194304. Nothing was written to the device"
if command -v shasum >/dev/null; then SUM=$(shasum -a 256 "$OUT" | cut -d' ' -f1)
elif command -v sha256sum >/dev/null; then SUM=$(sha256sum "$OUT" | cut -d' ' -f1)
else SUM="(no sha tool)"; fi

say "Backup complete"
info "$OUT"
info "4194304 bytes"
info "sha256 $SUM"
printf '  \033[1;33mthis file contains your Wi-Fi password and printer access code in plain text.\033[0m\n'
printf '  \033[1;33mkeep it somewhere private. Never commit it, never post it.\033[0m\n'

# ---------------------------------------------------------------------------
# Build, if we are not given an image.
# ---------------------------------------------------------------------------
if [[ -z "$IMAGE" ]]; then
    IMAGE="$ROOT/firmware/build/panda_vent.bin"
    say "Building"
    command -v idf.py >/dev/null || die "idf.py not found. Source ESP-IDF v5.3.1's export.sh, or pass --image"
    ( cd "$ROOT/firmware" && idf.py build ) || die "build failed. Your device is untouched and your backup is at $OUT"
fi
[[ -f "$IMAGE" ]] || die "no image at $IMAGE"

say "Ready to flash"
info "image  $IMAGE"
info "size   $(wc -c < "$IMAGE" | tr -d ' ') bytes"
info "port   $PORT"
info "backup $OUT"
ask "write it?" || die "nothing was written. Your backup is at $OUT"

( cd "$ROOT/firmware" && idf.py -p "$PORT" flash ) \
    || die "flash failed. Recover with:
  python3 -m esptool --chip esp32 --port $PORT -b $BAUD write-flash 0 $OUT"

say "Done"
info "power cycle it, then join the Panda_Vent_<MAC> hotspot at http://192.168.4.1"
info "or reach it on your network if it was already configured"
info "your backup is at $OUT"
