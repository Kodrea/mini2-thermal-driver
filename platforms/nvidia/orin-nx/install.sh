#!/bin/bash
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    exec sudo "$0" "$@"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_RELEASE="$(uname -r)"
MODULE_DIR="/lib/modules/${KERNEL_RELEASE}/extra"
BOOT_DIR="/boot"
EXTLINUX_CONF="/boot/extlinux/extlinux.conf"
DTBO_NAME="rs300-cam1.dtbo"
DTBO_DEST="${BOOT_DIR}/${DTBO_NAME}"
MODPROBE_CONF="/etc/modprobe.d/rs300.conf"
VALIDATE_SCRIPT="/usr/local/sbin/rs300-validate.sh"
LOAD_SERVICE="/etc/systemd/system/rs300-load.service"
SET_DEFAULT=0
UPDATE_EXTLINUX=1
INSTALL_SERVICE=1
INSTALL_SLOT_DTB=1
VIDEO_DEV="auto"

usage() {
    cat <<'EOF'
Usage: sudo ./install.sh [options]

Options:
  --set-default       Add/update the rs300_cam1 extlinux entry and make it default.
  --no-extlinux       Do not modify /boot/extlinux/extlinux.conf.
  --no-slot-dtb       Do not update the active A/B kernel-dtb partition.
  --no-service        Do not install/enable rs300-load.service.
  --video-device DEV  Video node used by the validation service (default: auto-detect rs300).
  -h, --help          Show this help.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --set-default)
            SET_DEFAULT=1
            ;;
        --no-extlinux)
            UPDATE_EXTLINUX=0
            ;;
        --no-slot-dtb)
            INSTALL_SLOT_DTB=0
            ;;
        --no-service)
            INSTALL_SERVICE=0
            ;;
        --video-device)
            shift
            VIDEO_DEV="${1:?missing video device}"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "Missing required command: $1" >&2
        exit 1
    }
}

backup_file() {
    local path="$1"
    local stamp

    [ -f "$path" ] || return 0
    stamp="$(date -u +%Y%m%dT%H%M%SZ)"
    cp -a "$path" "${path}.bak-rs300-${stamp}"
}

extlinux_value() {
    local label="$1"
    local key="$2"
    awk -v label="$label" -v key="$key" '
        $1 == "LABEL" { in_label = ($2 == label) }
        in_label && $1 == key {
            sub("^[[:space:]]*" key "[[:space:]]+", "")
            print
            exit
        }
    ' "$EXTLINUX_CONF"
}

extlinux_has_label() {
    local label="$1"

    awk -v label="$label" '$1 == "LABEL" && $2 == label { found = 1 } END { exit(found ? 0 : 1) }' \
        "$EXTLINUX_CONF"
}

current_default_label() {
    awk '$1 == "DEFAULT" { print $2; exit }' "$EXTLINUX_CONF"
}

rewrite_extlinux_label() {
    local label="$1"
    local menu_label="$2"
    local linux="$3"
    local initrd="$4"
    local fdt="$5"
    local overlays="$6"
    local append="$7"
    local tmp

    tmp="$(mktemp)"
    awk \
        -v label="$label" \
        -v menu_label="$menu_label" \
        -v linux="$linux" \
        -v initrd="$initrd" \
        -v fdt="$fdt" \
        -v overlays="$overlays" \
        -v append="$append" '
        function emit_label() {
            print "LABEL " label
            print "      MENU LABEL " menu_label
            print "      LINUX " linux
            print "      INITRD " initrd
            if (fdt != "")
                print "      FDT " fdt
            if (overlays != "")
                print "      OVERLAYS " overlays
            print "      APPEND " append
        }

        $1 == "LABEL" {
            if (in_target) {
                emit_label()
                in_target = 0
                emitted = 1
            }
            if ($2 == label) {
                in_target = 1
                next
            }
        }

        !in_target { print }

        END {
            if (in_target) {
                emit_label()
                emitted = 1
            } else if (!emitted) {
                print ""
                emit_label()
            }
        }
    ' "$EXTLINUX_CONF" > "$tmp"
    cat "$tmp" > "$EXTLINUX_CONF"
    rm -f "$tmp"
}

install_extlinux_entry() {
    local default_label source_label linux initrd fdt append overlays rs300_fdt
    local primary_linux primary_initrd primary_append primary_overlays

    [ -f "$EXTLINUX_CONF" ] || {
        echo "No extlinux.conf at $EXTLINUX_CONF; skipping boot entry." >&2
        return 0
    }

    default_label="$(current_default_label)"
    [ -n "$default_label" ] || default_label="primary"
    source_label="$default_label"
    if [ "$source_label" = "rs300_cam1" ] && extlinux_has_label primary; then
        source_label="primary"
    fi

    linux="$(extlinux_value "$source_label" LINUX)"
    initrd="$(extlinux_value "$source_label" INITRD)"
    fdt="$(extlinux_value "$source_label" FDT)"
    append="$(extlinux_value "$source_label" APPEND)"
    overlays="$(extlinux_value "$source_label" OVERLAYS)"
    rs300_fdt="$(extlinux_value rs300_cam1_dtb FDT)"
    [ -n "$rs300_fdt" ] || rs300_fdt="$fdt"

    [ -n "$linux" ] || linux="/boot/Image"
    [ -n "$initrd" ] || initrd="/boot/initrd"
    [ -n "$append" ] || append='${cbootargs} root=/dev/nvme0n1p1 rw rootwait rootfstype=ext4'

    case " ${overlays} " in
        *" ${DTBO_DEST} "*) ;;
        *) overlays="${overlays:+$overlays }${DTBO_DEST}" ;;
    esac

    backup_file "$EXTLINUX_CONF"
    rewrite_extlinux_label \
        "rs300_cam1" \
        "RS300/WN2640 CAM1 thermal camera: 640x512, 2 lanes, 80 MHz" \
        "$linux" \
        "$initrd" \
        "$rs300_fdt" \
        "$overlays" \
        "$append"

    if [ "$SET_DEFAULT" -eq 1 ]; then
        sed -i 's/^DEFAULT .*/DEFAULT rs300_cam1/' "$EXTLINUX_CONF"

        if extlinux_has_label primary; then
            primary_linux="$(extlinux_value primary LINUX)"
            primary_initrd="$(extlinux_value primary INITRD)"
            primary_append="$(extlinux_value primary APPEND)"
            primary_overlays="$(extlinux_value primary OVERLAYS)"

            [ -n "$primary_linux" ] || primary_linux="$linux"
            [ -n "$primary_initrd" ] || primary_initrd="$initrd"
            [ -n "$primary_append" ] || primary_append="$append"

            case " ${primary_overlays} " in
                *" ${DTBO_DEST} "*) ;;
                *) primary_overlays="${primary_overlays:+$primary_overlays }${DTBO_DEST}" ;;
            esac

            rewrite_extlinux_label \
                "primary" \
                "primary kernel with RS300/WN2640 CAM1 thermal camera" \
                "$primary_linux" \
                "$primary_initrd" \
                "$rs300_fdt" \
                "$primary_overlays" \
                "$primary_append"
        fi
    fi
}

find_rs300_base_dtb() {
    local fdt

    fdt="$(extlinux_value rs300_cam1 FDT || true)"
    [ -n "$fdt" ] || fdt="$(extlinux_value rs300_cam1_dtb FDT || true)"
    [ -n "$fdt" ] || fdt="${BOOT_DIR}/kernel_tegra234-auvidea-jnx120x+p3767-0000-nv-rs300-cam1.dtb"

    [ -f "$fdt" ] || {
        echo "Could not find an RS300 base DTB. Expected $fdt" >&2
        return 1
    }

    printf '%s\n' "$fdt"
}

active_slot_dtb_partlabel() {
    local slot

    slot="$(nvbootctrl get-current-slot 2>/dev/null || true)"
    case "$slot" in
        0|A|a) printf '%s\n' "A_kernel-dtb" ;;
        1|B|b) printf '%s\n' "B_kernel-dtb" ;;
        *)
            echo "Could not determine active boot slot from nvbootctrl output: ${slot:-<empty>}" >&2
            return 1
            ;;
    esac
}

install_active_slot_dtb() {
    local base_dtb merged_dtb partlabel part size part_size backup_dir backup

    need_cmd fdtoverlay
    need_cmd fdtget
    need_cmd nvbootctrl
    need_cmd blockdev
    need_cmd dd

    base_dtb="$(find_rs300_base_dtb)"
    merged_dtb="${BOOT_DIR}/rs300-cam1-active.dtb"
    partlabel="$(active_slot_dtb_partlabel)"
    part="/dev/disk/by-partlabel/${partlabel}"

    [ -e "$part" ] || {
        echo "Active DTB partition not found: $part" >&2
        return 1
    }

    fdtoverlay -i "$base_dtb" -o "$merged_dtb" "$DTBO_DEST"
    fdtget "$merged_dtb" /bus@0/cam_i2cmux/i2c@1/rs300@3c compatible >/dev/null
    fdtget "$merged_dtb" /bus@0/cam_i2cmux/i2c@1/rs300@3c power-gpios >/dev/null

    size="$(stat -c%s "$merged_dtb")"
    part_size="$(blockdev --getsize64 "$part")"
    if [ "$size" -gt "$part_size" ]; then
        echo "Merged DTB (${size} bytes) is larger than ${part} (${part_size} bytes)" >&2
        return 1
    fi

    backup_dir="${BOOT_DIR}/rs300-backups"
    mkdir -p "$backup_dir"
    backup="${backup_dir}/${partlabel}-$(date -u +%Y%m%dT%H%M%SZ).dtb"
    dd if="$part" of="$backup" bs=1M status=none
    chmod 0644 "$backup"

    dd if="$merged_dtb" of="$part" bs=4K conv=fsync status=none
    echo "Installed merged RS300 DTB to ${part} (${partlabel}); backup: ${backup}"
}

install_validation_service() {
    cat >"$VALIDATE_SCRIPT" <<'EOF'
#!/bin/sh
set -u

DEV_ARG="${1:-auto}"
WIDTH=640
HEIGHT=512
PIX=UYVY
VALID_FRAMES=30
EXPECT=$((WIDTH * HEIGHT * 2 * VALID_FRAMES))
FRAME_SIZE=$((WIDTH * HEIGHT * 2))
CHECK_FRAME=10
OUT=/run/rs300-validate.raw

log() {
    logger -t rs300-validate "$*"
    echo "rs300-validate: $*"
}

find_rs300_video() {
    for candidate in /dev/video*; do
        [ -e "$candidate" ] || continue

        if v4l2-ctl -d "$candidate" --info 2>/dev/null |
            grep -Eiq 'Card type[[:space:]]*:.*(rs300|wn2640)'; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done

    return 1
}

frame_has_signal() {
    dd if="$OUT" bs="$FRAME_SIZE" skip="$CHECK_FRAME" count=1 2>/dev/null |
        od -An -tu1 -v |
        awk '
            { for (i = 1; i <= NF; i++) c[$i] = 1 }
            END { exit(length(c) > 10 ? 0 : 1) }
        '
}

wait_dev=0
if [ "$DEV_ARG" = "auto" ] || [ -z "$DEV_ARG" ]; then
    DEV=""
    while [ "$wait_dev" -lt 30 ]; do
        DEV="$(find_rs300_video || true)"
        [ -n "$DEV" ] && break
        sleep 1
        wait_dev=$((wait_dev + 1))
    done

    if [ -z "$DEV" ]; then
        log "could not auto-detect rs300 video node"
        exit 1
    fi
else
    DEV="$DEV_ARG"
    while [ ! -e "$DEV" ] && [ "$wait_dev" -lt 30 ]; do
        sleep 1
        wait_dev=$((wait_dev + 1))
    done

    if [ ! -e "$DEV" ]; then
        log "device $DEV did not appear"
        exit 1
    fi
fi

log "using $DEV"

for attempt in 1 2 3; do
    log "attempt $attempt: validating $DEV"
    rm -f "$OUT"

    timeout 35s v4l2-ctl -d "$DEV" \
        --set-fmt-video=width=${WIDTH},height=${HEIGHT},pixelformat=${PIX} \
        --stream-mmap=4 --stream-count=${VALID_FRAMES} --stream-to="$OUT" >/dev/null 2>&1
    rc=$?
    size=0
    [ -f "$OUT" ] && size=$(stat -c%s "$OUT" 2>/dev/null || echo 0)

    if [ "$rc" -eq 0 ] && [ "$size" -eq "$EXPECT" ] && frame_has_signal; then
        log "ready after attempt $attempt: ${VALID_FRAMES} frames, ${size} bytes"
        rm -f "$OUT"
        exit 0
    fi

    log "validation failed on attempt $attempt: rc=$rc size=$size expected=$EXPECT or frame ${CHECK_FRAME} is blank"
    rm -f "$OUT"
    sleep 5
done

log "camera did not validate after 3 attempts"
exit 1
EOF
    chmod 0755 "$VALIDATE_SCRIPT"

    cat >"$LOAD_SERVICE" <<EOF
[Unit]
Description=Load and validate RS300/WN2640 CAM1 thermal camera
After=systemd-udev-settle.service

[Service]
Type=oneshot
ExecStart=/sbin/modprobe rs300
ExecStartPost=${VALIDATE_SCRIPT} ${VIDEO_DEV}
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF

    systemctl daemon-reload
    systemctl enable rs300-load.service
}

need_cmd make
need_cmd install
need_cmd depmod
need_cmd dtc

cd "$SCRIPT_DIR"

make clean
make
make dtbo

install -D -m 0644 rs300.ko "${MODULE_DIR}/rs300.ko"
install -D -m 0644 "${DTBO_NAME}" "$DTBO_DEST"
depmod -a

cat >"$MODPROBE_CONF" <<'EOF'
blacklist rs300
options rs300 mode=0 fps=60 set_fps_cmd=0 type=16 output_source=-1 yuv_order=2 start_path=1 start_dst=1 start_width=0 start_height=0 advertise_width=0 advertise_height=0 power_on_delay_ms=8000 auto_shutter=0 startup_ffc=0 native_y16=0
EOF

if [ "$INSTALL_SERVICE" -eq 1 ]; then
    install_validation_service
fi

if [ "$UPDATE_EXTLINUX" -eq 1 ]; then
    install_extlinux_entry
fi

if [ "$INSTALL_SLOT_DTB" -eq 1 ]; then
    install_active_slot_dtb
fi

systemctl disable --now cam1-pac00-high.service >/dev/null 2>&1 || true

echo "Installed rs300.ko, ${DTBO_DEST}, and ${MODPROBE_CONF}."
if [ "$UPDATE_EXTLINUX" -eq 1 ]; then
    echo "Installed/updated extlinux label: rs300_cam1"
    [ "$SET_DEFAULT" -eq 1 ] && echo "Set DEFAULT rs300_cam1."
fi
if [ "$INSTALL_SLOT_DTB" -eq 1 ]; then
    echo "Updated active Jetson kernel-dtb partition with merged RS300 DTB."
fi
if [ "$INSTALL_SERVICE" -eq 1 ]; then
    echo "Enabled rs300-load.service with ${VIDEO_DEV} validation."
fi
echo "Reboot, then check: systemctl status rs300-load.service"
