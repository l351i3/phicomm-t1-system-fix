#!/bin/bash
# =============================================================================
# T1 Rescue USB - In-Place Refresh                          v1.0  2026-10-12
#
# Updates the rescue USB disk (SanDisk Ultra 28.6G) in place:
#   - KEEPS its proven USB boot chain untouched (boot.ini / boot.scr
#     dual-device scripts, aml_autoscript / s905_autoscript factory-U-Boot
#     compat, boot-emmc.* switch scripts)
#   - refreshes BOOT partition content from eMMC (kernel / initrd / dtb /
#     uEnv) - content files only, never the boot scripts
#   - refreshes ROOTFS partition to a full copy of the running system
#   - fixes rescue-local identities (rootfs UUID stays 88e4250c..., fstab
#     /boot -> LABEL=BOOT, uEnv FDT -> -nott active variant)
#   - disables the stale armbian_first_run.txt, cleans macOS metadata
#
# Run at T1 as root:  bash rescue-refresh.sh [--yes]
#
# Target disk identity gate (hard-coded on purpose - this exact disk):
#   p1 vfat LABEL=BOOT   UUID=36AD-BCF6
#   p2 ext4 LABEL=ROOTFS UUID=88e4250c-7462-43f5-bb12-525a6e5b2b7c
# Refuses to touch anything else.
# =============================================================================
set -euo pipefail

DISK=/dev/sda
PART1="${DISK}1"
PART2="${DISK}2"
EXPECT_VFAT_UUID="36AD-BCF6"
EXPECT_ROOT_UUID="88e4250c-7462-43f5-bb12-525a6e5b2b7c"
MNT_BOOT=/mnt/rr-boot
MNT_ROOT=/mnt/rr-root
BACKUP_DIR=/root/rescue-refresh-backup-20261012

log() { echo "[$(date '+%F %T')] $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

[ "$(id -u)" = "0" ] || die "must run as root"
[ -b "$DISK" ] || die "$DISK not found"

# --- identity gate: verify this is exactly the rescue disk -------------------
V_UUID="$(blkid -s UUID -o value "$PART1" | tr 'a-z' 'A-Z')"
R_UUID="$(blkid -s UUID -o value "$PART2")"
[ "$V_UUID" = "$EXPECT_VFAT_UUID" ] || die "$PART1 UUID mismatch ($V_UUID)"
[ "$R_UUID" = "$EXPECT_ROOT_UUID" ] || die "$PART2 UUID mismatch ($R_UUID)"
ROOT_SRC="$(findmnt -no SOURCE /)"
case "$ROOT_SRC" in
    "${DISK}"*) die "$DISK carries the running rootfs, refusing" ;;
    "/dev/mmcblk2p2") : ;;
    *) die "unexpected running rootfs ($ROOT_SRC)" ;;
esac

if [ "${1:-}" != "--yes" ]; then
    echo "Refresh rescue USB $DISK in place (update contents to current system)."
    echo "Boot chain files on the disk are kept; only content is updated."
    read -r -p "Type YES to continue: " ans
    [ "$ans" = "YES" ] || die "aborted"
fi

# --- 1. unmount anything currently on the disk ------------------------------
log "unmounting ${DISK}* ..."
while read -r mp; do
    [ -n "$mp" ] || continue
    umount "$mp" || die "cannot unmount $mp (busy?)"
done < <(lsblk -no MOUNTPOINT "$DISK")

# --- 2. the old traces must have been backed up first ------------------------
if [ ! -f "$BACKUP_DIR/boot-scripts.tar.gz" ] || [ ! -f "$BACKUP_DIR/rootfs-traces.tar.gz" ]; then
    die "old-traces backups missing in $BACKUP_DIR - back up before refreshing"
fi

mkdir -p "$MNT_BOOT" "$MNT_ROOT"
mount "$PART1" "$MNT_BOOT"
mount "$PART2" "$MNT_ROOT"

# --- 3. refresh ROOTFS to a copy of the running system -----------------------
log "syncing rootfs (takes a few minutes) ..."
rsync -aHAX --numeric-ids -x --delete --info=progress2 \
    --exclude='/lost+found/' \
    --exclude='/var/log.hdd/' \
    --exclude='/mnt/' \
    --exclude='/media/' \
    / "$MNT_ROOT/"
mkdir -p "$MNT_ROOT"/{boot,proc,sys,dev,run,tmp,mnt,media,var/tmp,var/log,var/log.hdd}

# --- 4. rescue-local identities in fstab ------------------------------------
sed -i "s|^UUID=[^[:space:]]*\([[:space:]]*/[[:space:]]\)|UUID=${EXPECT_ROOT_UUID}\1|" "$MNT_ROOT/etc/fstab"
sed -i "s|^LABEL=BOOT_EMMC|LABEL=BOOT|" "$MNT_ROOT/etc/fstab"

# --- 5. refresh BOOT partition CONTENT (never the boot scripts) --------------
log "updating boot content ..."
for f in zImage uInitrd uInitrd-6.12.65-ophub System.map-6.12.65-ophub \
         config-6.12.65-ophub initrd.img-6.12.65-ophub; do
    if [ -f "/boot/$f" ]; then cp -f "/boot/$f" "$MNT_BOOT/$f"; fi
done
for d in /boot/dtb/amlogic/meson-gxm-phicomm-t1*.dtb; do
    cp -f "$d" "$MNT_BOOT/dtb/amlogic/$(basename "$d")"
done
# uEnv: FDT -> active -nott variant; keep the rescue root UUID and rootwait
sed -i "s|^FDT=/dtb/amlogic/meson-gxm-phicomm-t1\.dtb$|FDT=/dtb/amlogic/meson-gxm-phicomm-t1-nott.dtb|" "$MNT_BOOT/uEnv.txt"

# disable the stale first-run file so it cannot act on the refreshed system
if [ -f "$MNT_BOOT/armbian_first_run.txt" ]; then
    mv -f "$MNT_BOOT/armbian_first_run.txt" "$MNT_BOOT/armbian_first_run.txt.done"
fi
# clean macOS metadata from the FAT partition
rm -rf "$MNT_BOOT/.fseventsd" "$MNT_BOOT/.Spotlight-V100" "$MNT_BOOT/._armbian_first_run.txt"

# --- 6. verify ---------------------------------------------------------------
log "verifying ..."
cmp -s /boot/zImage "$MNT_BOOT/zImage" || die "zImage copy mismatch"
cmp -s /boot/uInitrd "$MNT_BOOT/uInitrd" || die "uInitrd copy mismatch"
cmp -s /boot/dtb/amlogic/meson-gxm-phicomm-t1-nott.dtb \
       "$MNT_BOOT/dtb/amlogic/meson-gxm-phicomm-t1-nott.dtb" || die "dtb copy mismatch"
grep -q '^FDT=/dtb/amlogic/meson-gxm-phicomm-t1-nott.dtb$' "$MNT_BOOT/uEnv.txt" || die "uEnv FDT check failed"
grep -q "root=UUID=${EXPECT_ROOT_UUID}" "$MNT_BOOT/uEnv.txt" || die "uEnv root check failed"
grep -q "LABEL=BOOT" "$MNT_ROOT/etc/fstab" || die "fstab LABEL check failed"
grep -q "UUID=${EXPECT_ROOT_UUID}" "$MNT_ROOT/etc/fstab" || die "fstab UUID check failed"
for s in boot.cmd boot.scr boot.ini aml_autoscript s905_autoscript boot-emmc.scr boot-emmc.ini; do
    if [ ! -f "$MNT_BOOT/$s" ]; then die "boot script missing: $s"; fi
done
if [ -f "$MNT_BOOT/armbian_first_run.txt" ]; then die "first-run file still active"; fi

log "sample file counts (source vs usb):"
for d in /etc /usr /opt /root /var/lib; do
    a="$(find "$d" -type f 2>/dev/null | wc -l)"
    b="$(find "$MNT_ROOT$d" -type f 2>/dev/null | wc -l)"
    echo "    ${d}: ${a} vs ${b}"
done

sync
umount "$MNT_BOOT" "$MNT_ROOT"
log "DONE. Rescue USB refreshed to the current system."
log "Test: keep it inserted and reboot the T1 -> should boot from USB."
log "      Remove it and reboot -> boots from eMMC as usual."
