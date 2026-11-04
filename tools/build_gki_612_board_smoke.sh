#!/bin/bash
set -euo pipefail

repo=$(realpath "${1:?source checkout required}")
text_offset=${SHENG_GKI_TEXT_OFFSET:-0x0}
case "$text_offset" in
    0x0|0x80000) ;;
    *) printf '%s\n' 'Unsupported SHENG_GKI_TEXT_OFFSET' >&2; exit 2 ;;
esac
work=$(mktemp -d "${RUNNER_TEMP:-/tmp}/sheng-gki612-board.XXXXXX")
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
    printf '{"exit_code":%d,"stage":"%s","text_offset":"%s","device_tested":false,"temp_boot_packaging_verified":false}\n' \
        "$status" "$stage" "$text_offset" > "$artifacts/build-result.json"
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

# 核对原源码后引入偏移参数，保持链接起点
stage=text-offset-source
python3 "$repo/tools/verify_gki_612_board_smoke.py" \
    --prepare-kernel-source "$kernel" --text-offset "$text_offset" \
    --source-metadata "$artifacts/text-offset-source.json"
git -C "$kernel" diff -- arch/arm64/Makefile arch/arm64/kernel/head.S \
    arch/arm64/kernel/image.h > "$artifacts/kernel-text-offset.patch"

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
options=(ARCH=arm64 LLVM="$clang_bin/" LLVM_IAS=1 PAHOLE="$btf_tools/bin/pahole"
    TEXT_OFFSET="$text_offset")

# 将独立初始化入口编入镜像，拒绝外部旧启动环境
stage=board-init
rootfs=$work/rootfs
mkdir -p "$rootfs"
"$clang_bin/clang" --target=aarch64-linux-gnu -O2 -nostdlib -static \
    -ffreestanding -fno-stack-protector -fno-builtin -fuse-ld=lld \
    -Wl,-e,_start "$repo/tools/gki_612_board_smoke_init.c" -o "$rootfs/init"
chmod 0755 "$rootfs/init"
printf '%s\n' 'dir /dev 0755 0 0' 'nod /dev/console 0600 0 0 c 5 1' \
    'dir /proc 0755 0 0' 'dir /sys 0755 0 0' \
    "file /init $rootfs/init 0755 0 0" > "$work/board-initramfs.list"

stage=board-dtb-source
board=sm8550-xiaomi-sheng-smoke
cp "$repo/configs/boards/$board.dts" "$kernel/arch/arm64/boot/dts/qcom/"
printf '\n%s\n' "dtb-\$(CONFIG_ARCH_QCOM) += $board.dtb" \
    >> "$kernel/arch/arm64/boot/dts/qcom/Makefile"

stage=config
make -C "$kernel" O="$output" "${options[@]}" gki_defconfig
"$kernel/scripts/kconfig/merge_config.sh" -m -O "$output" "$output/.config" \
    "$repo/configs/gki-612-sheng.fragment" \
    "$repo/configs/gki-612-sheng-smoke.fragment"
"$kernel/scripts/config" --file "$output/.config" \
    --set-str INITRAMFS_SOURCE "$work/board-initramfs.list"
make -C "$kernel" O="$output" "${options[@]}" olddefconfig
cp "$output/.config" "$artifacts/kernel.config"
python3 "$repo/tools/verify_gki_612_board_smoke.py" "$output/.config" \
    --text-offset "$text_offset"

stage=kernel-and-dtb
make -C "$kernel" O="$output" "${options[@]}" -j"$jobs" Image "qcom/$board.dtb"
cp "$output/arch/arm64/boot/Image" "$artifacts/Image"
cp "$output/arch/arm64/boot/dts/qcom/$board.dtb" "$artifacts/$board.dtb"
cp "$output/vmlinux" "$artifacts/vmlinux"
cp "$output/System.map" "$artifacts/System.map"
cp "$output/include/config/kernel.release" "$artifacts/kernel.release"
cp "$rootfs/init" "$artifacts/board-init.elf"
cp "$repo/configs/boards/$board.dts" "$artifacts/$board.dts"
cp "$repo/configs/gki-612-sheng-smoke.fragment" "$artifacts/board.config.fragment"
python3 "$repo/tools/verify_gki_612_board_smoke.py" \
    "$output/.config" "$artifacts/$board.dtb" --text-offset "$text_offset" \
    --image "$artifacts/Image" --vmlinux "$artifacts/vmlinux" \
    --kernel-source "$kernel" --source-metadata "$artifacts/text-offset-source.json" \
    --artifact-metadata "$artifacts/text-offset-artifact.json"
"$clang_bin/llvm-readelf" -h "$output/vmlinux" > "$artifacts/vmlinux-elf-header.txt"

# 仅交付构建证据，临时启动封装需单独验收
stage=artifact-audit
(cd "$artifacts" && sha256sum Image vmlinux System.map kernel.config \
    kernel.release board-init.elf "$board.dtb" text-offset-source.json \
    text-offset-artifact.json kernel-text-offset.patch > SHA256SUMS)
stage=complete-build-only
