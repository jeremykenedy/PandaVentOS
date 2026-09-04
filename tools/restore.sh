#!/usr/bin/env bash
#
# PandaVentOS restorer. Puts the device back the way it was.
#
# Three different things people mean by "restore", and they need different
# tools, so pick the one that matches what went wrong.
#
#   tools/restore.sh --factory 192.168.x.y     BIQU's firmware, over the network
#   tools/restore.sh --factory --port /dev/... BIQU's firmware, over a cable
#   tools/restore.sh --image FILE.bin          your own 4 MB backup, over a cable
#   tools/restore.sh --list                    what backups you have
#
# --factory writes BIQU's published application image. That image is THEIRS and
# is not redistributed here, so you supply it yourself: put it somewhere and
# point at it with --factory-bin FILE (before --factory), or set PV_FACTORY_BIN.
# The known-good
# SHA-256 of v1.0.0 is printed below and checked before anything is written, so
# you can tell whether the file you obtained is the one this was tested against.
#
# --factory does NOT restore your settings, because the stock firmware does not
# understand the configuration this one writes and will replace it with its own
# defaults on first boot.
#
# --image writes a full 4 MB image taken off your own device: firmware,
# settings, Wi-Fi, printer binding, everything, exactly as it was at the moment
# the backup was taken. This is the one that recovers a device that will not
# boot, and the only one that can.
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
BACKUP_DIR="${PV_BACKUP_DIR:-$ROOT/private/backups}"
# BIQU's image is not shipped with this project. Point at your own copy.
FACTORY_BIN="${PV_FACTORY_BIN:-$ROOT/panda_vent_v1.0.0.bin}"
FACTORY_SHA256="0f52294e00b41524e11f11236c92aebd55d3ebb0658d2affad498929dbc0c178"
MODE=""
IMAGE=""
PORT=""
HOST=""
BAUD="${PV_BAUD:-115200}"

say()  { printf '\n\033[1m%s\033[0m\n' "$*"; }
info() { printf '  %s\n' "$*"; }
warn() { printf '  \033[1;33m%s\033[0m\n' "$*"; }
die()  { printf '\n\033[1;31mstopped:\033[0m %s\n\n' "$*" >&2; exit 1; }

ask() {
    # Defaulted, not just declared. Under `set -u` an unset `reply` is a crash,
    # and the read fails whenever there is no tty: a pipe, CI, ssh without -t.
    # Every destructive write in this script is behind this prompt, so the
    # answer to "nobody is there to say yes" has to be no, not a stack trace.
    local reply=""
    printf '\n%s [y/N] ' "$1"
    read -r reply </dev/tty || true
    [[ "$reply" == "y" || "$reply" == "Y" || "$reply" == "yes" ]]
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --factory-bin) FACTORY_BIN="${2:-}"; shift 2 ;;
        --factory) MODE="factory"
                   if [[ "${2:-}" =~ ^[0-9a-zA-Z.-]+$ && "${2:-}" != --* ]]; then HOST="$2"; shift; fi
                   shift ;;
        --image)   MODE="image"; IMAGE="${2:-}"; shift 2 ;;
        --list)    MODE="list";  shift ;;
        --port)    PORT="${2:-}"; shift 2 ;;
        --baud)    BAUD="${2:-}"; shift 2 ;;
        -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)         die "unknown option: $1" ;;
    esac
done

# ---------------------------------------------------------------------------
if [[ "$MODE" == "list" || -z "$MODE" ]]; then
    say "Backups in $BACKUP_DIR"
    if [[ -d "$BACKUP_DIR" ]] && compgen -G "$BACKUP_DIR/*.bin" >/dev/null; then
        for f in "$BACKUP_DIR"/*.bin; do
            printf '  %-52s %10s bytes\n' "$(basename "$f")" "$(wc -c < "$f" | tr -d ' ')"
        done
    else
        info "none"
    fi
    [[ -z "$MODE" ]] && { printf '\n'; sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; }
    exit 0
fi

find_port() {
    [[ -n "$PORT" ]] && return
    mapfile -t PORTS < <(ls /dev/cu.wchusbserial* /dev/cu.usbserial* \
                            /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || true)
    [[ ${#PORTS[@]} -gt 0 ]] || die "no serial port found. Plug it in, or pass --port"
    if [[ ${#PORTS[@]} -eq 1 ]]; then PORT="${PORTS[0]}"; info "found $PORT"; return; fi
    info "more than one candidate:"
    for i in "${!PORTS[@]}"; do printf '    %d) %s\n' "$((i+1))" "${PORTS[$i]}"; done
    printf '\n  which one? '
    read -r n </dev/tty
    PORT="${PORTS[$((n-1))]}" || die "no such choice"
}

need_esptool() {
    python3 -c 'import esptool' 2>/dev/null && return
    info "esptool is not installed"
    ask "install it with pip?" || die "esptool is required"
    python3 -m pip install --user esptool || die "esptool install failed"
}

# ---------------------------------------------------------------------------
# Back to BIQU's firmware.
# ---------------------------------------------------------------------------
if [[ "$MODE" == "factory" ]]; then
    if [[ ! -f "$FACTORY_BIN" ]]; then
        die "BIQU's image is not here, and this project does not ship it.

  It is their firmware, under their licence, so you supply your own copy:

      tools/restore.sh --factory-bin /path/to/image.bin --factory <host>
      PV_FACTORY_BIN=/path/to/image.bin tools/restore.sh --factory <host>

  The image this project was tested against has SHA-256

      $FACTORY_SHA256

  and that is checked before anything is written. A file that does not match
  is not refused, only reported, because a later stock release is a legitimate
  thing to want and will have a different hash.

  Looked for: $FACTORY_BIN"
    fi
    SIZE=$(wc -c < "$FACTORY_BIN" | tr -d ' ')
    GOT=$(shasum -a 256 "$FACTORY_BIN" | cut -d" " -f1)

    say "Restoring BIQU's firmware"
    info "$FACTORY_BIN"
    info "$SIZE bytes"
    if [[ "$GOT" == "$FACTORY_SHA256" ]]; then
        info "sha256 matches the tested v1.0.0 image"
    else
        warn "sha256 $GOT"
        warn "does NOT match the tested v1.0.0 image. This may be a different"
        warn "stock release, which is fine, or the wrong file, which is not."
        ask "go ahead anyway?" || die "nothing was written"
    fi
    warn "the stock firmware does not understand this one's configuration and"
    warn "will replace it with its own defaults. You will set the device up again."

    if [[ -n "$HOST" ]]; then
        command -v curl >/dev/null || die "curl not found"
        curl -fsS -m 10 -o /dev/null "http://$HOST/" || die "no answer from http://$HOST/"
        info "http://$HOST/ answered"
        ask "upload it?" || die "nothing was written"
        curl -fsS -m 300 -X POST -H 'OTA-Type: ota_fw' \
             --data-binary "@$FACTORY_BIN" "http://$HOST/ota" \
             || die "upload failed. The device is still running the old image"
        say "Waiting for it to come back"
        for i in $(seq 1 40); do
            sleep 2
            if curl -fsS -m 4 -o /dev/null "http://$HOST/" 2>/dev/null; then
                say "Done"
                info "BIQU's firmware is running"
                exit 0
            fi
        done
        die "it did not come back within eighty seconds. Check http://$HOST/ by hand"
    fi

    need_esptool; find_port
    ask "write it over $PORT?" || die "nothing was written"
    python3 -m esptool --chip esp32 --port "$PORT" -b "$BAUD" \
        write-flash 0x10000 "$FACTORY_BIN" || die "flash failed"
    say "Done"
    info "power cycle it. It comes up as a stock Panda Vent."
    exit 0
fi

# ---------------------------------------------------------------------------
# Back to a full image of your own.
# ---------------------------------------------------------------------------
if [[ "$MODE" == "image" ]]; then
    [[ -n "$IMAGE" ]] || die "--image needs a file. Run --list to see what you have"
    [[ -f "$IMAGE" ]] || { [[ -f "$BACKUP_DIR/$IMAGE" ]] && IMAGE="$BACKUP_DIR/$IMAGE"; }
    [[ -f "$IMAGE" ]] || die "no such file: $IMAGE"

    SIZE=$(wc -c < "$IMAGE" | tr -d ' ')
    [[ "$SIZE" == "4194304" ]] || die "$IMAGE is $SIZE bytes, not 4194304.
  A full image is exactly 4 MB. An application image is not a full image and
  writing one at offset 0 will destroy the bootloader."

    need_esptool; find_port

    say "Restoring a full 4 MB image"
    info "$IMAGE"
    info "4194304 bytes, offset 0"
    warn "this overwrites EVERYTHING on the device, including the settings and"
    warn "Wi-Fi it has now, and replaces them with whatever was there when this"
    warn "image was taken."
    ask "write it over $PORT?" || die "nothing was written"

    python3 -m esptool --chip esp32 --port "$PORT" -b "$BAUD" \
        write-flash 0 "$IMAGE" || die "flash failed. The device may be in a bad state.
  Try again; a failed write is recoverable as long as the same image is to hand."

    say "Done"
    info "power cycle it. It is exactly as it was when that image was taken."
    exit 0
fi
