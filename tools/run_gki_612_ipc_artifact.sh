#!/bin/bash
set -euo pipefail

repo=$(realpath "${1:?source checkout required}")
work=$(mktemp -d "${RUNNER_TEMP:-/tmp}/sheng-gki612-ipc.XXXXXX")
artifacts=${SHENG_GKI_ARTIFACT_DIR:-$work/artifacts}
input=$work/input
stage=prepare
transaction_passed=false
mkdir -p "$artifacts"
artifacts=$(realpath "$artifacts")


finish()
{
    local status=$?
    printf '{"exit_code":%d,"stage":"%s","device_tested":false,"full_ipc_transactions_tested":false,"basic_transaction_fd_tested":%s}\n' \
        "$status" "$stage" "$transaction_passed" > "$artifacts/probe-result.json"
    exit "$status"
}


trap finish EXIT
exec > >(tee "$artifacts/probe.log") 2>&1

# 下载固定构建产物，保留来源及完整摘要校验
stage=artifact
source_url=$(python3 "$repo/tools/gki_612_ipc_artifact.py" url)
curl --fail --silent --show-error --location --retry 3 \
    -H "Authorization: Bearer ${GITHUB_TOKEN:?artifact read token required}" \
    -H 'Accept: application/vnd.github+json' "$source_url" \
    > "$artifacts/source-artifact.json"
curl --fail --silent --show-error --location --retry 3 \
    -H "Authorization: Bearer $GITHUB_TOKEN" \
    -H 'Accept: application/vnd.github+json' "$source_url/zip" > "$work/source.zip"
python3 "$repo/tools/gki_612_ipc_artifact.py" prepare \
    "$work/source.zip" "$input" "$artifacts/input-audit.json" \
    "$artifacts/source-artifact.json"
cp "$input/kernel.config" "$artifacts/kernel.config"
cp "$input/kernel.release" "$artifacts/kernel.release"
git -C "$repo" rev-parse HEAD > "$artifacts/recipe-revision.txt"
python3 "$repo/tools/verify_gki_612.py" "$input/kernel.config"
for option in CONFIG_DEVTMPFS=y CONFIG_PROC_FS=y CONFIG_SYSFS=y \
    CONFIG_MODULE_SIG=y CONFIG_MODULE_SIG_ALL=y '# CONFIG_CFI_PERMISSIVE is not set'; do
    grep -Fx "$option" "$input/kernel.config"
done
module=$input/rootfs/probe/sheng_hmos_ipc.ko
test -z "$(modinfo -F depends "$module")"
test "$(modinfo -F vermagic "$module")" = \
    '6.12.69-android16-6-sheng-pidns-4k+ SMP preempt mod_unload modversions aarch64'
modinfo "$module" > "$artifacts/loaded-module.modinfo"

# 只构建独立用户态探针，复用原内核及其签名模块
stage=probe-init
aarch64-linux-gnu-gcc --version > "$artifacts/probe-compiler.txt"
aarch64-linux-gnu-gcc -O2 -nostdlib -static -ffreestanding \
    -fno-stack-protector -fno-builtin -Wl,-e,_start \
    "$repo/tools/gki_612_ipc_smoke_init.c" -o "$input/rootfs/init"
chmod 0755 "$input/rootfs/init"
aarch64-linux-gnu-gcc -O2 -std=gnu11 -Wall -Wextra -Werror -static \
    "$repo/tools/gki_612_binder_transaction.c" -o "$input/rootfs/probe/binder-transaction"
chmod 0755 "$input/rootfs/probe/binder-transaction"
cp "$input/rootfs/probe/binder-transaction" "$artifacts/transaction-helper.elf"
aarch64-linux-gnu-readelf -h "$artifacts/transaction-helper.elf" \
    > "$artifacts/transaction-helper-elf.txt"
chmod 0644 "$module"
cp "$input/rootfs/init" "$artifacts/probe-init.elf"
aarch64-linux-gnu-readelf -h "$artifacts/probe-init.elf" > "$artifacts/probe-init-elf.txt"
gzip -dc "$input/smoke-initramfs.cpio.gz" > "$work/probe.cpio"
(cd "$input/rootfs" && find . -print0 | LC_ALL=C sort -z | \
    cpio --null -o --format=newc --reproducible --owner=0:0) >> "$work/probe.cpio"
gzip -n -c "$work/probe.cpio" > "$artifacts/probe-initramfs.cpio.gz"

# 使用内核原生模块校验及显式签名强制策略
stage=qemu-ipc-transaction-and-fd
set +e
timeout -s KILL 60 qemu-system-aarch64 \
    -machine virt,gic-version=3 -cpu max -m 1024 -smp 2 -nographic -no-reboot \
    -kernel "$input/Image" -initrd "$artifacts/probe-initramfs.cpio.gz" \
    -append 'console=ttyAMA0 earlycon=pl011,0x9000000 rdinit=/init panic=-1 kvm-arm.mode=none module.sig_enforce=1' \
    > "$artifacts/qemu-ipc-smoke.log" 2>&1
qemu_status=$?
set -e
printf '%d\n' "$qemu_status" > "$artifacts/qemu-exit-code.txt"
test "$qemu_status" = 0
python3 "$repo/tools/verify_gki_612.py" \
    "$input/kernel.config" "$artifacts/qemu-ipc-smoke.log"
python3 "$repo/tools/gki_612_ipc_artifact.py" verify "$artifacts/qemu-ipc-smoke.log"
transaction_passed=true
stage=complete-basic-transaction-and-fd-only
(cd "$artifacts" && sha256sum input-audit.json kernel.config kernel.release \
    probe-init.elf transaction-helper.elf probe-initramfs.cpio.gz \
    qemu-ipc-smoke.log > SHA256SUMS)
