import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


KERNEL_COMMIT = "b18aa09ef8e78438227d33ab5e938145332e0e03"
TEXT_OFFSETS = ("0x0", "0x80000")
KIMAGE_VADDR = 0xFFFFFFC080000000
SOURCE_HASHES = {
    "arch/arm64/Makefile": "963589f7c67809fa79fdac7fb1c26dda4f93942d78a435602d19ee8da853d9ab",
    "arch/arm64/kernel/head.S": "7482e330880f728fafee7f79709c116d2051bf53660e47dafd7d2276e93dbf3d",
    "arch/arm64/kernel/image.h": "38250b439a9f562a38c590755237d4557768cabc8b803d186d9c618feeebebfa",
    "arch/arm64/kernel/vmlinux.lds.S": "d4a190848b0228d2552a28b8c48ef397fcd3f76410ef89b7f607547f444a352f",
    "arch/arm64/kernel/pi/map_kernel.c": "f13ed9a35d78270f48e1f95aa5d3f5fb6e963d564e9a67de8ca99cbfb2fc166e",
    "arch/arm64/include/asm/memory.h": "fdb489e8a4dfcf38651cc8feb286ec3288c4850a51cefc10ca6b249103647c7f",
}
SOURCE_EDITS = {
    "arch/arm64/Makefile": (
        "LDFLAGS_vmlinux\t:=--no-undefined -X --pic-veneer\n",
        "TEXT_OFFSET ?= 0x0\n"
        "ifneq ($(TEXT_OFFSET),0x0)\n"
        "ifneq ($(TEXT_OFFSET),0x80000)\n"
        "$(error Unsupported TEXT_OFFSET: $(TEXT_OFFSET))\n"
        "endif\n"
        "endif\n"
        "KBUILD_CPPFLAGS += -DTEXT_OFFSET=$(TEXT_OFFSET)\n\n"
        "LDFLAGS_vmlinux\t:=--no-undefined -X --pic-veneer\n",
    ),
    "arch/arm64/kernel/head.S": (
        "\t.quad\t0\t\t\t\t// Image load offset from start of RAM, little-endian\n",
        "\tle64sym\t_kernel_offset_le\t\t// Image load offset, little-endian\n",
    ),
    "arch/arm64/kernel/image.h": (
        "\tDEFINE_IMAGE_LE64(_kernel_size_le, _end - _text);\t\\\n",
        "\tDEFINE_IMAGE_LE64(_kernel_offset_le, TEXT_OFFSET);\t\\\n"
        "\tDEFINE_IMAGE_LE64(_kernel_size_le, _end - _text);\t\\\n",
    ),
}
REQUIRED_CONFIG = [
    "CONFIG_ARM64_4K_PAGES=y",
    "CONFIG_CMDLINE_FORCE=y",
    "CONFIG_INITRAMFS_FORCE=y",
    "CONFIG_CFI_CLANG=y",
    "CONFIG_MODVERSIONS=y",
    "CONFIG_RELOCATABLE=y",
    "CONFIG_RANDOMIZE_BASE=y",
    "# CONFIG_CPU_BIG_ENDIAN is not set",
    "CONFIG_MODULE_SIG=y",
    "CONFIG_MODULE_SIG_ALL=y",
    "CONFIG_MODULE_SIG_SHA1=y",
    'CONFIG_MODULE_SIG_HASH="sha1"',
    'CONFIG_MODULE_SIG_KEY="certs/signing_key.pem"',
    "# CONFIG_MODULE_SIG_FORCE is not set",
    "CONFIG_COMMON_CLK_QCOM=y",
    "CONFIG_SM_GCC_8550=y",
    "CONFIG_SM_TCSRCC_8550=y",
    "CONFIG_QCOM_SCM=y",
    "CONFIG_QCOM_RPMH=y",
    "CONFIG_QCOM_COMMAND_DB=y",
    "CONFIG_QCOM_CLK_RPMH=y",
    "CONFIG_PINCTRL_MSM=y",
    "CONFIG_PINCTRL_SM8550=y",
    "CONFIG_SPMI_MSM_PMIC_ARB=y",
    "CONFIG_MFD_SPMI_PMIC=y",
    "CONFIG_REGULATOR_QCOM_RPMH=y",
    "CONFIG_INTERCONNECT_QCOM_SM8550=y",
    "CONFIG_ARM_SMMU=y",
    "CONFIG_USB_DWC3_QCOM=y",
    "CONFIG_USB_DWC3_GADGET=y",
    "CONFIG_PHY_QCOM_SNPS_EUSB2=y",
    "CONFIG_PHY_QCOM_EUSB2_REPEATER=y",
    "CONFIG_USB_G_SERIAL=y",
    "CONFIG_U_SERIAL_CONSOLE=y",
    'CONFIG_LOCALVERSION="-android16-6-sheng-ram-smoke"',
    "# CONFIG_SCSI_UFS_QCOM is not set",
    "# CONFIG_DRM_MSM is not set",
]


def file_hash(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, data):
    Path(path).write_text(json.dumps(data, indent=2) + "\n",
        encoding="utf-8", newline="\n")


def source_delta(originals):
    result = dict(originals)
    for name, (old, new) in SOURCE_EDITS.items():
        old, new = old.encode(), new.encode()
        if originals[name].count(old) != 1:
            raise SystemExit("Kernel source anchor mismatch: " + name)
        result[name] = originals[name].replace(old, new, 1)
    return result


def kernel_revision(kernel):
    revision = subprocess.check_output(
        ["git", "-C", str(kernel), "rev-parse", "HEAD"], text=True).strip()
    if revision != KERNEL_COMMIT:
        raise SystemExit("Unexpected kernel revision")
    return revision


def prepare_kernel_source(kernel, text_offset, metadata_path):
    kernel_revision(kernel)
    originals = {name: (kernel / name).read_bytes() for name in SOURCE_HASHES}
    for name, data in originals.items():
        if hashlib.sha256(data).hexdigest() != SOURCE_HASHES[name]:
            raise SystemExit("Original kernel source hash mismatch: " + name)
    prepared = source_delta(originals)
    # 全部原文校验通过后才写入三个源码变更
    for name in SOURCE_EDITS:
        (kernel / name).write_bytes(prepared[name])
    write_json(metadata_path, {
        "kernel_commit": KERNEL_COMMIT,
        "text_offset": text_offset,
        "patch_kind": "source_parameter_no_linker_padding",
        "files": {name: {
            "original_sha256": SOURCE_HASHES[name],
            "prepared_sha256": hashlib.sha256(data).hexdigest(),
            "changed": name in SOURCE_EDITS,
        } for name, data in prepared.items()},
        "device_tested": False,
        "temp_boot_packaging_verified": False,
    })


def verify_source_metadata(kernel, text_offset, metadata_path):
    kernel_revision(kernel)
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    if (metadata.get("kernel_commit") != KERNEL_COMMIT or
        metadata.get("text_offset") != text_offset or
        metadata.get("patch_kind") != "source_parameter_no_linker_padding" or
        set(metadata.get("files", {})) != set(SOURCE_HASHES)):
        raise SystemExit("Text offset source metadata mismatch")
    restored = {}
    for name, original_hash in SOURCE_HASHES.items():
        data = (kernel / name).read_bytes()
        record = metadata["files"][name]
        if (record.get("original_sha256") != original_hash or
            record.get("prepared_sha256") != hashlib.sha256(data).hexdigest() or
            record.get("changed") != (name in SOURCE_EDITS)):
            raise SystemExit("Prepared kernel source hash mismatch: " + name)
        if name in SOURCE_EDITS:
            old, new = (item.encode() for item in SOURCE_EDITS[name])
            if data.count(new) != 1:
                raise SystemExit("Prepared kernel source anchor mismatch: " + name)
            data = data.replace(new, old, 1)
        if hashlib.sha256(data).hexdigest() != original_hash:
            raise SystemExit("Kernel source delta mismatch: " + name)
        restored[name] = data
    if source_delta(restored) != {
        name: (kernel / name).read_bytes() for name in SOURCE_HASHES
    }:
        raise SystemExit("Kernel source round-trip mismatch")
    return metadata


def verify_config(config):
    lines = set(config.read_text(encoding="utf-8").splitlines())
    missing = [item for item in REQUIRED_CONFIG if item not in lines]
    if missing:
        raise SystemExit("Board configuration mismatch: " + repr(missing))
    if not any(line.startswith('CONFIG_INITRAMFS_SOURCE="') and line !=
        'CONFIG_INITRAMFS_SOURCE=""' for line in lines):
        raise SystemExit("Missing built-in initramfs")


def verify_dtb(path):
    data = path.read_bytes()
    if len(data) < 40:
        raise SystemExit("Truncated DTB")
    magic, length = struct.unpack_from(">II", data)
    if magic != 0xD00DFEED or length != len(data):
        raise SystemExit("DTB header mismatch")


def read_kernel_elf(path):
    size = path.stat().st_size
    with path.open("rb") as stream:
        def read_at(offset, length):
            if offset < 0 or length < 0 or offset + length > size:
                raise SystemExit("Truncated vmlinux")
            stream.seek(offset)
            return stream.read(length)

        header = struct.unpack("<16sHHIQQQIHHHHHH", read_at(0, 64))
        if (header[0][:7] != b"\x7fELF\x02\x01\x01" or
            header[1:4] != (3, 183, 1) or header[8] != 64 or
            header[11] != 64 or not header[12]):
            raise SystemExit("Expected ELF64 LE AArch64 ET_DYN vmlinux")
        sections = [struct.unpack("<IIQQQQIIQQ",
            read_at(header[6] + index * 64, 64))
            for index in range(header[12])]
        if header[13] >= len(sections):
            raise SystemExit("Invalid ELF section strings")
        names = sections[header[13]]
        names = read_at(names[4], names[5])
        by_name = {}
        for section in sections:
            end = names.find(b"\0", section[0])
            if end < 0:
                raise SystemExit("Invalid ELF section name")
            by_name[names[section[0]:end]] = section
        head = by_name.get(b".head.text")
        table = by_name.get(b".symtab")
        if (not head or head[1] != 1 or head[5] < 64 or
            not head[2] & 2 or not table or table[1] != 2 or
            table[9] != 24 or table[5] % 24 or table[6] >= len(sections)):
            raise SystemExit("Missing kernel header or symbol table")
        strings = sections[table[6]]
        strings = read_at(strings[4], strings[5])
        wanted = {b"_text", b"_end", b"_kernel_offset_le_lo32",
            b"_kernel_offset_le_hi32", b"__pi_early_map_kernel",
            b"__primary_switch", b"__pi_rela_start", b"__pi_rela_end"}
        symbols = {}
        for name, info, other, index, value, length in struct.iter_unpack(
            "<IBBHQQ", read_at(table[4], table[5])):
            end = strings.find(b"\0", name)
            if end < 0:
                raise SystemExit("Invalid ELF symbol name")
            name = strings[name:end]
            if name in wanted:
                if name in symbols:
                    raise SystemExit("Duplicate kernel symbol")
                symbols[name] = (value, index)
        return {"entry": header[4], "head_address": head[3],
            "head_bytes": read_at(head[4], 64), "symbols": symbols}


def verify_artifact(image, vmlinux, text_offset):
    with image.open("rb") as stream:
        head = stream.read(64)
    if len(head) != 64:
        raise SystemExit("Truncated Image")
    offset, footprint, flags = struct.unpack_from("<QQQ", head, 8)
    if (offset != int(text_offset, 16) or footprint < image.stat().st_size or
        footprint % 4096 or flags != 0xA or
        struct.unpack_from("<I", head, 56)[0] != 0x644D5241):
        raise SystemExit("Image offset, footprint or ARM64 header mismatch")
    elf = read_kernel_elf(vmlinux)
    symbols = elf["symbols"]
    required = {b"_text", b"_end", b"_kernel_offset_le_lo32",
        b"_kernel_offset_le_hi32", b"__pi_early_map_kernel",
        b"__primary_switch", b"__pi_rela_start", b"__pi_rela_end"}
    if not required <= set(symbols):
        raise SystemExit("Missing kernel text offset or PI relocation symbols")
    if (elf["entry"] != KIMAGE_VADDR or
        symbols[b"_text"][0] != KIMAGE_VADDR or
        elf["head_address"] != KIMAGE_VADDR or
        symbols[b"_end"][0] - KIMAGE_VADDR != footprint or
        elf["head_bytes"] != head):
        raise SystemExit("Image/vmlinux entry, header or footprint mismatch")
    if (symbols[b"_kernel_offset_le_lo32"] != (offset, 0xFFF1) or
        symbols[b"_kernel_offset_le_hi32"] != (0, 0xFFF1) or
        symbols[b"__pi_rela_end"][0] <= symbols[b"__pi_rela_start"][0]):
        raise SystemExit("Offset ABS symbols or relocation range mismatch")
    for name in (b"__pi_early_map_kernel", b"__primary_switch"):
        if not KIMAGE_VADDR <= symbols[name][0] < symbols[b"_end"][0]:
            raise SystemExit("PI function outside Image footprint")
    return {"text_offset": text_offset, "image_size": footprint,
        "Image_bytes": image.stat().st_size, "Image_sha256": file_hash(image),
        "vmlinux_sha256": file_hash(vmlinux), "ELF_type": "ET_DYN",
        "ELF_entry": hex(elf["entry"]),
        "symbols": {name.decode(): hex(value[0])
            for name, value in symbols.items()},
        "head64_identical": True, "device_tested": False,
        "temp_boot_packaging_verified": False}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("config", nargs="?", type=Path)
    parser.add_argument("dtb", nargs="?", type=Path)
    parser.add_argument("--text-offset", choices=TEXT_OFFSETS, default="0x0")
    parser.add_argument("--prepare-kernel-source", type=Path)
    parser.add_argument("--kernel-source", type=Path)
    parser.add_argument("--source-metadata", type=Path)
    parser.add_argument("--image", type=Path)
    parser.add_argument("--vmlinux", type=Path)
    parser.add_argument("--artifact-metadata", type=Path)
    args = parser.parse_args()
    if args.prepare_kernel_source:
        if (args.config or args.dtb or args.image or args.vmlinux or
            args.kernel_source or args.artifact_metadata or
            not args.source_metadata):
            parser.error("Source preparation requires only source, offset and metadata")
        prepare_kernel_source(args.prepare_kernel_source, args.text_offset,
            args.source_metadata)
        print("SHENG_GKI_612_TEXT_OFFSET_SOURCE_PASS")
        return
    if not args.config:
        parser.error("CONFIG is required")
    verify_config(args.config)
    if args.dtb:
        verify_dtb(args.dtb)
    artifact_args = (args.image, args.vmlinux, args.kernel_source,
        args.source_metadata, args.artifact_metadata)
    if any(artifact_args):
        if not all(artifact_args):
            parser.error("Artifact verification requires Image, vmlinux, source and metadata")
        source = verify_source_metadata(args.kernel_source, args.text_offset,
            args.source_metadata)
        result = verify_artifact(args.image, args.vmlinux, args.text_offset)
        result["kernel_commit"] = source["kernel_commit"]
        result["source_metadata_sha256"] = file_hash(args.source_metadata)
        result["config_sha256"] = file_hash(args.config)
        if args.dtb:
            result["DTB_sha256"] = file_hash(args.dtb)
        write_json(args.artifact_metadata, result)
    print("SHENG_GKI_612_BOARD_BUILD_CONFIG_PASS")


if __name__ == "__main__":
    main()
