"""R015 仅新增具名执行准入/实际字节归属，不改变旧 ownership/cleanup。"""
import hashlib
import json
import math
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


def canonical_sha(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def read_bound(path, expected_sha):
    path = Path(path)
    require(path.is_absolute() and ".." not in path.parts, "R016 absolute evidence path required")
    require(all(not stat.S_ISLNK(item.lstat().st_mode) for item in (path, *path.parents)), "R016 linked evidence")
    require(stat.S_ISREG(path.stat().st_mode) and path.stat().st_size <= 2 * 1024 * 1024, "R016 evidence type/size")
    data = path.read_bytes()
    require(hashlib.sha256(data).hexdigest() == expected_sha, "R016 evidence SHA drift")
    return json.loads(data)


def check_history_envelope(role, ledger):
    require(isinstance(ledger, dict) and ledger.get("role") == role and isinstance(ledger.get("runs"), list),
            "R016 ledger role/history mismatch")


def control_debt(stage, role, authorization):
    config = authorization.get("r016", {})
    require(config.get("revision") == 1 and config.get("control_debt_seconds") == 60, "R016 debt authorization")
    stage = Path(stage)
    require(config.get("control_debt_path") == str(stage / "leader/r016-control-debt-002.json"), "R016 debt path")
    debt = read_bound(config["control_debt_path"], config.get("control_debt_sha256"))
    require(debt.get("schema") == "r016-control-debt-v1" and debt.get("role") == "builder" and
            type(debt.get("debt_seconds")) is int and debt["debt_seconds"] == 60 and
            debt.get("meaning") == "conservative_limit_not_measured" and
            debt.get("old_failure_path") == config.get("old_failure_path") and
            debt.get("old_failure_sha256") == config.get("old_failure_sha256"), "R016 debt binding")
    failure = read_bound(config["old_failure_path"], config["old_failure_sha256"])
    require(failure.get("budget_settlement") == "unknown" and failure.get("check_ledger_record") == "not_created" and
            failure.get("test_execution") == "not_started", "R016 original failure mismatch")
    ledger = json.loads((stage / "builder/ledger.json").read_text())
    check_history_envelope("builder", ledger)
    rows = [row for row in ledger["runs"] if row.get("run_id") == "run-r015-build-001"]
    require(len(rows) == 1 and canonical_sha(rows[0]) == debt.get("build_record_sha256"), "R016 canonical build record drift")
    return 60 if role == "builder" else 0


def claim_recovery(stage, role, run_id):
    if role != "builder" or run_id != "run-r015-check-001": return None
    stage = Path(stage)
    # Claim before the remaining admission checks; rejected attempts stay consumed.
    used = stage / "builder/cache/r016-attempt002-used.json"
    require(all(not stat.S_ISLNK(p.lstat().st_mode) for p in (used.parent, *used.parent.parents)), "R016 linked claim parent")
    descriptor = os.open(used, os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW, 0o600)
    try:
        with os.fdopen(descriptor, "w") as stream:
            json.dump(dict(schema="r016-recovery-claim-v1", role=role, run_id=run_id, attempt="attempt002",
                           state="attempted_once"), stream)
            stream.flush()
            os.fsync(stream.fileno())
    finally:
        directory = os.open(used.parent, os.O_DIRECTORY | os.O_NOFOLLOW)
        try: os.fsync(directory)
        finally: os.close(directory)
    return used


def consume_recovery(stage, role, run_id, authorization, ledger, context=None, claim=None):
    if role != "builder" or run_id != "run-r015-check-001":
        if callable(context): context()
        return
    stage = Path(stage)
    if claim is None: claim = claim_recovery(stage, role, run_id)
    require(claim == stage / "builder/cache/r016-attempt002-used.json", "R016 claim identity")
    config = authorization["r016"]
    require(config.get("attempt_path") == str(stage / "leader/r016-attempt002.json") and
            config.get("used_path") == str(claim), "R016 recovery paths")
    context = context() if callable(context) else context
    require(isinstance(context, dict), "R016 actual recovery invocation context missing")
    marker = read_bound(config["attempt_path"], context.get("attempt_sha256"))
    require(marker.get("schema") == "r016-recovery-attempt-v1" and marker.get("state") == "authorized_for_single_claim" and
            marker.get("role") == role and marker.get("run_id") == run_id and marker.get("attempt") == "attempt002" and
            marker.get("old_failure_sha256") == config["old_failure_sha256"] and
            marker.get("package_sha256") == config["common_package_sha256"] and
            marker.get("build_receipt_sha256") == config["build_receipt_sha256"], "R016 recovery marker mismatch")
    require(marker.get("command_sha256") == context.get("command_sha256") and
            marker.get("inner_argv_sha256") == context.get("inner_argv_sha256"), "R016 recovery exact command drift")
    if ledger is None:
        ledger = json.loads((stage / role / "ledger.json").read_text())
    check_history_envelope(role, ledger)
    require(not any(row.get("run_id") == run_id for row in ledger["runs"]) and not (stage / role / run_id).exists(),
            "R016 recovery logical slot already exists")
    build_rows = [row for row in ledger["runs"] if row.get("run_id") == "run-r015-build-001"]
    require(len(build_rows) == 1 and marker.get("build_record_sha256") == canonical_sha(build_rows[0]), "R016 recovery build row")


def recovery_context(args, repository, invocation):
    if args.role != "builder" or args.run_id != "run-r015-check-001":
        require(not args.r016_attempt_sha256, "R016 attempt flag outside exact recovery")
        return None
    require(isinstance(args.r016_attempt_sha256, str) and len(args.r016_attempt_sha256) == 64 and
            all(c in "0123456789abcdef" for c in args.r016_attempt_sha256), "R016 attempt SHA required")
    command_path = repository / ".cache/v0.5.1-s4/builder/cache/r016-commands-001.json"
    require(all(not p.is_symlink() for p in (command_path, *command_path.parents)), "R016 linked command table")
    rows = json.loads(command_path.read_text())["commands"]
    rows = [row for row in rows if row["run_id"] == args.run_id]
    require(len(rows) == 1, "R016 exact recovery row missing")
    actual = list(invocation)
    require(actual.count("--r016-attempt-sha256") == 1, "R016 repeated attempt flag")
    actual[actual.index("--r016-attempt-sha256") + 1] = "R016_ATTEMPT_SHA256"
    require(actual == rows[0]["argv"], "R016 actual invocation differs from frozen command")
    return dict(attempt_sha256=args.r016_attempt_sha256, command_sha256=canonical_sha(rows[0]),
                inner_argv_sha256=canonical_sha(args.command))


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
                row.get("byte_classification_status") == "verified" for row in prior), "R015 earlier failure stops all later slots")
    require(all(type(row.get("status")) is str and type(row.get("byte_classification_status")) is str and
                type(row.get("accounting_errors")) is list for row in prior), "R016 prerequisite field type")
    for row in prior:
        expected_kind, expected_seconds = SLOTS.get(row["run_id"], (None, None))
        require(row.get("kind") == expected_kind and type(row.get("reserved_seconds")) in (int, float) and
                row["reserved_seconds"] == expected_seconds and type(row.get("charged_seconds")) in (int, float) and
                math.isfinite(row["charged_seconds"]) and row["charged_seconds"] >= 0, "R016 prerequisite settlement fields")
        output = Path(row.get("output", ""))
        require(output.is_absolute() and ".." not in output.parts and output.name == row["run_id"] and
                output.parent.name == role, "R016 prerequisite output/role mismatch")
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
    total += r016_shared_bytes(repository)
    return total


def r016_shared_bytes(repository):
    repository = Path(repository)
    tool = repository / "benchmark/tail-localization"
    paths = [tool / "budget_v12.py", tool / "localize_v14.py", *sorted((tool / "r016").rglob("*"))]
    leader = repository / ".cache/v0.5.1-s4/leader"
    paths += [leader / "r015-check-admission-failure-001.json", *sorted(leader.glob("r016-control-debt-*.json"))]
    paths += [path for path in (leader / "r016-attempt002.json", leader / "r016-attempt002-called.json") if path.exists()]
    total = 0
    for path in set(paths):
        metadata = path.lstat()
        require(not stat.S_ISLNK(metadata.st_mode), "R016 shared material link")
        if stat.S_ISREG(metadata.st_mode): total += metadata.st_size
        else: require(stat.S_ISDIR(metadata.st_mode), "R016 shared special material")
    return total


def capacity_groups(stage, role, shared_bytes):
    role_root = Path(stage) / role
    repair_bytes = r016_shared_bytes(Path(stage).parents[1])
    groups = {"build": shared_bytes - repair_bytes, "baseline": 0, "other": repair_bytes}
    roots = [(role_root / "cache/measurement-baseline-r015", "build"), (role_root / "tmp", "build")]
    roots += [(path, "build") for path in (role_root / "cache").iterdir()
              if path.name.startswith("r015-")]
    roots += [(path, "other") for path in (role_root / "cache").iterdir() if path.name.startswith("r016-")]
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
                         "--governance-test": str(repository / "benchmark/tail-localization/r016/test_r015_adapter.py")})
        for flag, name in (("--governance-test-sha256", "test_r015_adapter.py"),
                           ("--governance-module-sha256", "r015_admission.py")):
            expected[flag] = hashlib.sha256((repository / "benchmark/tail-localization/r016" / name).read_bytes()).hexdigest()
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
