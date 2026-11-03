from pathlib import Path
import struct
import sys


if len(sys.argv) not in (2, 3):
    raise SystemExit("Usage: verify_gki_612_board_smoke.py CONFIG [DTB]")
lines = set(Path(sys.argv[1]).read_text(encoding="utf-8").splitlines())
required = [
    "CONFIG_ARM64_4K_PAGES=y",
    "CONFIG_CMDLINE_FORCE=y",
    "CONFIG_INITRAMFS_FORCE=y",
    "CONFIG_CFI_CLANG=y",
    "CONFIG_MODVERSIONS=y",
    "CONFIG_COMMON_CLK_QCOM=y",
    "CONFIG_SM_GCC_8550=y",
    "CONFIG_SM_TCSRCC_8550=y",
    "CONFIG_QCOM_SCM=y",
    "CONFIG_QCOM_RPMH=y",
    "CONFIG_QCOM_COMMAND_DB=y",
    "CONFIG_QCOM_CLK_RPMH=y",
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
missing = [item for item in required if item not in lines]
if missing:
    raise SystemExit("Board configuration mismatch: " + repr(missing))
if not any(line.startswith('CONFIG_INITRAMFS_SOURCE="') and line !=
    'CONFIG_INITRAMFS_SOURCE=""' for line in lines):
    raise SystemExit("Missing built-in initramfs")

# 核对编译后的设备树容器，保留实机验收边界
if len(sys.argv) == 3:
    data = Path(sys.argv[2]).read_bytes()
    if len(data) < 40:
        raise SystemExit("Truncated DTB")
    magic, length = struct.unpack_from(">II", data)
    if magic != 0xD00DFEED or length != len(data):
        raise SystemExit("DTB header mismatch")
print("SHENG_GKI_612_BOARD_BUILD_CONFIG_PASS")
