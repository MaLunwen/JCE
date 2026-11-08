"""First-party stock component producer, separate from strict app admission.

Builds only the maintained Windows x64 Lua/JS runtime consumer. The stock EXE
has no selected scene or appended payload and is not a runnable application.
Packing and COFF embedding use the existing SDK public host tools; this module
contains no archive encoder, compiler discovery, alternate app recipe or SDK.
"""
from __future__ import annotations
from contextlib import nullcontext
import json
from pathlib import Path
import time
import uuid

from tools.build import delivery, sdk_product, stock_app

TARGET = "jce_stock_runtime_verify"
CACHE = Path("build/consumers/stock-runtime/windows-x64-dist")


def _changed_write(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.is_file() or path.read_bytes() != data:
        path.write_bytes(data)


def _snapshot(root):
    source = root / "tools/runtime"
    paths = [path for path in source.rglob("*") if path.is_file() and
             not any(part in ("build", "__pycache__", ".git") for part in path.relative_to(source).parts)]
    paths.extend((root / "tools/build/stock_runtime.py", root / "tools/build/stock_app.py", root / "LICENSE"))
    if not paths or len(paths) > 512:
        raise stock_app.StockError("stock producer source roster unavailable or excessive")
    return {str(path): stock_app._sha(stock_app._read(path, 1024 * 1024)) for path in sorted(paths)}


def _verify_sdk(driver, sdk):
    return sdk_product.verify_product(sdk, system="Windows", arch="x86_64",
        config="Release", variant="dist", delivery="embedded",
        expected_version=sdk_product.source_version(driver.ROOT), required_paths=(
            "bin/jce_pak.exe", "bin/jce_bin2obj.exe", "share/jce/THIRD_PARTY_LICENSES.md",
            "lib/cmake/JCE/JCEConfig.cmake"))


def _output_identities(pak, obj, bom, key_source):
    return {str(path): stock_app._sha(stock_app._read(path, cap)) for path, cap in (
        (pak, stock_app.HOST_LIMIT), (obj, stock_app.HOST_LIMIT),
        (bom, 16 * 1024 * 1024), (key_source, 1024 * 1024))}


def _reuse_stamp(path, fingerprint, pak, obj, bom, key_source):
    if not path.is_file():
        return False
    try:
        stamp = stock_app._json(stock_app._read(path, 64 * 1024))
        return (type(stamp) is dict and set(stamp) == {"schema", "fingerprint", "outputs"} and
            type(stamp["schema"]) is int and stamp["schema"] == 1 and
            stamp["fingerprint"] == fingerprint and
            stamp["outputs"] == _output_identities(pak, obj, bom, key_source))
    except (ValueError, OSError):
        return False


def _fresh_deep_report(path, pak):
    report = delivery._read_public_json(path)
    header, totals = report.get("header", {}), report.get("totals", {})
    count = totals.get("entries")
    if (report.get("schema") != "jce.pakbom.v1" or report.get("file") != str(pak) or
            report.get("file_size") != pak.stat().st_size or type(count) is not int or count < 1 or
            any(header.get(key) is not True for key in
                ("header_verified", "encrypted", "secure_index", "authenticated", "auth_verified")) or
            totals.get("verify_ran") is not True or totals.get("verified_count") != count or
            totals.get("corrupt_count") != 0 or totals.get("skipped_count") != 0 or
            totals.get("encrypted_count") != count or totals.get("authenticated_count") != count):
        raise stock_app.StockError("reused stock archive lacks fresh authenticated deep verification")


def build(driver, args):
    """One fixed consumer cache under the shared driver SDK/cache leases."""
    if driver is None:
        raise stock_app.StockError("build-runtime requires the supported scripts/jce.py driver")
    if driver.HOST != "windows":
        raise stock_app.StockError("stock runtime producer currently supports Windows x64 only")
    target = driver.resolve_target("x64")
    if target.get("host") != "windows" or target.get("arch") != "x86_64":
        raise stock_app.StockError("stock runtime producer needs the Windows x64 target")
    root = Path(driver.ROOT).resolve()
    sdk = Path(args.sdk).absolute()
    directory = root / CACHE
    jobs = getattr(args, "jobs", None)
    if jobs is not None and (type(jobs) is not int or jobs < 1):
        raise stock_app.StockError("stock producer jobs must be positive")
    if driver.DRY_RUN:
        raise stock_app.StockError("stock runtime producer requires actual SDK/asset verification; dry-run is not evidence")
    with driver._cache_scope([directory, sdk]):
        _verify_sdk(driver, sdk)
        product_before = stock_app._read(sdk / sdk_product.PRODUCT_FILE, sdk_product.MAX_MANIFEST_BYTES)
        inputs = _snapshot(root)
        compile_identity = stock_app._sha(json.dumps({"sdk": stock_app._sha(product_before),
            "sources": inputs}, sort_keys=True, separators=(",", ":")).encode())
        tools = driver._sdk_host_tools(sdk, target)
        if not {"jce_pak", "jce_bin2obj"} <= set(tools):
            raise stock_app.StockError("selected SDK lacks executable public PAK/bin2obj tools; no fallback is permitted")
        for name in ("jce_pak", "jce_bin2obj"):
            if Path(tools[name]).resolve() != (sdk / "bin" / (name + ".exe")).resolve():
                raise stock_app.StockError("stock producer tool escaped the explicitly verified native SDK")
        environment = driver.msvc_env(target)
        if jobs is not None:
            environment["CMAKE_BUILD_PARALLEL_LEVEL"] = str(jobs)
        parallel = driver.build_jobs(environment)
        directory.mkdir(parents=True, exist_ok=True)
        run_dir = directory / "receipts" / uuid.uuid4().hex
        run_dir.mkdir(parents=True, exist_ok=False)
        receipt_path = run_dir / "receipt.json"
        receipt = {"schema": 1, "scope": "stock-runtime-component-producer",
            "status": "STARTED", "returncode": None,
            "sdk": {"path": str(sdk), "product_sha256": stock_app._sha(product_before)},
            "cache": str(directory), "target": dict(stock_app._TARGET),
            "inputs_before": inputs, "compile_input_sha256": compile_identity, "commands": [],
            "runnable_application": False, "runtime_execution_verified": False,
            "relocation_verified": False}
        _changed_write(receipt_path, (json.dumps(receipt, indent=2, sort_keys=True) + "\n").encode())
        def run(command):
            receipt["commands"].append([str(part) for part in command])
            driver.run(command, env=environment, cwd=root)
        try:
            generated = directory / "stock-generated"
            resources = generated / "resources"
            notices = generated / "notice_sources"
            license_bytes = stock_app._read(root / "LICENSE", 4 * 1024 * 1024)
            third_bytes = stock_app._read(sdk / "share/jce/THIRD_PARTY_LICENSES.md", 4 * 1024 * 1024)
            for name, value in (("LICENSE", license_bytes), ("THIRD_PARTY_LICENSES.md", third_bytes)):
                _changed_write(resources / "licenses" / name, value)
                _changed_write(notices / name, value)
            # An owned output stage must never grow hidden boot/content inputs.
            if {path.relative_to(resources).as_posix() for path in resources.rglob("*") if path.is_file()} != {
                    "licenses/LICENSE", "licenses/THIRD_PARTY_LICENSES.md"}:
                raise stock_app.StockError("unexpected content in owned stock notice resource stage")
            key = driver._project_pak_key(directory)
            key_file = directory / ".jce/pak_key.hex"
            if stock_app._key_bytes(stock_app._read(key_file, 128)) != key:
                raise stock_app.StockError("producer key bytes disagree with the owned key file")
            key_source = generated / "stock_pak_key.c"
            pak, obj = generated / "stock_assets.pak", generated / "stock_assets.obj"
            bom = generated / "stock_assets.bom.json"
            roots = [sdk / "share/jce" / name for name in ("engine_resources", "engine_ui")
                     if (sdk / "share/jce" / name).is_dir()]
            if not roots:
                raise stock_app.StockError("selected SDK lacks stock runtime resource trees")
            pack = [str(tools["jce_pak"])]
            for source in (*roots, resources):
                pack.extend(("--resource-dir", str(source)))
            pack.extend(("--pak-file", str(pak), "--header-file", str(generated / "stock_assets.h"),
                "--manifest-file", str(generated / "stock_assets.cmake"), "--json", str(bom),
                "--obj-format", "none", "--symbol-prefix", "assets_pak_data", "--level", "19",
                "--platform", "desktop", "--bundle-target", "windows", "--encrypt-key-file", str(key_file),
                "--exclude-segment", "AGENTS.md", "--exclude-segment", "agents.md"))
            embed_command = [str(tools["jce_bin2obj"]), "--input", str(pak), "--symbol", "assets_pak_data",
                             "--output", str(obj), "--format", "coff", "--arch", "x64"]
            asset_fingerprint = stock_app._sha(json.dumps({
                "sdk": stock_app._sha(product_before), "key": stock_app._sha(key),
                "license": stock_app._sha(license_bytes), "third_party": stock_app._sha(third_bytes),
                "pack_command": pack, "embed_command": embed_command},
                sort_keys=True, separators=(",", ":")).encode())
            stamp_path = generated / "assets-success.json"
            reused = _reuse_stamp(stamp_path, asset_fingerprint, pak, obj, bom, key_source)
            if reused:
                deep_report = run_dir / "assets-reuse-inspection.json"
                run([str(tools["jce_pak"]), "--inspect", str(pak), "--verify", "--key-file", str(key_file),
                     "--json", str(deep_report), "--platform", "desktop", "--bundle-target", "windows"])
                _fresh_deep_report(deep_report, pak)
            else:
                # Only this producer's own generated outputs are replaced.
                stamp_path.unlink(missing_ok=True)
                for path in (pak, obj, bom):
                    path.unlink(missing_ok=True)
                driver._write_pak_key_shares_c(key_source, key)
                run(pack)
                run(embed_command)
            report = delivery._read_public_json(bom)
            count = delivery.verify_selected_pack(report, pak, obj, [], [], {})
            addresses = {row["path"] for row in report["sources"]}
            if ("jce/runtime_boot.json" in addresses or "__bundle__/manifest.json" in addresses or
                    not {"licenses/license", "licenses/third_party_licenses.md"} <= addresses or
                    any(part == "agents.md" for address in addresses for part in address.split("/"))):
                raise stock_app.StockError("stock component contains boot metadata or lacks actual notices")
            if not reused:
                _changed_write(stamp_path, (json.dumps({"schema": 1, "fingerprint": asset_fingerprint,
                    "outputs": _output_identities(pak, obj, bom, key_source)},
                    sort_keys=True, indent=2) + "\n").encode())
            receipt["assets_reused"] = reused
            cmake = ["cmake", "-S", str(root / "tools/runtime"), "-B", str(directory), "-G", "Ninja",
                "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl",
                "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded", "-DJCE_BUILD_VARIANT=dist",
                "-DJCE_STOCK_INPUT_SHA256=" + compile_identity,
                "-DJCE_DELIVERY_PROFILE=embedded", "-DJCE_DIR=" + str(sdk / "lib/cmake/JCE"),
                "-U", "JCE_PROJECT_PREBUILT_ASSETS_*", "-U", "JCE_PROJECT_PREBUILT_PAK_KEY_C",
                "-U", "JCE_PROJECT_BUNDLES", "-U", "JCE_PROJECT_PREBUILT_BUNDLE_DIR",
                "-DJCE_PAK_EXECUTABLE=" + str(tools["jce_pak"]),
                "-DJCE_BIN2OBJ_EXECUTABLE=" + str(tools["jce_bin2obj"]),
                "-DJCE_PROJECT_PREBUILT_ASSETS_OBJ=" + str(obj),
                "-DJCE_PROJECT_PREBUILT_PAK_KEY_C=" + str(key_source),
                "-DJCE_PROJECT_STAGE_LOOSE_ASSETS=OFF", "-DJCE_STOCK_LICENSE_FILE=" + str(root / "LICENSE")]
            run(cmake)
            started = time.time_ns()
            run(["cmake", "--build", str(directory), "--target", TARGET, "--parallel", parallel])
            exe = directory / "jce_stock_runtime.exe"
            host = stock_app._read(exe, stock_app.HOST_LIMIT)
            stock_app._host(host)
            imports = stock_app.standalone.verify(exe)["imports"]
            observed = stock_app.standalone.verify_notices(exe, notices)
            if observed != {"LICENSE": stock_app._sha(license_bytes), "THIRD_PARTY_LICENSES.md": stock_app._sha(third_bytes)}:
                raise stock_app.StockError("stock executable notices do not match actual producer inputs")
            if stock_app._read(pak, stock_app.HOST_LIMIT) not in host:
                raise stock_app.StockError("deep-verified stock resource PAK is not embedded in the executable")
            last_test = directory / "Testing/Temporary/LastTest.log"
            if not last_test.is_file() or last_test.stat().st_mtime_ns < started:
                raise stock_app.StockError("stock CPU controls lack a fresh native CTest log")
            test_raw = stock_app._read(last_test, 8 * 1024 * 1024)
            _changed_write(run_dir / "LastTest.log", test_raw)
            if _snapshot(root) != inputs:
                raise stock_app.StockError("stock producer source changed during build")
            _verify_sdk(driver, sdk)
            if stock_app._read(sdk / sdk_product.PRODUCT_FILE, sdk_product.MAX_MANIFEST_BYTES) != product_before:
                raise stock_app.StockError("selected SDK product changed during stock build")
            receipt.update(status="STOCK_COMPONENT_BUILT_CPU_CONTROLS_PASSED_NOT_AN_APPLICATION", returncode=0,
                inputs_after=inputs, output={"path": str(exe), "size": len(host), "sha256": stock_app._sha(host)},
                assets={"path": str(pak), "sha256": stock_app._sha(stock_app._read(pak, stock_app.HOST_LIMIT)),
                        "bom_path": str(bom), "bom_sha256": stock_app._sha(stock_app._read(bom, 16 * 1024 * 1024)),
                        "entry_count": count, "has_runtime_boot": False},
                sealing_key={"path": str(key_file), "sha256": stock_app._sha(key)},
                imports=imports, native_cpu_log={"path": str(run_dir / "LastTest.log"), "sha256": stock_app._sha(test_raw)})
        except BaseException as error:
            receipt.update(status="FAILED", returncode=getattr(error, "returncode", 1), error=str(error))
            raise
        finally:
            _changed_write(receipt_path, (json.dumps(receipt, indent=2, sort_keys=True) + "\n").encode())
        return {"receipt_path": str(receipt_path), "receipt_sha256": stock_app._sha(stock_app._read(receipt_path, 1024 * 1024)),
                **receipt}
