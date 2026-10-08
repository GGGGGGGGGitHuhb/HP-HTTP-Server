"""固定两直接子进程；PID/starttime 核实后才发信号，未知身份不认领。"""
import errno
import os
import signal
import time
from pathlib import Path


def process_identity(pid):
    try:
        value = Path(f"/proc/{pid}/stat").read_text()
    except OSError as error:
        if error.errno in (errno.ENOENT, errno.ESRCH):
            return None
        raise
    end = value.rfind(")")
    if end < 0:
        raise ValueError("malformed process stat")
    fields = value[end + 2:].split()
    if len(fields) <= 19:
        raise ValueError("short process stat")
    return {"pid": pid, "starttime": int(fields[19]), "state": fields[0]}


def sample_resources(identity):
    try:
        value = Path(f"/proc/{identity['pid']}/stat").read_text()
    except OSError as error:
        if error.errno in (errno.ENOENT, errno.ESRCH):
            return None
        raise
    end = value.rfind(")")
    if end < 0:
        raise ValueError("malformed resource stat")
    fields = value[end + 2:].split()
    if len(fields) <= 21 or int(fields[19]) != identity["starttime"]:
        raise ValueError("resource identity unknown/reused")
    rss_pages = int(fields[21])
    if rss_pages < 0:
        raise ValueError("negative resource RSS")
    return {"pid": identity["pid"], "starttime": identity["starttime"], "observed_ns": time.monotonic_ns(),
            "utime_ticks": int(fields[11]), "stime_ticks": int(fields[12]),
            "clock_ticks_per_second": os.sysconf("SC_CLK_TCK"),
            "rss_bytes": rss_pages * os.sysconf("SC_PAGE_SIZE")}


class OwnedProcess:
    def __init__(self, process, identity):
        self.process = process
        self.identity = identity

    def signal_verified(self, signum):
        actual = process_identity(self.process.pid)
        if actual is None:
            return "exited"
        if self.identity is None or actual["starttime"] != self.identity["starttime"]:
            raise RuntimeError("process identity unknown/reused")
        try:
            os.kill(self.process.pid, signum)
        except OSError as error:
            if error.errno not in (errno.ENOENT, errno.ESRCH):
                raise
        return "signalled"

    def close(self, deadline):
        result = {"identity": self.identity, "reaped": False, "errors": [], "unknown": []}
        if self.process.poll() is None and time.monotonic() >= deadline:
            result["unknown"].append({"pid": self.process.pid, "identity": self.identity, "reason": "deadline_exhausted"})
            result["returncode"] = self.process.returncode
            return result
        if self.process.poll() is None:
            try:
                self.signal_verified(signal.SIGTERM)
            except BaseException as error:
                result["errors"].append({"operation": "SIGTERM", "type": type(error).__name__, "errno": getattr(error, "errno", None)})
                result["unknown"].append({"pid": self.process.pid, "identity": self.identity})
            try:
                self.process.wait(timeout=max(0, min(0.5, deadline - time.monotonic())))
            except BaseException as error:
                result["errors"].append({"operation": "wait_after_TERM", "type": type(error).__name__, "errno": getattr(error, "errno", None)})
            if self.process.poll() is None and time.monotonic() < deadline:
                try:
                    self.signal_verified(signal.SIGKILL)
                except BaseException as error:
                    result["errors"].append({"operation": "SIGKILL", "type": type(error).__name__, "errno": getattr(error, "errno", None)})
                try:
                    self.process.wait(timeout=max(0, deadline - time.monotonic()))
                except BaseException as error:
                    result["errors"].append({"operation": "final_wait", "type": type(error).__name__, "errno": getattr(error, "errno", None)})
        result["reaped"] = self.process.poll() is not None
        result["returncode"] = self.process.returncode
        if not result["reaped"]:
            result["unknown"].append({"pid": self.process.pid, "identity": self.identity, "reason": "not_reaped"})
        return result
