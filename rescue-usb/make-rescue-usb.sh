#!/bin/bash
# =============================================================================
# T1 (Phicomm T1, Amlogic S912) Rescue USB Disk Builder     v1.0  2026-10-12
#
# Purpose : Make / refresh the rescue USB disk: a full copy of the current
#           eMMC system that can boot the T1 on its own.
# Run at  : T1 itself, as root.  Usage: bash make-rescue-usb.sh [--yes]
#
# How it works:
#   T1 U-Boot boot order is "usb mmc" (U-Boot env: boot_order="usb mmc"; it
#   scans each device for boot.ini / boot.scr). With the rescue disk inserted
#   the T1 boots from USB; with it removed it boots from eMMC as usual.
#
# Disk layout (mirrors eMMC):
#   p1: FAT32 ~512M  LABEL=BOOT_USB    (copy of eMMC /boot + custom files)
#   p2: ext4  rest   LABEL=ROOTFS_USB  (copy of eMMC /)
#
# Custom pieces (isolation from eMMC, no conflicts):
#   - new rootfs UUID; the copied uEnv.txt APPEND points at it
#   - fstab mounts /boot by LABEL=BOOT_USB
#   - custom boot.scr (compiled from boot.cmd.usb) boots purely from USB
#   - boot.ini removed so U-Boot cannot pick the ODROID-style script first
# =============================================================================
set -euo pipefail

DISK=/dev/sda
PART1="${DISK}1"
PART2="${DISK}2"
BOOT_LABEL=BOOT_USB
ROOT_LABEL=ROOTFS_USB
STAGE="$(cd "$(dirname "$0")" && pwd)"
MNT_BOOT=/mnt/rescue-boot
MNT_ROOT=/mnt/rescue-root

log()  { echo "[$(date '+%F %T')] $*"; }
die()  { echo "ERROR: $*" >&2; exit 1; }

# --------------------------- safety gate ------------------------------------
[ "$(id -u)" = "0" ] || die "must run as root"
[ -b "$DISK" ]       || die "$DISK not found"
command -v parted  >/dev/null || die "parted not found"
command -v mkimage >/dev/null || die "mkimage not found"
[ -f "${STAGE}/boot.cmd.usb" ] || die "missing ${STAGE}/boot.cmd.usb"

TRAN="$(lsblk -no TRAN "$DISK" | head -1 | tr -d '[:space:]')"
[ "$TRAN" = "usb" ] || die "$DISK is not a USB device (TRAN='${TRAN}'), refusing"

ROOT_SRC="$(findmnt -no SOURCE /)"
case "$ROOT_SRC" in
    "${DISK}"*) die "$DISK carries the running rootfs, refusing" ;;
esac

echo "--------------------------------------------------------------"
echo "This will ERASE ${DISK}: ALL partitions and data will be lost."
echo "Target device:"
lsblk -o NAME,SIZE,FSTYPE,LABEL,MOUNTPOINT "$DISK" || true
echo "--------------------------------------------------------------"
if [ "${1:-}" != "--yes" ]; then
    read -r -p "Type YES to continue: " ans
    [ "$ans" = "YES" ] || die "aborted by user"
fi

# --------------------------- 1. unmount -------------------------------------
log "unmounting ${DISK}* ..."
while read -r mp; do
    [ -n "$mp" ] || continue
    umount "$mp" || die "cannot unmount $mp (busy?)"
done < <(lsblk -no MOUNTPOINT "$DISK")

# --------------------------- 2. partition table -----------------------------
log "writing new MBR partition table on ${DISK} ..."
parted -s "$DISK" mklabel msdos
parted -s "$DISK" mkpart primary fat32 139264s 1185791s
parted -s "$DISK" mkpart primary ext4  1638400s 100%
partprobe "$DISK" || true
udevadm settle || true
sleep 2
[ -b "$PART1" ] && [ -b "$PART2" ] || die "partition devices not ready"

# --------------------------- 3. format --------------------------------------
log "formatting ${PART1} (FAT32, ${BOOT_LABEL}) ..."
mkfs.vfat -F32 -n "$BOOT_LABEL" "$PART1" >/dev/null
ROOT_UUID="$(uuidgen | tr 'A-Z' 'a-z')"
log "formatting ${PART2} (ext4, ${ROOT_LABEL}, UUID=${ROOT_UUID}) ..."
mkfs.ext4 -q -F -L "$ROOT_LABEL" -U "$ROOT_UUID" "$PART2"

# --------------------------- 4. mount ---------------------------------------
mkdir -p "$MNT_BOOT" "$MNT_ROOT"
mount "$PART1" "$MNT_BOOT"
mount "$PART2" "$MNT_ROOT"

# --------------------------- 5. copy /boot ----------------------------------
log "copying boot partition (/boot -> ${PART1}) ..."
rsync -a --exclude='boot.ini' --exclude='lost+found' /boot/ "${MNT_BOOT}/"

# --------------------------- 6. copy rootfs ---------------------------------
log "copying rootfs (/ -> ${PART2}); takes a few minutes ..."
rsync -aHAX --numeric-ids -x --info=progress2 \
    --exclude='/lost+found/' \
    --exclude='/var/log.hdd/' \
    --exclude='/mnt/' \
    --exclude='/media/' \
    / "${MNT_ROOT}/"
# keep expected mount-point directories present
mkdir -p "${MNT_ROOT}"/{boot,proc,sys,dev,run,tmp,mnt,media,var/tmp,var/log,var/log.hdd}

# --------------------------- 7. custom files --------------------------------
log "installing custom boot files ..."

# 7a. uEnv.txt -> root=UUID of the USB rootfs; keep the original as reference
grep -q '^APPEND=root=UUID=' "${MNT_BOOT}/uEnv.txt" || die "unexpected uEnv.txt format"
sed "s|^APPEND=.*|APPEND=root=UUID=${ROOT_UUID} rootflags=data=writeback rw rootfstype=ext4 console=ttyAML0,115200n8 console=tty0 no_console_suspend consoleblank=0 fsck.fix=yes fsck.repair=yes net.ifnames=0 max_loop=128 cgroup_enable=cpuset cgroup_memory=1 cgroup_enable=memory swapaccount=1|" \
    "${MNT_BOOT}/uEnv.txt" > "${MNT_BOOT}/uEnv.txt.new"
cp -f "${MNT_BOOT}/uEnv.txt" "${MNT_BOOT}/uEnv.txt.emmc-original"
mv -f "${MNT_BOOT}/uEnv.txt.new" "${MNT_BOOT}/uEnv.txt"

# 7b. custom boot.scr compiled from boot.cmd.usb
mkimage -A arm -O linux -T script -C none -n "T1 USB rescue" \
    -d "${STAGE}/boot.cmd.usb" "${MNT_BOOT}/boot.scr" >/dev/null

# 7c. make sure no boot.ini (U-Boot tries it before boot.scr)
rm -f "${MNT_BOOT}/boot.ini"

# 7d. fstab: root + /boot follow this disk's identity
sed -i "s|^UUID=[^[:space:]]*\([[:space:]]*/[[:space:]]\)|UUID=${ROOT_UUID}\1|" "${MNT_ROOT}/etc/fstab"
sed -i "s|^LABEL=BOOT_EMMC|LABEL=${BOOT_LABEL}|" "${MNT_ROOT}/etc/fstab"

# --------------------------- 8. verify --------------------------------------
log "verifying ..."
grep -q "root=UUID=${ROOT_UUID}" "${MNT_BOOT}/uEnv.txt" || die "uEnv.txt root check failed"
grep -q "LABEL=${BOOT_LABEL}"    "${MNT_ROOT}/etc/fstab" || die "fstab LABEL check failed"
grep -q "UUID=${ROOT_UUID}"      "${MNT_ROOT}/etc/fstab" || die "fstab UUID check failed"
mkimage -l "${MNT_BOOT}/boot.scr" >/dev/null || die "boot.scr header check failed"
[ ! -e "${MNT_BOOT}/boot.ini" ] || die "boot.ini still present"

log "file-count sample (source vs usb):"
for d in /etc /usr /opt /root /var/lib; do
    a="$(find "$d" -type f 2>/dev/null | wc -l)"
    b="$(find "${MNT_ROOT}${d}" -type f 2>/dev/null | wc -l)"
    echo "    ${d}: ${a} vs ${b}"
done

log "new disk identity:"
blkid "$PART1" "$PART2"

sync

# --------------------------- 9. finish --------------------------------------
umount "$MNT_BOOT" "$MNT_ROOT"
log "DONE. Rescue USB ready."
log "Test: keep it inserted and reboot the T1 -> should boot from USB."
log "      Remove it and reboot -> boots from eMMC as usual."
