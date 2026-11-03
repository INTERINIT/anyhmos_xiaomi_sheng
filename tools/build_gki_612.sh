#!/bin/bash
set -euo pipefail

repo=$(realpath "${1:?source checkout required}")
work=$(mktemp -d "${RUNNER_TEMP:-/tmp}/sheng-gki612.XXXXXX")
artifacts=${SHENG_GKI_ARTIFACT_DIR:-$work/artifacts}
kernel=$work/common
toolchain=$work/clang
output=$work/out
kernel_commit=b18aa09ef8e78438227d33ab5e938145332e0e03
clang_commit=70a12c4fe88940ab2b0e6d763492dffa2f07e15a
jobs=${SHENG_GKI_JOBS:-4}
stage=prepare
mkdir -p "$kernel" "$toolchain" "$output" "$artifacts"
artifacts=$(realpath "$artifacts")


finish()
{
    local status=$?
    printf '{"exit_code":%d,"stage":"%s","device_tested":false,"sheng_driver_port_complete":false}\n' \
        "$status" "$stage" > "$artifacts/build-result.json"
    exit "$status"
}


trap finish EXIT
exec > >(tee "$artifacts/build.log") 2>&1

# 固定源码和编译器提交，保留原始来源记录
stage=source
git -C "$kernel" init -q
git -C "$kernel" remote add origin https://android.googlesource.com/kernel/common
git -C "$kernel" fetch --depth=1 origin "$kernel_commit"
git -C "$kernel" checkout -q --detach FETCH_HEAD
test "$(git -C "$kernel" rev-parse HEAD)" = "$kernel_commit"
test "$(make -s -C "$kernel" kernelversion)" = 6.12.69
git -C "$kernel" rev-parse HEAD > "$artifacts/kernel-revision.txt"
git -C "$repo" rev-parse HEAD > "$artifacts/recipe-revision.txt"
cp "$kernel/build.config.constants" "$artifacts/build.config.constants"

stage=toolchain
git -C "$toolchain" init -q
git -C "$toolchain" remote add origin \
    https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86
git -C "$toolchain" sparse-checkout init --cone
git -C "$toolchain" sparse-checkout set clang-r536225
git -C "$toolchain" fetch --filter=blob:none --depth=1 origin "$clang_commit"
git -C "$toolchain" checkout -q --detach FETCH_HEAD
test "$(git -C "$toolchain" rev-parse HEAD)" = "$clang_commit"
clang_bin=$toolchain/clang-r536225/bin
"$clang_bin/clang" --version | tee "$artifacts/clang-version.txt"
git -C "$toolchain" rev-parse HEAD > "$artifacts/clang-revision.txt"

# 使用官方工具生成完整 BTF，避免旧版固定长度限制
stage=btf-tool
btf_tools=$work/btf-tools
btf_commit=010d8a83d84ced4beef7779453766350db8c08cd
btf_url=https://android.googlesource.com/kernel/prebuilts/build-tools/+/$btf_commit/linux-x86
mkdir -p "$btf_tools/bin" "$btf_tools/lib64"
curl --fail --location --retry 3 "$btf_url/bin/pahole?format=TEXT" \
    | base64 --decode > "$btf_tools/bin/pahole"
curl --fail --location --retry 3 "$btf_url/lib64/libc++.so?format=TEXT" \
    | base64 --decode > "$btf_tools/lib64/libc++.so"
printf '%s  %s\n' \
    8bd05335b93d42781a7208f0655147f30cdf2dc3fbf2ce4d5da557b77375f36f "$btf_tools/bin/pahole" \
    7b91de1cba2ad902aad871b77e58af40ec339aac6d514452f71475e895e1fba6 "$btf_tools/lib64/libc++.so" \
    | sha256sum --check
chmod 0755 "$btf_tools/bin/pahole"
"$btf_tools/bin/pahole" --version | tee "$artifacts/pahole-version.txt"
printf '%s\n' "$btf_commit" > "$artifacts/pahole-revision.txt"

export KBUILD_BUILD_USER=INTERINIT
export KBUILD_BUILD_HOST=sheng-build
export KBUILD_BUILD_VERSION=1
export SOURCE_DATE_EPOCH=$(git -C "$kernel" show -s --format=%ct HEAD)
export KBUILD_BUILD_TIMESTAMP=$(git -C "$kernel" show -s --format=%cD HEAD)
options=(ARCH=arm64 LLVM="$clang_bin/" LLVM_IAS=1 PAHOLE="$btf_tools/bin/pahole")

# 从官方配置生成独立四千字节页和命名空间变体
stage=config
make -C "$kernel" O="$output" "${options[@]}" gki_defconfig
"$kernel/scripts/kconfig/merge_config.sh" -m -O "$output" "$output/.config" \
    "$repo/configs/gki-612-sheng.fragment"
make -C "$kernel" O="$output" "${options[@]}" olddefconfig
cp "$output/.config" "$artifacts/kernel.config"
cp "$repo/configs/gki-612-sheng.fragment" "$artifacts/config.fragment"
python3 "$repo/tools/verify_gki_612.py" "$output/.config"

stage=kernel
make -C "$kernel" O="$output" "${options[@]}" -j"$jobs" Image modules
cp "$output/arch/arm64/boot/Image" "$artifacts/Image"
cp "$output/vmlinux" "$artifacts/vmlinux"
cp "$output/System.map" "$artifacts/System.map"
cp "$output/Module.symvers" "$artifacts/Module.symvers"
cp "$output/include/config/kernel.release" "$artifacts/kernel.release"
"$clang_bin/llvm-readelf" -h "$output/vmlinux" > "$artifacts/vmlinux-elf-header.txt"

stage=module-archive
modules=$work/modules-stage
make -C "$kernel" O="$output" "${options[@]}" \
    INSTALL_MOD_PATH="$modules" INSTALL_MOD_STRIP=1 modules_install
release=$(cat "$artifacts/kernel.release")
# 仅移除模块暂存树内的构建路径链接
for link in "$modules/lib/modules/$release/build" "$modules/lib/modules/$release/source"; do
    if [ -L "$link" ]; then
        unlink "$link"
    elif [ -e "$link" ]; then
        echo "Unexpected module staging entry: $link" >&2
        exit 1
    fi
done
tar -czf "$artifacts/modules.tar.gz" -C "$modules" lib

# 虚拟机只验证通用启动，不替代平板驱动验收
stage=qemu-smoke
rootfs=$work/rootfs
mkdir -p "$rootfs"
"$clang_bin/clang" --target=aarch64-linux-gnu -O2 -nostdlib -static \
    -ffreestanding -fno-stack-protector -fno-builtin -fuse-ld=lld \
    -Wl,-e,_start "$repo/tools/gki_612_smoke_init.c" -o "$rootfs/init"
chmod 0755 "$rootfs/init"
printf '%s\n' 'dir /dev 0755 0 0' 'nod /dev/console 0600 0 0 c 5 1' \
    'dir /proc 0755 0 0' "file /init $rootfs/init 0755 0 0" > "$work/initramfs.list"
"$output/usr/gen_init_cpio" "$work/initramfs.list" | gzip -n \
    > "$artifacts/smoke-initramfs.cpio.gz"
set +e
timeout -s KILL 45 qemu-system-aarch64 -machine virt,gic-version=3 -cpu max \
    -m 1024 -smp 2 -nographic -no-reboot -kernel "$artifacts/Image" \
    -initrd "$artifacts/smoke-initramfs.cpio.gz" \
    -append 'console=ttyAMA0 earlycon=pl011,0x9000000 rdinit=/init panic=-1 kvm-arm.mode=none' \
    > "$artifacts/qemu-smoke.log" 2>&1
qemu_status=$?
set -e
printf '%s\n' "$qemu_status" > "$artifacts/qemu-exit-code.txt"
test "$qemu_status" = 0
python3 "$repo/tools/verify_gki_612.py" "$output/.config" "$artifacts/qemu-smoke.log"

stage=artifact-audit
(cd "$artifacts" && sha256sum Image vmlinux System.map Module.symvers \
    kernel.config kernel.release modules.tar.gz smoke-initramfs.cpio.gz qemu-smoke.log > SHA256SUMS)
stage=complete
