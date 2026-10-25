#!/usr/bin/env bash
# Launches a small Windows VM (KVM accelerated) for building the Windows portable zip.
#
#   sudo apt install qemu-system-x86 ovmf
#   bash scripts/windows-vm.sh /path/to/windows.iso          # 首次安装
#   bash scripts/windows-vm.sh /path/to/windows.iso --run    # 之后直接启动
#
# The VM gets 2 vCPU / 3 GB (adjust with VCPUS / RAM env vars) and a shared folder
# (./vm-share) so the built zip can be moved back to the host.
set -euo pipefail
cd "$(dirname "$0")/.."

ISO="${1:?usage: windows-vm.sh <windows.iso> [--run]}"
MODE="${2:-install}"
IMG="${VM_IMG:-vm/windows.qcow2}"
SHARE="${VM_SHARE:-vm-share}"
VCPUS="${VCPUS:-2}"
RAM="${RAM:-3G}"
DISK="${DISK:-40G}"

mkdir -p "$(dirname "$IMG")" "$SHARE"

if [ ! -f /dev/kvm ]; then
    echo "warning: /dev/kvm not found - the VM will be *extremely* slow." >&2
fi

# "small Windows" tips:
#   * Windows Server 2022/2025 Core or Windows 10/11 LTSC install in ~8-12 GB and run
#     fine with 3 GB; Tiny11 is smaller still but unofficial (your call).
#   * Windows 11 needs TPM + Secure Boot: add the swtpm/OVMF lines below.
WITH_TPM="${WITH_TPM:-0}"

if [ ! -f "$IMG" ]; then
    echo "== creating $IMG ($DISK)"
    qemu-img create -f qcow2 "$IMG" "$DISK" >/dev/null
    MODE=install
fi

BOOT_ARGS=()
if [ "$MODE" = "install" ]; then
    BOOT_ARGS=(-drive "file=$ISO,media=cdrom,readonly=on" -boot d)
    echo "== installing from $ISO (eject the ISO / reboot when done)"
else
    echo "== booting the installed Windows"
fi

TPM_ARGS=()
if [ "$WITH_TPM" = "1" ]; then
    TPM_ARGS=(-chardev socket,id=chrtpm,path=vm/swtpm.sock
              -tpmdev emulator,id=tpm0,chardev=chrtpm
              -device tpm-tis,tpmdev=tpm0)
fi

# A 9p share shows up in Windows only with the virtio-win driver; for simplicity the
# launcher also exposes an SMB share alternative (host: qemu's built-in user net).
exec qemu-system-x86_64 \
    -enable-kvm -machine q35,accel=kvm -cpu host \
    -smp "$VCPUS" -m "$RAM" \
    -drive "file=$IMG,if=virtio,cache=writeback,discard=unmap" \
    "${BOOT_ARGS[@]}" "${TPM_ARGS[@]}" \
    -device virtio-net-pci,netdev=n0 -netdev "user,id=n0,smb=$SHARE" \
    -device virtio-vga-gl -display gtk,gl=on \
    -device qemu-xhci -device usb-tablet \
    -rtc base=localtime \
    -name "AdaptNote build VM"

# After Windows is installed and you install the build tools, either:
#   a) download the repo inside the VM and run scripts\make_portable.ps1, then copy
#      dist\*.zip out through the SMB share (\\10.0.2.4\qemu) or a USB image, or
#   b) mount a raw "usb stick" image:
#      qemu-img create -f raw vm/usb.img 4G   (add: -drive file=vm/usb.img,if=none,id=u0
#                                              -device usb-storage,drive=u0)
