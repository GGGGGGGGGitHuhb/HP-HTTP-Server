"""R015 仅新增具名执行准入/实际字节归属，不改变旧 ownership/cleanup。"""
import hashlib
import json
import os
from pathlib import Path
import stat

SLOTS = {"run-r015-build-001": ("selfcheck", 180), "run-r015-check-001": ("selfcheck", 60),
         "run-r015-smoke-001": ("smoke", 20), "run-r015-baseline-001": ("baseline", 45),
         "run-r015-offline-001": ("offline", 30)}
START_BYTES = {"builder": 1486288348, "reviewer": 117361673}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def check_slot(role, run_id, kind, seconds, authorization, runs, current_bytes, shared_bytes):
    if not run_id.startswith("run-r015-"):
        require(kind != "baseline", "baseline kind requires exact R015 slot")
        return False
    config = authorization.get("r015", {})
    require(config.get("revision") == 1 and config.get("no_automatic_retries") is True,
            "R015 authorization missing")
    require(role in ("builder", "reviewer") and run_id in SLOTS and SLOTS[run_id] == (kind, seconds),
            "R015 exact role/run/kind/deadline mismatch")
    require(not (role == "reviewer" and run_id == "run-r015-baseline-001"), "Builder-only baseline")
    require(not any(row["run_id"] == run_id for row in runs), "R015 once-only slot consumed")
    prior = [row for row in runs if row["run_id"].startswith("run-r015-")]
    require(all(row["status"] == "valid" and row.get("accounting_errors") == [] and
                row.get("byte_accounting") == "verified" for row in prior), "R015 earlier failure stops all later slots")
    sequence = list(SLOTS)
    if role == "reviewer":
        sequence.remove("run-r015-baseline-001")
    position = sequence.index(run_id)
    require([row["run_id"] for row in prior] == sequence[:position], "R015 prerequisite sequence missing/drift")
    require(config.get("per_role_output_bytes") == 512 * 1024 * 1024 and
            config.get("build_output_bytes") == 320 * 1024 * 1024 and
            config.get("baseline_output_bytes") == 128 * 1024 * 1024 and
            config.get("other_output_bytes") == 64 * 1024 * 1024, "R015 full capacity contract drift")
    require(shared_bytes >= 0 and current_bytes >= START_BYTES[role], "R015 byte origin unknown")
    if run_id == "run-r015-build-001":
        require(current_bytes + shared_bytes + 512 * 1024 * 1024 + 8 * 1024 * 1024 <= authorization["per_role_output_bytes"],
                "whole R015 plan cannot fit before build")
    return True


def shared_package_bytes(repository):
    root = Path(repository) / "benchmark/tail-localization/measurement-baseline"
    total = 0
    for directory, subdirectories, filenames in os.walk(root, followlinks=False):
        for name in subdirectories + filenames:
            path = Path(directory) / name
            metadata = path.lstat()
            require(not stat.S_ISLNK(metadata.st_mode), "shared package link rejected")
            if stat.S_ISREG(metadata.st_mode):
                total += metadata.st_size
            else:
                require(stat.S_ISDIR(metadata.st_mode), "shared package special node")
    for name in ("budget_v11.py", "localize_v13.py", "r015_admission.py", "test_r015_adapter.py"):
        metadata = (Path(repository) / "benchmark/tail-localization" / name).lstat()
        require(stat.S_ISREG(metadata.st_mode), "R015 governance source not regular")
        total += metadata.st_size
    return total


def capacity_groups(stage, role, shared_bytes):
    role_root = Path(stage) / role
    groups = {"build": shared_bytes, "baseline": 0, "other": 0}
    roots = [(role_root / "cache/measurement-baseline-r015", "build"), (role_root / "tmp", "build")]
    roots += [(path, "build") for path in (role_root / "cache").iterdir()
              if path.name.startswith("r015-")]
    roots += [(role_root / run_id, "build" if "build" in run_id else "baseline" if "baseline" in run_id else "other")
              for run_id in SLOTS]
    for root, category in roots:
        try:
            root_metadata = root.lstat()
        except FileNotFoundError:
            continue
        require(not stat.S_ISLNK(root_metadata.st_mode), "R015 output root link rejected")
        if stat.S_ISREG(root_metadata.st_mode):
            groups[category] += root_metadata.st_size
            continue
        require(stat.S_ISDIR(root_metadata.st_mode), "R015 output root type rejected")
        for directory, subdirectories, filenames in os.walk(root, followlinks=False):
            for name in subdirectories + filenames:
                path = Path(directory) / name
                metadata = path.lstat()
                require(not stat.S_ISLNK(metadata.st_mode), "R015 output link rejected")
                if stat.S_ISREG(metadata.st_mode):
                    groups[category] += metadata.st_size
                else:
                    require(stat.S_ISDIR(metadata.st_mode), "R015 special output")
    require(groups["build"] <= 320 * 1024 * 1024 and groups["baseline"] <= 128 * 1024 * 1024 and
            groups["other"] <= 64 * 1024 * 1024, "R015 category bytes exhausted")
    return groups


def prepare_environment(args, reservation, repository, environment, governor_identity):
    if not args.run_id.startswith("run-r015-"):
        return
    require(governor_identity is not None, "governor identity missing")
    command = args.command
    require(command[:2] == ["/usr/bin/python3", "-I"], "R015 explicit isolated Python required")
    script = Path(command[2]).absolute()
    role_root = Path(reservation.root) / args.role
    primary_package = role_root / "cache/measurement-baseline-r015"
    build_output = role_root / "run-r015-build-001/build-output"
    relocated_package = build_output / "relocated-package"
    expected_package = primary_package if args.run_id in ("run-r015-build-001", "run-r015-check-001") else relocated_package
    bundle = role_root / "cache/r015-verification-bundle-003"
    reviewer_offline = args.role == "reviewer" and args.run_id == "run-r015-offline-001"
    require(script.parent == (bundle if reviewer_offline else expected_package), "R015 active package path mismatch")
    expected_script = {"run-r015-build-001": "build_package.py", "run-r015-check-001": "check_package.py",
                       "run-r015-smoke-001": "execute_sample.py", "run-r015-baseline-001": "execute_sample.py",
                       "run-r015-offline-001": "verify_sample.py"}[args.run_id]
    require(script.name == ("r015_review_verify_001.py" if reviewer_offline else expected_script), "R015 script mismatch")
    flags = command[3:]
    require(len(flags) % 2 == 0, "R015 arguments must be explicit flag/value pairs")
    pairs = list(zip(flags[::2], flags[1::2]))
    offline = args.run_id == "run-r015-offline-001"
    require(len({key for key, value in pairs}) == len(pairs) - (1 if offline else 0), "R015 repeated argument")
    supplied = dict(pairs)
    expected = {"--output-root": str(reservation.output)}
    if args.run_id == "run-r015-build-001":
        expected.update({"--package-root": str(primary_package), "--output-root": str(build_output)})
    elif args.run_id == "run-r015-check-001":
        expected.update({"--package-root": str(primary_package), "--build-output": str(build_output),
                         "--governance-test": str(repository / "benchmark/tail-localization/test_r015_adapter.py")})
        for flag, name in (("--governance-test-sha256", "test_r015_adapter.py"),
                           ("--governance-module-sha256", "r015_admission.py")):
            expected[flag] = hashlib.sha256((repository / "benchmark/tail-localization" / name).read_bytes()).hexdigest()
        if args.role == "reviewer":
            expected.update({"--verification-bundle": str(bundle), "--verification-bundle-sha256":
                             "762a67ec34542b2fd12271d5e4e27befe84c05f526322bbdedc4158a22726898"})
    elif args.run_id in ("run-r015-smoke-001", "run-r015-baseline-001"):
        expected.update({"--role": args.role, "--run-id": args.run_id, "--build-output": str(build_output)})
    else:
        expected["--sample-dir"] = str(role_root / "run-r015-baseline-001")
        if reviewer_offline:
            expected.update({"--common-package": str(relocated_package), "--sample-dir":
                             str(Path(reservation.root) / "builder/run-r015-baseline-001")})
        require([value for key, value in pairs if key == "--sample-dir"] ==
                [str(role_root / "run-r015-smoke-001"), expected["--sample-dir"]], "R015 exact two samples")
    require(supplied == expected, "R015 exact inner arguments mismatch")
    package_sha = hashlib.sha256((expected_package / "inputs-lock.json").read_bytes()).hexdigest()
    require(getattr(args, "r015_package_sha256", None) == package_sha, "sealed R015 package identity drift")
    for key in ("LD_PRELOAD", "LUA_INIT", "LUA_INIT_5_1", "PYTHONPATH", "PYTHONHOME"):
        environment.pop(key, None)
    environment.update(HP_BASELINE_ROLE=args.role, HP_BASELINE_RUN=args.run_id,
                       HP_BASELINE_OUTPUT_ROOT=str(reservation.output),
                       HP_BASELINE_PACKAGE_SHA256=package_sha,
                       HP_BASELINE_GOVERNOR_PID=str(governor_identity["pid"]),
                       HP_BASELINE_GOVERNOR_STARTTIME=str(governor_identity["starttime"]),
                       HP_BASELINE_WORK_DEADLINE=str(reservation.started + args.seconds - 7),
                       HP_BASELINE_CLEANUP_DEADLINE=str(reservation.started + args.seconds),
                       LD_LIBRARY_PATH=str(expected_package / "runtime"),
                       LUA_PATH=str(expected_package / "runtime/lua/?.lua") + ";" + str(expected_package / "runtime/lua/?/init.lua"),
                       LUA_CPATH="")
    if args.run_id not in ("run-r015-build-001",):
        receipt_sha = hashlib.sha256((build_output / "build-receipt.json").read_bytes()).hexdigest()
        builds = [row for row in reservation.ledger["runs"] if row["run_id"] == "run-r015-build-001"]
        require(len(builds) == 1 and builds[0].get("r015_build_receipt_sha256") == receipt_sha and
                builds[0].get("r015_package_sha256") == package_sha, "successful build artifact binding drift")
        environment["HP_BASELINE_BUILD_RECEIPT_SHA256"] = receipt_sha


def record_build_artifact(args, reservation, environment):
    if not args.run_id.startswith("run-r015-"):
        return
    reservation.record["r015_package_sha256"] = environment["HP_BASELINE_PACKAGE_SHA256"]
    if args.run_id == "run-r015-build-001":
        receipt = reservation.output / "build-output/build-receipt.json"
        reservation.record["r015_build_receipt_sha256"] = hashlib.sha256(receipt.read_bytes()).hexdigest()
