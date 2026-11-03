from pathlib import Path
import sys


# 核对真实配置，拒绝仅修改版本标识的构建
if len(sys.argv) not in (2, 3):
    raise SystemExit("Usage: verify_gki_612.py CONFIG [QEMU_LOG]")
config = Path(sys.argv[1]).read_text(encoding="utf-8")
required = [
    "CONFIG_ARM64_4K_PAGES=y",
    "CONFIG_NAMESPACES=y",
    "CONFIG_PID_NS=y",
    "CONFIG_USER_NS=y",
    "CONFIG_UTS_NS=y",
    "CONFIG_SYSVIPC=y",
    "CONFIG_IPC_NS=y",
    "CONFIG_NET_NS=y",
    "CONFIG_CGROUP_PIDS=y",
    "CONFIG_ANDROID_BINDER_IPC=y",
    "CONFIG_ANDROID_BINDERFS=y",
    "CONFIG_CFI_CLANG=y",
    "CONFIG_MODVERSIONS=y",
    "CONFIG_MODULES=y",
    'CONFIG_LOCALVERSION="-android16-6-sheng-pidns-4k"',
]
missing = [value for value in required if value not in config.splitlines()]
if missing:
    raise SystemExit("Configuration mismatch: " + repr(missing))
for name in ["CONFIG_RUST", "CONFIG_ANDROID_BINDER_IPC_RUST"]:
    if any(line.startswith(name + "=") for line in config.splitlines()):
        raise SystemExit("Unexpected enabled option: " + name)
print("SHENG_GKI_612_CONFIG_PASS")

# 按串口文本行核对启动标记，保留原始换行
if len(sys.argv) == 3:
    log = Path(sys.argv[2]).read_bytes().decode("utf-8", errors="replace")
    lines = log.splitlines()
    if "SHENG_GKI_612_SMOKE_OK" not in lines:
        raise SystemExit("Missing userspace startup marker")
    if "SHENG_GKI_612_SMOKE_FAILED" in log:
        raise SystemExit("Userspace startup failed")
    if "Linux version 6.12.69-android16-6-sheng-pidns-4k" not in log:
        raise SystemExit("Kernel version mismatch")
    print("SHENG_GKI_612_QEMU_PASS")
