#!/usr/bin/env python3
"""Thin formal gate for the existing installed-SDK smoke owner; no build recipe.

77 means the requested evidence was not run. Invalid explicitly selected SDKs
and incomplete installed payloads fail; a normal cross build is not a run proof.
"""
import argparse
import importlib.util
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--consumer", choices=("c99", "scripting"), required=True)
    parser.add_argument("--sdk", default="")
    parser.add_argument("--arch", default="")
    parser.add_argument("--variant", choices=("release", "dist"), default="release")
    parser.add_argument("--delivery-profile", choices=("authoring", "embedded"), default="authoring")
    parser.add_argument("--build-dir", type=Path, required=True)
    args = parser.parse_args(argv)
    if not args.sdk.strip():
        print("JCE_SDK_CONSUMER: SKIPPED — JCE_TEST_SDK_DIR is not explicitly configured", flush=True)
        return 77
    spec = importlib.util.spec_from_file_location("jce_sdk_consumer_driver", ROOT / "tools/build/jce.py")
    driver = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = driver
    spec.loader.exec_module(driver)
    args.require_run = True
    args.introspect = ""
    args.introspect_out = ""
    try:
        driver.cmd_smoke(args)
    except SystemExit as error:
        return error.code if isinstance(error.code, int) else 1
    except (OSError, ValueError, RuntimeError) as error:
        print(f"JCE_SDK_CONSUMER: FAIL — {error}", file=sys.stderr, flush=True)
        return 1
    print(f"JCE_SDK_CONSUMER: PASS {args.consumer} (native installed-SDK Noop run)", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
