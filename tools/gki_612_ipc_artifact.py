from pathlib import Path
import hashlib
import json
import sys
import tarfile
import zipfile


SOURCE_REPO = "INTERINIT/anyhmos_xiaomi_sheng"
SOURCE_RUN = 41711381488
SOURCE_COMMIT = "641212e7b1163cfd828be134710f6f47d70f2800"
ARTIFACT_ID = 13269866707
ARTIFACT_SHA256 = "4dc9d95e2b362334a7feb52dd5fb1df2694f6bed22ed2fbcfaf419d89cb229be"
IMAGE_SHA256 = "82800e984ad1c01d282cf8c097b789abcf7516443fa1cca65914c230968e7214"
MODULE_SHA256 = "3ba9c5cbf3ff4d8d194d3e04303a745497904afd0f3fa5238048ec897202dafe"
RELEASE = "6.12.69-android16-6-sheng-pidns-4k+"


def digest(data):
    return hashlib.sha256(data).hexdigest()


# 固定同次构建的镜像与签名模块，拒绝跨构建混用
def prepare(archive, destination, report, metadata):
    meta = json.loads(Path(metadata).read_text(encoding="utf-8"))
    if meta["id"] != ARTIFACT_ID or meta["workflow_run"]["id"] != SOURCE_RUN or \
        meta["workflow_run"]["head_sha"] != SOURCE_COMMIT or meta["expired"]:
        raise SystemExit("Source artifact identity mismatch")
    archive = Path(archive)
    if digest(archive.read_bytes()) != ARTIFACT_SHA256:
        raise SystemExit("Source artifact digest mismatch")
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    selected = ["Image", "kernel.config", "kernel.release", "modules.tar.gz",
        "smoke-initramfs.cpio.gz"]
    audit = {}
    with zipfile.ZipFile(archive) as bundle:
        if len(bundle.namelist()) != len(set(bundle.namelist())):
            raise SystemExit("Duplicate source archive entries")
        for name in selected:
            entry = bundle.getinfo(name)
            if entry.file_size > 80 * 1024 * 1024:
                raise SystemExit("Unexpected source entry size")
            data = bundle.read(name)
            (destination / name).write_bytes(data)
            audit[name] = {"bytes": len(data), "sha256": digest(data)}
    if audit["Image"]["sha256"] != IMAGE_SHA256 or \
        (destination / "kernel.release").read_text().strip() != RELEASE:
        raise SystemExit("Kernel image or release mismatch")
    module_path = "lib/modules/" + RELEASE + "/updates/sheng_hmos_ipc.ko"
    with tarfile.open(destination / "modules.tar.gz", "r:gz") as modules:
        matches = [entry for entry in modules if entry.name == module_path]
        if len(matches) != 1 or not matches[0].isfile() or matches[0].size > 8 * 1024 * 1024:
            raise SystemExit("Missing unique regular signed module")
        module = modules.extractfile(matches[0]).read()
    if digest(module) != MODULE_SHA256 or \
        not module.endswith(b"~Module signature appended~\n"):
        raise SystemExit("Signed module digest mismatch")
    rootfs = destination / "rootfs"
    (rootfs / "probe").mkdir(parents=True, exist_ok=True)
    (rootfs / "sys").mkdir(parents=True, exist_ok=True)
    (rootfs / "probe" / "sheng_hmos_ipc.ko").write_bytes(module)
    audit["signed_module"] = {"path": module_path, "bytes": len(module),
        "sha256": digest(module)}
    result = {"source_repository": SOURCE_REPO, "source_run": SOURCE_RUN,
        "source_commit": SOURCE_COMMIT, "artifact_id": ARTIFACT_ID,
        "artifact_sha256": ARTIFACT_SHA256, "files": audit,
        "full_ipc_transactions_tested": False, "device_tested": False}
    Path(report).write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")


# 保留原启动门禁，并核对新增模块入口的完整成功标记
def verify(log_path):
    log = Path(log_path).read_bytes().decode("utf-8", errors="replace")
    lines = log.splitlines()
    required = ["SHENG_GKI_612_SIG_ENFORCED", "SHENG_GKI_612_FINIT_RESULT=0",
        "SHENG_GKI_612_IPC_LOAD_OK", "SHENG_GKI_612_BINDER_VERSION_8",
        "SHENG_GKI_612_IPC_SMOKE_OK"]
    for marker in required:
        if lines.count(marker) != 1:
            raise SystemExit("Missing or duplicate IPC marker: " + marker)
    for marker in ["SHENG_IPC private-api-end ret=0", "SHENG_IPC binder-end ret=0"]:
        if marker not in log:
            raise SystemExit("Missing module initialization evidence: " + marker)
    for failed in ["SHENG_GKI_612_IPC_SMOKE_FAILED", "Kernel panic",
        "CFI failure", "module verification failed", "Unknown symbol",
        "disagrees about version", "BUG:", "WARNING:"]:
        if failed in log:
            raise SystemExit("IPC runtime failure: " + failed)
    print("SHENG_GKI_612_IPC_LOAD_AND_VERSION_PASS")


if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "url":
        print("https://api.github.com/repos/" + SOURCE_REPO +
            "/actions/artifacts/" + str(ARTIFACT_ID))
    elif len(sys.argv) == 6 and sys.argv[1] == "prepare":
        prepare(*sys.argv[2:])
    elif len(sys.argv) == 3 and sys.argv[1] == "verify":
        verify(sys.argv[2])
    else:
        raise SystemExit("Usage: gki_612_ipc_artifact.py url|prepare ZIP DEST REPORT META|verify LOG")
