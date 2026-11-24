#!/system/bin/sh

# sparsemgr.sh - Generic Sparse ext4 Image Manager
# Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Handles migration from directory-based rootfs to sparse ext4 images,
# and resizing of existing sparse images.

SCRIPT_NAME="$(basename "$0")"

# BusyBox Detection
_find_busybox() {
    if [ -x "/data/local/Droidspaces/bin/busybox" ]; then
        echo "/data/local/Droidspaces/bin/busybox"
        return
    fi
    local _p
    _p="$(command -v busybox 2>/dev/null)"
    [ -n "$_p" ] && { echo "$_p"; return; }
    _p="$(which busybox 2>/dev/null)"
    [ -n "$_p" ] && { echo "$_p"; return; }
    echo ""
}
BB=$(_find_busybox)

# Logging
log()   { echo "[SPARSE] $1"; }
warn()  { echo "[WARN]   $1"; }
error() { echo "[ERROR]  $1"; }


# Portable Tool Wrappers

# truncate -s <size> <file>
_truncate() {
    # BusyBox first for the same reason as _mount: one implementation that behaves
    # the same on every device. Both are a plain ftruncate and produce a
    # byte-identical sparse file, so there is nothing to lose by preferring ours.
    if [ -n "$BB" ] && "$BB" truncate -s "$1" "$2" 2>/dev/null; then
        return 0
    elif truncate -s "$1" "$2" 2>/dev/null; then
        return 0
    fi
    error "truncate: neither system nor busybox truncate succeeded"
    return 1
}

# mount wrapper - passes all args through, tries busybox then system
_mount() {
    # BusyBox first: Android's mount is toybox, whose losetup aborts rather than
    # truncating when a backing path exceeds 63 characters, so it cannot loop-mount
    # an image stored on a deep custom path.
    if [ -n "$BB" ] && "$BB" mount "$@" 2>/dev/null; then
        return 0
    elif mount "$@" 2>/dev/null; then
        return 0
    fi
    error "mount: failed with args: $*"
    return 1
}

# umount wrapper
_umount() {
    # Same order as _mount, so whatever attached the loop device also detaches it.
    if [ -n "$BB" ] && "$BB" umount "$@" 2>/dev/null; then
        return 0
    elif umount "$@" 2>/dev/null; then
        return 0
    fi
    return 1
}

# mountpoint check (returns 0 if mounted)
_is_mounted() {
    local path="$1"
    if [ -n "$BB" ] && "$BB" mountpoint -q "$path" 2>/dev/null; then
        return 0
    elif mountpoint -q "$path" 2>/dev/null; then
        return 0
    fi
    # Fallback: parse /proc/mounts
    grep -q " $path " /proc/mounts 2>/dev/null
}

# stat file size in bytes
_stat_size() {
    local out
    out=$(stat -c%s "$1" 2>/dev/null) && [ -n "$out" ] && { echo "$out"; return; }
    [ -n "$BB" ] && out=$("$BB" stat -c%s "$1" 2>/dev/null) && [ -n "$out" ] && { echo "$out"; return; }
    # last resort: wc
    wc -c < "$1" 2>/dev/null || echo "0"
}

# du actual disk usage
_du_h() {
    local out
    out=$(du -h "$1" 2>/dev/null | cut -f1) && [ -n "$out" ] && { echo "$out"; return; }
    [ -n "$BB" ] && out=$("$BB" du -h "$1" 2>/dev/null | cut -f1) && [ -n "$out" ] && { echo "$out"; return; }
    echo "unknown"
}

# du actual disk usage in KB, 0 when it cannot be measured
_du_kb() {
    local out
    out=$(du -sk "$1" 2>/dev/null | cut -f1) && [ -n "$out" ] && { echo "$out"; return; }
    [ -n "$BB" ] && out=$("$BB" du -sk "$1" 2>/dev/null | cut -f1) && [ -n "$out" ] && { echo "$out"; return; }
    echo "0"
}

# portable sync + sleep
_sync()  { sync 2>/dev/null || { [ -n "$BB" ] && "$BB" sync 2>/dev/null; } || true; }
_sleep() { sleep "$1" 2>/dev/null || { [ -n "$BB" ] && "$BB" sleep "$1" 2>/dev/null; } || true; }

# mkdir -p wrapper
_mkdir() { mkdir -p "$@" 2>/dev/null || { [ -n "$BB" ] && "$BB" mkdir -p "$@" 2>/dev/null; }; }

# rm wrapper
_rm()    { rm "$@" 2>/dev/null || { [ -n "$BB" ] && "$BB" rm "$@" 2>/dev/null; } || true; }

# mv wrapper
_mv()    { mv "$@" 2>/dev/null || { [ -n "$BB" ] && "$BB" mv "$@" 2>/dev/null; }; }

# tar: prefer busybox tar for Android compat, fall back to system tar
_tar_create() {
    # _tar_create <source_dir>   - writes to stdout
    if [ -n "$BB" ] && "$BB" tar --help >/dev/null 2>&1; then
        (cd "$1" && "$BB" tar -cf - .)
    else
        (cd "$1" && tar -cf - .)
    fi
}
_tar_extract() {
    # _tar_extract <dest_dir>   - reads from stdin
    if [ -n "$BB" ] && "$BB" tar --help >/dev/null 2>&1; then
        (cd "$1" && "$BB" tar -xf -)
    else
        (cd "$1" && tar -xf -)
    fi
}

# mkfs.ext4 wrapper
_mkfs_ext4() {
    local img="$1"
    if command -v mkfs.ext4 >/dev/null 2>&1; then
        mkfs.ext4 -F -L "rootfs" "$img" >/dev/null
        return $?
    elif command -v mke2fs >/dev/null 2>&1; then
        mke2fs -t ext4 -F -L "rootfs" "$img" >/dev/null
        return $?
    fi
    # Deliberately no busybox fallback: its mke2fs rejects -t and builds bare ext2,
    # with no has_journal, no extent and no ext_attr, so a rootfs formatted by it
    # cannot hold SELinux labels or file capabilities. The ext4 driver would mount
    # it anyway, turning a clear failure into a subtly broken container.
    error "No ext4 formatting tool found (tried mkfs.ext4, mke2fs)"
    return 1
}

# Requirement Checks
check_migrate_requirements() {
    log "Checking tools for migrate..."
    local fail=0

    if [ -z "$BB" ]; then
        warn "busybox not found at default path or in PATH - some fallbacks unavailable"
    else
        log "busybox   : $BB"
    fi

    local has_mkfs=0
    command -v mkfs.ext4 >/dev/null 2>&1 && has_mkfs=1
    command -v mke2fs    >/dev/null 2>&1 && has_mkfs=1
    # No busybox probe here: _mkfs_ext4 deliberately does not fall back to it, so
    # counting it would report ok on a device where formatting then fails.
    if [ "$has_mkfs" -eq 0 ]; then
        error "mkfs.ext4 / mke2fs not found"
        fail=1
    else
        log "mkfs.ext4 : ok"
    fi

    local has_tar=0
    command -v tar >/dev/null 2>&1 && has_tar=1
    [ -n "$BB" ] && "$BB" tar --help >/dev/null 2>&1 && has_tar=1
    if [ "$has_tar" -eq 0 ]; then
        error "tar not found"
        fail=1
    else
        log "tar       : ok"
    fi

    [ "$fail" -ne 0 ] && exit 1
    log "All migrate requirements met"
}

check_resize_requirements() {
    log "Checking tools for resize..."
    local fail=0

    command -v e2fsck >/dev/null 2>&1    || { error "e2fsck not found";    fail=1; }
    command -v resize2fs >/dev/null 2>&1 || { error "resize2fs not found"; fail=1; }

    [ "$fail" -ne 0 ] && { error "Install e2fsprogs and retry"; exit 1; }
    log "All resize requirements met"
}

check_reclaim_requirements() {
    log "Checking tools for reclaim..."
    local fail=0

    command -v tune2fs >/dev/null 2>&1 || { error "tune2fs not found";  fail=1; }
    command -v losetup >/dev/null 2>&1 || { error "losetup not found";  fail=1; }
    if [ -z "$BB" ] && ! command -v fstrim >/dev/null 2>&1; then
        error "neither busybox nor fstrim is available"
        fail=1
    fi

    [ "$fail" -ne 0 ] && { error "Reclaim cannot run on this device"; exit 1; }
}

# migrate: cleanup on error
_migrate_cleanup() {
    warn "Error during migration - cleaning up temp files..."
    _is_mounted "$ROOTFS_SPARSE" && {
        _umount "$ROOTFS_SPARSE" 2>/dev/null ||
        _umount -f "$ROOTFS_SPARSE" 2>/dev/null ||
        _umount -l "$ROOTFS_SPARSE" 2>/dev/null
    }
    _rm -rf "$ROOTFS_SPARSE"
    _rm -f  "${ROOTFS_IMG}"
    log "Cleanup done - original rootfs is preserved"
}

# CMD: migrate
cmd_migrate() {
    local size_input="$1"
    local size_gb
    size_gb=$(echo "$size_input" | sed 's/[^0-9]//g')

    # Validate size
    if [ -z "$size_gb" ]; then
        error "Invalid size: '$size_input'"
        echo "Example: $SCRIPT_NAME -d /path/to/dir migrate 16"
        exit 1
    fi
    if [ "$size_gb" -lt 1 ] || [ "$size_gb" -gt 512 ]; then
        error "Size must be between 1 and 512 GB (got: ${size_gb}GB)"
        exit 1
    fi

    # Validate paths
    if [ ! -d "$ROOTFS_DIR" ]; then
        error "rootfs directory not found: $ROOTFS_DIR"
        exit 1
    fi
    if [ -z "$(ls -A "$ROOTFS_DIR" 2>/dev/null)" ]; then
        error "rootfs directory is empty: $ROOTFS_DIR"
        exit 1
    fi
    if [ -f "$ROOTFS_IMG" ]; then
        error "Sparse image already exists: $ROOTFS_IMG"
        error "Use 'resize' to change its size, or remove it manually to re-migrate"
        exit 1
    fi
    if [ -d "$ROOTFS_SPARSE" ]; then
        error "Temp migration directory already exists: $ROOTFS_SPARSE"
        error "Remove it manually before retrying"
        exit 1
    fi

    # Refuse if rootfs dir is currently mounted
    if _is_mounted "$ROOTFS_DIR"; then
        error "$ROOTFS_DIR is currently mounted"
        error "Unmount it first before running migrate"
        exit 1
    fi

    check_migrate_requirements

    # A size that cannot hold what is about to be copied used to fail halfway
    # through the tar, after the image was formatted. Now that the floor is 1GB,
    # check it first: the copy needs the source's disk usage plus room for ext4
    # metadata, so 90% of the image is the budget. Failing here costs nothing.
    local src_kb budget_kb
    src_kb=$(_du_kb "$ROOTFS_DIR")
    budget_kb=$(_awk -v g="$size_gb" 'BEGIN{printf "%d", g*1024*1024*90/100}')
    if [ "$src_kb" -gt 0 ] && [ "$src_kb" -gt "$budget_kb" ]; then
        error "Rootfs needs about $(( src_kb / 1024 )) MB but ${size_gb}GB leaves only $(( budget_kb / 1024 )) MB"
        error "Pick a larger size, or clean up the rootfs first"
        exit 1
    fi

    log " Migration: directory -> sparse image"
    log "   Source  : $ROOTFS_DIR"
    log "   Image   : $ROOTFS_IMG"
    log "   Size    : ${size_gb}GB"

    trap _migrate_cleanup EXIT

    # Step 1: Create sparse file
    log "[1/5] Creating ${size_gb}GB sparse file..."
    if ! _truncate "${size_gb}G" "$ROOTFS_IMG"; then
        exit 1
    fi
    _sync; _sleep 2

    if [ ! -f "$ROOTFS_IMG" ]; then
        error "Sparse file was not created"
        exit 1
    fi
    local fsz
    fsz=$(_stat_size "$ROOTFS_IMG")
    if [ "$fsz" = "0" ] || [ -z "$fsz" ]; then
        error "Created file has zero size - disk full?"
        exit 1
    fi
    log "    File size: $fsz bytes ✓"

    # Step 2: Format as ext4
    log "[2/5] Formatting as ext4..."
    _sync; _sleep 1
    if ! _mkfs_ext4 "$ROOTFS_IMG"; then
        exit 1
    fi
    log "    ext4 format complete ✓"

    # Step 3: Mount sparse image to temp dir
    log "[3/5] Mounting sparse image..."
    _mkdir "$ROOTFS_SPARSE"
    chcon u:object_r:vold_data_file:s0 "$ROOTFS_IMG" 2>/dev/null || true
    if ! _mount -t ext4 -o loop,rw,noatime,nodiratime,data=ordered,commit=30 \
            "$ROOTFS_IMG" "$ROOTFS_SPARSE"; then
        exit 1
    fi
    log "    Mounted at $ROOTFS_SPARSE ✓"

    # Step 4: Copy rootfs via tar pipe
    log "[4/5] Copying rootfs (this may take a while)..."
    if ! _tar_create "$ROOTFS_DIR" | _tar_extract "$ROOTFS_SPARSE"; then
        error "tar pipe failed - rootfs copy incomplete"
        exit 1
    fi
    log "    Copy complete ✓"

    # Step 5: Finalize
    log "[5/5] Finalizing..."
    _sync
    if ! _umount "$ROOTFS_SPARSE" 2>/dev/null; then
        _umount -l "$ROOTFS_SPARSE" 2>/dev/null || {
            error "Failed to unmount sparse image"
            exit 1
        }
    fi

    local backup_dir="${ROOTFS_DIR}.bak"
    _mv "$ROOTFS_DIR" "$backup_dir"   || { error "Failed to rename original rootfs"; exit 1; }
    _mv "$ROOTFS_SPARSE" "$ROOTFS_DIR" || {
        error "Failed to rename sparse mount dir"
        _mv "$backup_dir" "$ROOTFS_DIR" 2>/dev/null
        exit 1
    }

    # NixOS marks /var/empty immutable, so not even root can delete it as it is. A mount
    # left under here would let chattr unprotect host files, so then leave the flags alone.
    { [ -z "$BB" ] || "$BB" grep -qF " $backup_dir/" /proc/mounts ||
        "$BB" chattr -R -ia "$backup_dir"; } >/dev/null 2>&1
    _rm -rf "$backup_dir"
    trap - EXIT

    log ""
    log "✅ Migration complete!"
    log "   Sparse image : $ROOTFS_IMG  (${size_gb}GB)"
    log "   Rootfs dir   : $ROOTFS_DIR"
}

# CMD: resize
cmd_resize() {
    local new_size_gb="$1"

    # Validate input
    if [ -z "$new_size_gb" ]; then
        error "No size specified"
        echo "Usage: $SCRIPT_NAME -i <image> resize <size_in_gb>"
        exit 1
    fi
    if ! [ "$new_size_gb" -eq "$new_size_gb" ] 2>/dev/null || [ "$new_size_gb" -le 0 ]; then
        error "Invalid size: '$new_size_gb' - must be a positive integer"
        exit 1
    fi
    if [ "$new_size_gb" -lt 1 ] || [ "$new_size_gb" -gt 512 ]; then
        error "Size must be between 1 and 512 GB"
        exit 1
    fi
    if [ ! -f "$RESIZE_IMG" ]; then
        error "Sparse image not found: $RESIZE_IMG"
        error "Run 'migrate' first to create a sparse image"
        exit 1
    fi

    check_resize_requirements

    # Check if the image is currently loop-mounted
    if grep -q "$RESIZE_IMG" /proc/mounts 2>/dev/null; then
        log "Image is currently mounted - unmounting..."
        local mount_point
        mount_point=$(grep "$RESIZE_IMG" /proc/mounts 2>/dev/null | awk '{print $2}' | head -1)
        if [ -n "$mount_point" ]; then
            _umount -f "$mount_point" 2>/dev/null ||
            _umount -l "$mount_point" 2>/dev/null || {
                error "Failed to unmount $mount_point - stop any processes using the image first"
                exit 1
            }
            _sleep 1
            log "    Unmounted $mount_point ✓"
        fi
    fi

    # Gather size info
    local actual_size sparse_size
    actual_size=$(_du_h "$RESIZE_IMG")
    sparse_size=$(ls -lh "$RESIZE_IMG" 2>/dev/null | tr -s ' ' | cut -d' ' -f5)
    [ -z "$sparse_size" ] && sparse_size="unknown"

    # No minimum or truncate of our own. resize2fs computes the real minimum from
    # the fs metadata and refuses below it (we never pass -f), and on an image
    # file it extends or ftruncates the file itself.

    log " Resize: $RESIZE_IMG"
    echo "   Image size           : $sparse_size"
    echo "   Allocated on disk    : $actual_size"
    echo "   Requested size       : ${new_size_gb}G"

    # Confirmation (skipped with -y / --yes)
    if [ "$YES_FLAG" -eq 0 ]; then
        warn "WARNING: Resizing a sparse image is a destructive operation"
        warn "  that CAN CORRUPT your filesystem if interrupted."
        warn "  Make a FULL BACKUP before proceeding."
        warn ""
        warn "  Resize : $sparse_size -> ${new_size_gb}G"
        printf "  Type 'YES' to confirm: "
        read -r _confirm
        [ "$_confirm" != "YES" ] && { log "Resize cancelled"; exit 0; }
    fi

    # Filesystem check
    log "[1/3] Checking filesystem integrity..."
    local fsck_out fsck_exit
    fsck_out=$(e2fsck -f -y "$RESIZE_IMG" 2>&1 >/dev/null)
    fsck_exit=$?
    # e2fsck exit codes: 0=clean, 1=corrected, 2=corrected+reboot, 4+=error
    if [ $fsck_exit -ge 4 ]; then
        error "Filesystem check FAILED (exit: $fsck_exit)"
        error "$fsck_out"
        error "Do NOT proceed - restore from backup"
        exit 1
    fi
    [ $fsck_exit -ne 0 ] && warn "e2fsck corrected issues (exit: $fsck_exit) - continuing"
    [ $fsck_exit -eq 0 ] && log "    Filesystem clean ✓"

    # Resize filesystem
    log "[2/3] Resizing filesystem to ${new_size_gb}G..."
    local resize_out resize_exit
    resize_out=$(resize2fs "$RESIZE_IMG" "${new_size_gb}G" 2>&1 >/dev/null)
    resize_exit=$?
    # resize2fs may exit non-zero but print "is now N blocks long" which means success
    if [ $resize_exit -ne 0 ] && ! echo "$resize_out" | grep -q "is now.*blocks long"; then
        error "resize2fs FAILED (exit: $resize_exit)"
        error "$resize_out"
        # resize2fs checks its minimum before touching anything, so this one is harmless
        if echo "$resize_out" | grep -q "smaller than minimum"; then
            error "Too small for the data in this image. Nothing was changed, pick a larger size."
            exit 1
        fi
        error "Restore from backup immediately!"
        exit 1
    fi
    [ $resize_exit -ne 0 ] && warn "resize2fs exited $resize_exit but reported success - continuing"
    log "    Filesystem resized ✓"

    # Verify by test-mounting to a temp directory
    log "[3/3] Verifying resized filesystem..."
    local verify_dir
    verify_dir="$(dirname "$RESIZE_IMG")/.resize_verify_$$"
    _mkdir "$verify_dir"
    if _mount -t ext4 -o loop,ro "$RESIZE_IMG" "$verify_dir" 2>/dev/null; then
        _umount "$verify_dir" 2>/dev/null
        _rm -rf "$verify_dir"
        log "    Mount verification passed ✓"
    else
        _rm -rf "$verify_dir"
        error "Failed to mount resized filesystem - possible corruption!"
        error "Restore from backup immediately!"
        exit 1
    fi

    _sleep 1
    local new_sparse
    new_sparse=$(ls -lh "$RESIZE_IMG" 2>/dev/null | tr -s ' ' | cut -d' ' -f5)

    log ""
    log "Resize complete!"
    log "   $sparse_size -> ${new_sparse}"
    log "   Image: $RESIZE_IMG"
}


# reclaim helpers. Every size here is a byte count of an image that can reach
# hundreds of GB, and the device shell's arithmetic is 32-bit: an 8 GB image is
# 2^33 bytes, which wraps to 0 and once made every container look minimal. So
# no byte count is ever arithmetic'd by the shell itself.
_awk() {
    if command -v awk >/dev/null 2>&1; then
        awk "$@"
    elif [ -n "$BB" ]; then
        "$BB" awk "$@"
    fi
}

# MB of a byte count
_bytes_mb() { _awk -v b="${1:-0}" 'BEGIN{printf "%d", b/1048576}'; }

# KB the image really occupies on the host. Sparse holes and f2fs compression
# both keep it below the file's size, and this is the number reclaim has to
# move: it is what a file manager shows.
_alloc_kb() {
    local b
    b=$(stat -c %b "$1" 2>/dev/null)
    if [ -z "$b" ] && [ -n "$BB" ]; then
        b=$("$BB" stat -c %b "$1" 2>/dev/null)
    fi
    [ -n "$b" ] || { echo 0; return; }
    _awk -v b="$b" 'BEGIN{printf "%d", b/2}'
}

# KB the filesystem counts as used, read from the on-disk superblock. Only valid
# for a stopped container: a mounted ext4 keeps its accounting in memory and
# writes the superblock back on checkpoint or unmount, so this trails whatever
# the journal has not flushed yet. Use _fs_used_kb_running for a live one.
_fs_used_kb() {
    local info count free bs
    info=$(tune2fs -l "$1" 2>/dev/null)
    count=$(printf '%s\n' "$info" | sed -n 's/^Block count: *\([0-9][0-9]*\).*/\1/p' | head -1)
    free=$(printf '%s\n' "$info" | sed -n 's/^Free blocks: *\([0-9][0-9]*\).*/\1/p' | head -1)
    bs=$(printf '%s\n' "$info" | sed -n 's/^Block size: *\([0-9][0-9]*\).*/\1/p' | head -1)
    [ -n "$count" ] && [ -n "$free" ] && [ -n "$bs" ] || return 1
    _awk -v c="$count" -v f="$free" -v b="$bs" 'BEGIN{printf "%d", (c-f)*b/1024}'
}

# KB the filesystem is worth in total (blocks x block size). Static: only a
# resize changes it, so reading the superblock offline is exact even while the
# container is running.
_fs_total_kb() {
    local info count bs
    info=$(tune2fs -l "$1" 2>/dev/null)
    count=$(printf '%s\n' "$info" | sed -n 's/^Block count: *\([0-9][0-9]*\).*/\1/p' | head -1)
    bs=$(printf '%s\n' "$info" | sed -n 's/^Block size: *\([0-9][0-9]*\).*/\1/p' | head -1)
    [ -n "$count" ] && [ -n "$bs" ] || return 1
    _awk -v c="$count" -v b="$bs" 'BEGIN{printf "%d", c*b/1024}'
}

# KB a running container's root filesystem really holds, metadata included. It is
# asked inside the container with df, because the on-disk superblock of a mounted
# ext4 only catches up on checkpoint or unmount.
#
# df's own "used" is not that number: ext4 reports only the usable blocks, leaving
# out what it spends on itself (journal, block and inode bitmaps, group
# descriptors). On a 16 GB image that gap is several hundred MB, and subtracting
# it from the host allocation made every preview promise space a trim could not
# find. So: total from the superblock, free from df, and the difference is what
# the image actually holds.
_fs_used_kb_running() {
    local name="$1" img="$2" bin out total used total_kb
    bin=$(_droidspaces_bin) || return 1
    out=$("$bin" -n "$name" run -- df -kP / 2>/dev/null | tail -1)
    total=$(printf '%s\n' "$out" | _awk 'NF >= 3 {print $2}')
    used=$(printf '%s\n' "$out" | _awk 'NF >= 3 {print $3}')
    case "$total$used" in
        "" | *[!0-9]*) return 1 ;;
    esac
    total_kb=$(_fs_total_kb "$img") || return 1
    _awk -v t="$total_kb" -v total="$total" -v used="$used" 'BEGIN{printf "%d", t-(total-used)}'
}

# The droidspaces binary that owns this install, or nothing.
_droidspaces_bin() {
    local bin="${DROIDSPACES_BIN:-}"
    if [ -z "$bin" ] || [ ! -x "$bin" ]; then
        bin="/data/local/Droidspaces/bin/droidspaces"
    fi
    [ -x "$bin" ] || bin=$(command -v droidspaces 2>/dev/null)
    [ -n "$bin" ] && [ -x "$bin" ] || return 1
    echo "$bin"
}

# The init pid of a running container, nothing when it is not running.
_container_pid() {
    local pidfile="/data/local/Droidspaces/Pids/$1.pid" pid
    [ -f "$pidfile" ] || return 1
    pid=$(cat "$pidfile" 2>/dev/null)
    [ -n "$pid" ] || return 1
    kill -0 "$pid" 2>/dev/null || return 1
    echo "$pid"
}

# The loop device an image is attached to, nothing when none is.
_loop_for_image() {
    losetup -a 2>/dev/null | grep -F "$1" | head -1 | cut -d: -f1
}

# fstrim one mounted path, with busybox when the system has none.
_fstrim_path() {
    if [ -n "$BB" ]; then
        "$BB" fstrim -v "$1" 2>&1
    else
        fstrim -v "$1" 2>&1
    fi
}

# Trim a running container from here, through /proc/<pid>/root. That path
# resolves to the container's root mount, so FITRIM reaches its ext4 on the loop
# device and every discard comes back as a hole in the image file. Nothing
# inside the container takes part, which also covers the distros whose busybox
# ships the fstrim symlink without the applet.
_trim_live() {
    local name="$1" pid out
    pid=$(_container_pid "$name")
    if [ -n "$pid" ]; then
        out=$(_fstrim_path "/proc/$pid/root")
        case "$out" in
            *trimmed*)
                printf '%s\n' "$out" | grep -i trimmed | tail -1
                return 0 ;;
        esac
    fi
    # A root that is an overlay rather than the image itself has nothing to
    # trim, but the container's own fstrim can still reach whatever it mounted.
    _trim_in_container "$name"
}

# Trim from inside the running container, as a fallback for that case.
_trim_in_container() {
    local name="$1" bin out
    if ! bin=$(_droidspaces_bin); then
        error "droidspaces binary not found"
        return 1
    fi
    out=$("$bin" -n "$name" run -- sh -c \
        'if command -v fstrim >/dev/null 2>&1; then fstrim -v /; else busybox fstrim -v /; fi' 2>&1)
    case "$out" in
        *trimmed*)
            printf '%s\n' "$out" | grep -i trimmed | tail -1
            return 0 ;;
        *)
            printf '%s\n' "$out" | tail -3
            return 1 ;;
    esac
}

# Trim with the image mounted here, for a stopped container whose fstrim is out
# of reach. The discards land in the backing file exactly the same way.
_trim_offline() {
    local img="$1" mnt out rc
    mnt="$(dirname "$img")/.reclaim_$$"
    _mkdir "$mnt" || { error "Could not create $mnt"; return 1; }
    if ! _mount -t ext4 -o loop,rw "$img" "$mnt"; then
        _rm -rf "$mnt"
        return 1
    fi
    out=$(_fstrim_path "$mnt")
    rc=$?
    _sync
    _umount "$mnt" 2>/dev/null
    _rm -rf "$mnt"
    if [ "$rc" -ne 0 ]; then
        printf '%s\n' "$out" | tail -3
        return 1
    fi
    printf '%s\n' "$out" | tail -1
    return 0
}


# CMD: reclaim
#
# Deleting files inside a container frees blocks inside the ext4 image, not on
# the host: the image keeps every block it ever allocated, so a container that
# once held 3 GB of downloads goes on paying for them after they are gone. A
# trim releases the blocks no file uses, and the loop device turns each discard
# into a hole in the backing file. The image keeps its size, its capacity and
# every file in it; it only stops occupying space it does not need.
cmd_reclaim() {
    local name="$CONTAINER_NAME" pid="" loop="" running=0
    local alloc_kb used_kb image_mb alloc_mb used_mb reclaim_kb reclaim_mb
    local trim_out="" trim_rc=0

    [ -f "$RESIZE_IMG" ] || {
        error "Sparse image not found: $RESIZE_IMG"
        exit 1
    }
    check_reclaim_requirements

    if [ -n "$name" ]; then
        pid=$(_container_pid "$name")
        [ -n "$pid" ] && running=1
    fi

    # A live image belongs to its container, and trimming it from here would be
    # guessing at a filesystem somebody else is mounting. Only the container's
    # own namespace can trim that one.
    loop=$(_loop_for_image "$RESIZE_IMG")
    if [ -n "$loop" ] && [ "$running" -eq 0 ]; then
        error "The image is attached to $loop, so a container is using it"
        error "Pass -c <container> so the trim runs where the image is mounted"
        exit 1
    fi

    alloc_kb=$(_alloc_kb "$RESIZE_IMG")
    used_kb=""
    # The on-disk superblock is stale while the filesystem is mounted, so a live
    # container is asked directly. A stopped one has nothing to be stale about.
    if [ "$running" -eq 1 ]; then
        used_kb=$(_fs_used_kb_running "$name" "$RESIZE_IMG")
    fi
    if [ -z "$used_kb" ]; then
        if ! used_kb=$(_fs_used_kb "$RESIZE_IMG"); then
            error "Could not read the filesystem - is this an ext2/3/4 image?"
            exit 1
        fi
    fi
    image_mb=$(_bytes_mb "$(_stat_size "$RESIZE_IMG")")
    alloc_mb=$(( alloc_kb / 1024 ))
    used_mb=$(( used_kb / 1024 ))

    # A floor, not a promise: the host can hold less than the filesystem uses
    # (f2fs compresses under /data), and it never holds less than the blocks the
    # filesystem calls free and has already discarded. Under-reporting is the
    # safe direction; the run below reports what actually came back.
    reclaim_kb=$(( alloc_kb - used_kb ))
    [ "$reclaim_kb" -lt 0 ] && reclaim_kb=0
    reclaim_mb=$(( reclaim_kb / 1024 ))

    if [ "$running" -eq 1 ]; then
        log "Container '$name' is running - trimming it live"
    else
        log "Container is stopped - mounting the image to trim it"
    fi
    log "   Capacity      : ${image_mb} MB (this does not change)"
    log "   Allocated     : ${alloc_mb} MB"
    log "   In use        : ${used_mb} MB (metadata included)"
    log "   Reclaimable   : ${reclaim_mb} MB (estimate)"

    if [ "$DRY_RUN" -eq 1 ]; then
        # The app reads these lines and shows the preview from them (IMAGE_MB and
        # ALLOC_MB for the two rows, RECLAIM_MB for the estimate; USED_MB is for a
        # log). A real run prints the result instead, so no key ever arrives twice.
        echo "IMAGE_MB=${image_mb}"
        echo "ALLOC_MB=${alloc_mb}"
        echo "USED_MB=${used_mb}"
        echo "RECLAIM_MB=${reclaim_mb}"
        log "Dry run: nothing was changed"
        exit 0
    fi

    if [ "$YES_FLAG" -eq 0 ]; then
        warn "Trimming releases blocks no file uses. Nothing is deleted, and the"
        warn "image keeps its size, its capacity and everything in it."
        printf "  Type 'YES' to confirm: "
        read -r _confirm
        [ "$_confirm" != "YES" ] && { log "Reclaim cancelled"; exit 0; }
    fi

    if [ "$running" -eq 1 ]; then
        log "[1/2] Trimming '$name'..."
        trim_out=$(_trim_live "$name")
        trim_rc=$?
    else
        log "[1/2] Trimming the mounted image..."
        trim_out=$(_trim_offline "$RESIZE_IMG")
        trim_rc=$?
    fi
    [ -n "$trim_out" ] && log "    $trim_out"
    if [ "$trim_rc" -ne 0 ]; then
        error "The trim did not complete - the image was left as it was"
        error "A loop or host filesystem without discard support looks like this"
        exit 1
    fi
    _sync; _sleep 1

    # The host is the only witness that matters: the file either has fewer
    # blocks on disk now or it has not, whatever the trim reported.
    log "[2/2] Measuring the result..."
    local new_alloc_kb new_alloc_mb freed_kb freed_mb
    new_alloc_kb=$(_alloc_kb "$RESIZE_IMG")
    freed_kb=$(( alloc_kb - new_alloc_kb ))
    [ "$freed_kb" -lt 0 ] && freed_kb=0
    new_alloc_mb=$(( new_alloc_kb / 1024 ))
    freed_mb=$(( freed_kb / 1024 ))

    echo "IMAGE_MB=${image_mb}"
    echo "FREED_MB=${freed_mb}"

    log ""
    if [ "$freed_kb" -le 0 ]; then
        log "Nothing came back: the image already occupies only what it uses"
    elif [ "$freed_mb" -le 0 ]; then
        log "Under 1 MB came back - the image was already as tight as it gets"
    else
        log "Reclaim complete!"
        log "   Allocated ${alloc_mb} MB -> ${new_alloc_mb} MB (freed ${freed_mb} MB)"
        log "   Capacity stays ${image_mb} MB"
    fi
    log "   Image: $RESIZE_IMG"
}


# Usage

usage() {
    echo "Usage:"
    echo "  $SCRIPT_NAME -d <base_dir> migrate <size_gb>"
    echo "  $SCRIPT_NAME -i <image>    resize  <size_gb>"
    echo "  $SCRIPT_NAME -i <image>    reclaim [-c <container>]"
    echo ""
    echo "Options:"
    echo "  -d, --dir <path>      Base directory containing rootfs/ (for migrate)"
    echo "                        Derives rootfs/ and rootfs.img from this path"
    echo "  -i, --image <file>    Path to an existing sparse image (resize, reclaim)"
    echo "  -c, --container <name>"
    echo "                        The container that owns the image (reclaim). A running"
    echo "                        one is trimmed live; a stopped one is mounted here."
    echo "  -y, --yes             Skip confirmation prompts"
    echo "      --dry-run         Report what could be freed, change nothing (reclaim)"
    echo "  -h, --help            Show this help and exit"
    echo ""
    echo "Commands:"
    echo "  migrate <size_gb>     Migrate directory-based rootfs/ to a sparse ext4 image"
    echo "                        size: 1-512 GB"
    echo "  resize  <size_gb>     Resize an existing sparse image"
    echo "                        size: 1-512 GB"
    echo "  reclaim               Give back the space deleted files left behind:"
    echo "                        unused blocks are trimmed, the loop device turns each"
    echo "                        discard into a hole in the image, and the file takes"
    echo "                        less room on disk. Size, capacity and contents stay."
    echo ""
    echo "BusyBox:"
    echo "  Default path  : /data/local/Droidspaces/bin/busybox"
    echo "  Auto-fallback : command -v busybox, then which busybox"
    if [ -n "$BB" ]; then
        echo "  Detected      : $BB"
    else
        echo "  Detected      : NOT FOUND (some operations may fail)"
    fi
    echo ""
    echo "Examples:"
    echo "  $SCRIPT_NAME -d /data/local/ubuntu migrate 16"
    echo "  $SCRIPT_NAME -i /data/local/ubuntu/rootfs.img resize 32"
    echo "  $SCRIPT_NAME -i /data/local/ubuntu/rootfs.img resize 8 --yes"
    echo "  $SCRIPT_NAME -i /data/local/ubuntu/rootfs.img reclaim --dry-run"
    echo "  $SCRIPT_NAME -i /data/local/ubuntu/rootfs.img -c ubuntu reclaim --yes"
    echo ""
    echo "Notes:"
    echo "  - Unmount the image/rootfs before resizing."
    echo "  - reclaim is safe on a running container; it deletes nothing."
    echo "  - migrate requires: mkfs.ext4 (or mke2fs), tar"
    echo "  - resize  requires: e2fsck, resize2fs"
    echo "  - reclaim requires: tune2fs, losetup, fstrim (busybox has one)"
    exit "${1:-0}"
}


# Argument Parsing

BASE_DIR=""
RESIZE_IMG=""
CONTAINER_NAME=""
COMMAND=""
CMD_ARG=""
YES_FLAG=0
DRY_RUN=0

while [ $# -gt 0 ]; do
    case "$1" in
        -d|--dir)
            [ -z "$2" ] && { error "--dir requires a path argument"; exit 1; }
            BASE_DIR="$2"; shift 2 ;;
        -i|--image)
            [ -z "$2" ] && { error "--image requires a file path argument"; exit 1; }
            RESIZE_IMG="$2"; shift 2 ;;
        -y|--yes)
            YES_FLAG=1; shift ;;
        -c|--container)
            [ -z "$2" ] && { error "--container requires a name argument"; exit 1; }
            CONTAINER_NAME="$2"; shift 2 ;;
        --dry-run)
            DRY_RUN=1; shift ;;
        -h|--help)
            usage 0 ;;
        migrate|resize|reclaim)
            COMMAND="$1"
            shift
            # Grab the size argument (next positional, if not a flag)
            if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then
                CMD_ARG="$1"; shift
            fi
            ;;
        -*)
            error "Unknown option: $1"
            usage 1 ;;
        *)
            # Bare positional: treat as size argument if command already set
            if [ -n "$COMMAND" ] && [ -z "$CMD_ARG" ]; then
                CMD_ARG="$1"; shift
            else
                error "Unexpected argument: $1"
                usage 1
            fi
            ;;
    esac
done


# Validate Required Args

if [ -z "$COMMAND" ]; then
    error "No command specified"
    usage 1
fi

case "$COMMAND" in
    migrate)
        if [ -z "$BASE_DIR" ]; then
            error "migrate requires -d <base_dir>"
            usage 1
        fi
        ;;
    resize)
        if [ -z "$RESIZE_IMG" ]; then
            error "resize requires -i <image>"
            usage 1
        fi
        ;;
    reclaim)
        if [ -z "$RESIZE_IMG" ]; then
            error "reclaim requires -i <image>"
            usage 1
        fi
        ;;
esac

# reclaim takes no size, so only the two size commands insist on one.
if [ -z "$CMD_ARG" ] && [ "$COMMAND" != "reclaim" ]; then
    error "Size argument required (e.g. '$SCRIPT_NAME ... $COMMAND 16')"
    usage 1
fi


# Root Check

if [ "$(id -u)" -ne 0 ]; then
    error "This script must be run as root"
    exit 1
fi


# Derive Paths (migrate only)

if [ "$COMMAND" = "migrate" ]; then
    if [ ! -d "$BASE_DIR" ]; then
        error "Base directory does not exist: $BASE_DIR"
        exit 1
    fi
    ROOTFS_DIR="$BASE_DIR/rootfs"
    ROOTFS_IMG="$BASE_DIR/rootfs.img"
    ROOTFS_SPARSE="$BASE_DIR/rootfs.sparse"   # temp mount point during migration
fi

# Dispatch

case "$COMMAND" in
    migrate) cmd_migrate "$CMD_ARG" ;;
    resize)  cmd_resize  "$CMD_ARG" ;;
    reclaim) cmd_reclaim ;;
esac
