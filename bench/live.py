"""The live bench: the twin of docs/MODEL.md in real time against Titra on QNX, and the fault matrix.

    python -m bench.live [--cots 3] [--warmup 360] [--cases F1,F2,...] [--types] [--out eval/results/faults.json]

For each cot the bench listens on two TCP ports, the oximeter's and the blender's (5551 + 2(N-1) and the next one),
as serial-to-Ethernet converter ports would. The QNX drivers connect to them from the guest (10.0.2.2 is this host).
Every second each cot's simulated baby breathes, the oximeter sends a $OX frame, and the blender twin applies the
$BL frames it receives and answers with $BS. Faults on the sensor and the links are injected here; faults on the
QNX processes are injected over ssh. Every time is taken on this host's monotonic clock, outside the guest.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from bench.blender import Blender
from bench.frames import FrameError, LineSplitter, parse_bl, parse_ox
from bench.metrics import in_target
from bench.oximeter import Oximeter
from bench.patient import Patient
from bench.scenario import draw

ROOT = Path(__file__).resolve().parents[1]
FAULTS = ROOT / "eval" / "fault_matrix.json"
PORT_BASE = 5551
SCENARIOS = {1: ("moderate", 0), 2: ("moderate", 1), 3: ("unstable", 0)}


@dataclass
class Event:
    t: float
    cot: int
    kind: str
    data: dict[str, Any] = field(default_factory=dict)


class Recorder:
    def __init__(self) -> None:
        self.t0 = time.monotonic()
        self.events: list[Event] = []

    def now(self) -> float:
        return time.monotonic() - self.t0

    def add(self, cot: int, kind: str, **data: Any) -> Event:
        e = Event(self.now(), cot, kind, data)
        self.events.append(e)
        return e

    def since(self, t: float, cot: int | None = None, kind: str | None = None) -> list[Event]:
        return [
            e
            for e in self.events
            if e.t >= t and (cot is None or e.cot == cot) and (kind is None or e.kind == kind)
        ]


class Link:
    """One listening port with at most one client: the newest connection replaces the old one."""

    def __init__(self, port: int, on_data: Callable[[bytes], None], on_connect: Callable[[], object]) -> None:
        self.port = port
        self.on_data = on_data
        self.on_connect = on_connect
        self.writer: asyncio.StreamWriter | None = None
        self.refusing = False

    async def start(self) -> None:
        await asyncio.start_server(self._client, "0.0.0.0", self.port)

    async def _client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        if self.refusing:
            writer.close()
            return
        if self.writer is not None:
            self.writer.close()
        self.writer = writer
        self.on_connect()
        try:
            while not reader.at_eof():
                data = await reader.read(512)
                if not data:
                    break
                if not self.refusing:
                    self.on_data(data)
        except (ConnectionError, OSError):
            pass
        finally:
            if self.writer is writer:
                self.writer = None
            writer.close()

    def write(self, data: bytes) -> None:
        if self.writer is not None and data:
            try:
                self.writer.write(data)
            except (ConnectionError, OSError):
                self.writer = None

    def drop(self) -> None:
        if self.writer is not None:
            self.writer.close()
            self.writer = None


class CotTwin:
    """One cot's simulated baby, oximeter and blender, wired to the guest's drivers."""

    def __init__(self, cot: int, rec: Recorder) -> None:
        family, seed = SCENARIOS.get(cot, ("moderate", cot))
        self.scn = draw(family, seed)
        self.cot = cot
        self.rec = rec
        self.patient = Patient(self.scn.patient, self.scn.rng("patient"), self.scn.initial_fio2)
        self.ox = Oximeter(self.scn.rng("oximeter"), self.scn.oximeter)
        self.blender = Blender(backup_fio2=self.scn.settings.backup_fio2, fio2=self.scn.initial_fio2)
        self.splitter = LineSplitter()
        port = PORT_BASE + 2 * (cot - 1)
        self.ox_link = Link(port, lambda d: None, lambda: rec.add(cot, "ox_connect"))
        self.bl_link = Link(port + 1, self._bl_data, lambda: rec.add(cot, "bl_connect"))
        self.last_state = "N"
        self.seconds: list[tuple[float, int | None, int, float]] = []  # (t, displayed SpO2, flags, FiO2)

    async def start(self) -> None:
        await self.ox_link.start()
        await self.bl_link.start()

    def _bl_data(self, data: bytes) -> None:
        for line in self.splitter.feed(data):
            try:
                f = parse_bl(line)
            except FrameError:
                self.rec.add(self.cot, "bl_bad", line=line[:40])
                continue
            now = self.rec.now()
            self.blender.on_frame(now, f)
            self.rec.add(self.cot, "bl", fio2=f.fio2_pm / 1000.0, alarm=f.alarm, seq=f.seq)
            self.bl_link.write(self.blender.status().encode())

    def tick(self) -> None:
        now = self.rec.now()
        sa = self.patient.step(self.blender.circuit)
        data = self.ox.transmit(sa, self.patient.pr)
        self.ox_link.write(data)
        for line in LineSplitter().feed(data):
            try:
                f = parse_ox(line)
                self.seconds.append((now, f.spo2, f.flags, self.blender.commanded))
            except FrameError:
                pass
        self.blender.step(now)
        if self.blender.state != self.last_state:
            self.rec.add(self.cot, "blender_state", state=self.blender.state, fio2=self.blender.fallback_fio2)
            self.last_state = self.blender.state
        self.bl_link.write(self.blender.status().encode())


class Guest:
    """Commands on the QNX guest over ssh."""

    def __init__(self, port: int = 2222, types: bool = False) -> None:
        self.port = port
        self.types = types

    async def run(self, cmd: str, timeout: float = 60.0) -> tuple[int, str]:
        args = [
            "ssh", "-p", str(self.port), "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
            "-o", "LogLevel=ERROR", "-o", "ConnectTimeout=20", "root@127.0.0.1", cmd,
        ]  # fmt: skip
        proc = await asyncio.create_subprocess_exec(
            *args, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT
        )
        try:
            out, _ = await asyncio.wait_for(proc.communicate(), timeout)
        except TimeoutError:
            proc.kill()
            return 124, ""
        return proc.returncode or 0, out.decode(errors="replace")

    def typed(self, type_name: str, cmd: str) -> str:
        return f"on -T {type_name} {cmd}" if self.types else cmd

    async def signal(self, prog: str, cot: int, sig: str) -> int:
        rc, _ = await self.run(f"kill -{sig} $(cat /dev/shmem/titra-{prog}-{cot}.pid)")
        return rc

    async def status(self, cot: int, timeout: float = 60.0) -> dict[str, Any]:
        rc, out = await self.run(f"cat /dev/titra/cot{cot}", timeout=timeout)
        try:
            data: dict[str, Any] = json.loads(out.strip().splitlines()[-1])
            return data
        except (ValueError, IndexError):
            return {}


def first(events: list[Event], pred: Callable[[Event], bool]) -> Event | None:
    return next((e for e in events if pred(e)), None)


def last_fio2_before(rec: Recorder, cot: int, t: float) -> float | None:
    frames = [e for e in rec.events if e.cot == cot and e.kind == "bl" and e.t < t]
    return float(frames[-1].data["fio2"]) if frames else None


async def run_case(case: dict[str, Any], cots: dict[int, CotTwin], rec: Recorder, guest: Guest) -> dict[str, Any]:
    cid, fault, cot = case["id"], case["fault"], int(case["cot"])
    twin = cots.get(cot)
    duration = float(case.get("duration_s", 0))
    observe = max(120.0, duration + 60.0)
    result: dict[str, Any] = {"id": cid, "fault": fault, "cot": cot, "expect": case["expect"]}
    watch = [int(c) for c in case.get("watch_cots", [])]
    before = {c: await guest.status(c) for c in (watch or ([cot] if cot else list(cots)))}

    t0 = rec.now()
    rec.add(cot, "fault_start", id=cid, fault=fault)
    fio2_before = last_fio2_before(rec, cot, t0) if cot else None
    polls: list[tuple[float, dict[str, Any]]] = []

    async def poll_status() -> None:
        # A stopped controller cannot answer its status path, so each read gets a short timeout.
        while rec.now() - t0 < observe:
            polls.append((rec.now(), await guest.status(cot, timeout=3.0)))
            await asyncio.sleep(0.5)

    poller = asyncio.ensure_future(poll_status()) if fault in ("kill_titractl", "stop_titractl") else None
    if fault in ("sensor_off", "cable_cut", "repeat_frame", "frozen_values", "garbage_line") and twin:
        twin.ox.force(fault)
    elif fault == "blender_link_cut" and twin:
        twin.bl_link.refusing = True
        twin.bl_link.drop()
    elif fault in ("kill_spo2d", "kill_valved", "kill_titractl"):
        await guest.signal(fault.split("_", 1)[1], cot, "KILL")
    elif fault == "stop_titractl":
        await guest.signal("titractl", cot, "STOP")
    elif fault == "bad_command":
        rc, out = await guest.run(guest.typed(f"titractl{cot}_t", f"/system/bin/titractl -n {cot} -X {case['attempts']}"))
        result["guest_output"] = out.strip()
    elif fault == "open_valve_from_other_type":
        rc, out = await guest.run(guest.typed("station_t", f"/system/bin/station -V {cot} -k {case['attempts']}"))
        result["guest_output"] = out.strip()
    elif fault == "cpu_load":
        cmd = f"/system/bin/titra-burn -n 4 -p 10 -t {int(duration)}"
        asyncio.ensure_future(guest.run(cmd, timeout=duration + 60))
    if duration and fault in ("sensor_off", "cable_cut", "repeat_frame", "frozen_values", "garbage_line"):
        await asyncio.sleep(duration)
        if twin:
            twin.ox.force(None)
    elif duration and fault == "blender_link_cut":
        if cot == 2 and twin:
            await asyncio.sleep(10.0)
            rc, out = await guest.run(f"cat /dev/valve/{cot}")
            result["driver_state_during_cut"] = out.strip()
        await asyncio.sleep(max(duration - 10.0, 0.0))
        if twin:
            twin.bl_link.refusing = False
    await asyncio.sleep(max(observe - (rec.now() - t0), 0.0))
    if poller is not None:
        await poller
    rec.add(cot, "fault_end", id=cid)
    after = {c: await guest.status(c) for c in before}

    ev = rec.since(t0, cot) if cot else []
    bl = [e for e in ev if e.kind == "bl"]
    alarm = first(bl, lambda e: e.data["alarm"] >= 2)
    high = first(bl, lambda e: e.data["alarm"] == 3)
    blender_hold = first(ev, lambda e: e.kind == "blender_state" and e.data["state"] == "H")
    blender_fb = first(ev, lambda e: e.kind == "blender_state" and e.data["state"] == "F")
    m: dict[str, Any] = {}
    alarm_t = min([e.t for e in (alarm, blender_hold) if e is not None], default=None)
    if alarm_t is not None:
        m["alarm_s"] = round(alarm_t - t0, 2)
    if fault in ("sensor_off", "cable_cut", "repeat_frame", "frozen_values") and high:
        m["fallback_s"] = round(high.t - t0, 2)
        m["fallback_fio2"] = high.data["fio2"]
        holding = [e.data["fio2"] for e in bl if e.t < high.t]
        m["fio2_before"] = fio2_before
        m["fio2_unchanged_while_holding"] = all(abs(f - (fio2_before or f)) < 1e-6 for f in holding)
    if fault == "blender_link_cut" and blender_fb:
        m["fallback_s"] = round(blender_fb.t - t0, 2)
        m["fallback_fio2"] = blender_fb.data["fio2"]
    if fault in ("garbage_line", "kill_spo2d"):
        m["fallback_seen"] = high is not None
    if fault == "kill_spo2d" and alarm_t is not None:
        resumed = first(bl, lambda e: e.t > alarm_t and e.data["alarm"] < 2)
        if resumed:
            m["resume_s"] = round(resumed.t - t0, 2)
    if fault in ("kill_titractl", "stop_titractl"):
        # Resumed when the status path answers from a new controller that is back in control.
        started0 = before.get(cot, {}).get("started")
        new = next(((t, st) for t, st in polls if st and st.get("started") != started0 and st.get("mode") == "control"),
                   None)  # fmt: skip
        if new is not None:
            m["resume_s"] = round(new[0] - t0, 2)
            m["fio2_before"] = fio2_before
            m["fio2_at_resume"] = new[1].get("fio2")
            m["step_at_resume"] = round(abs(float(new[1].get("fio2", 0.0)) - (fio2_before or 0.0)), 3)
            m["gap_s"] = m["resume_s"]
    if fault == "kill_valved":
        times = [e.t for e in rec.events if e.cot == cot and e.kind == "bl" and e.t >= t0 - 2.0]
        gaps = [(b - a, b) for a, b in zip(times, times[1:], strict=False) if b > t0]
        if gaps:
            gap, back = max(gaps)
            m["gap_s"] = round(gap, 2)
            m["resume_s"] = round(back - t0, 2)
    if fault in ("bad_command", "open_valve_from_other_type") and result.get("guest_output"):
        try:
            out = json.loads(result["guest_output"].splitlines()[-1])
            m["attempts"] = out.get("attempts")
            m["refused"] = out.get("refused")
            m["refused_fraction"] = (out.get("refused") or 0) / max(out.get("attempts") or 1, 1)
        except (ValueError, IndexError):
            pass
    if fault in ("cpu_load",) or watch:
        late = {
            c: int(after.get(c, {}).get("late_ticks", -1)) - int(before.get(c, {}).get("late_ticks", 0)) for c in before
        }
        m["late_ticks"] = late
        m["max_interval_s"] = {c: after.get(c, {}).get("max_interval") for c in before}
    result["measured"] = m
    result["pass"] = judge(case["expect"], m)
    return result


def judge(expect: dict[str, Any], m: dict[str, Any]) -> bool:
    ok = True
    for key, budget in expect.items():
        if key == "alarm_within_s":
            ok &= "alarm_s" in m and m["alarm_s"] <= budget
        elif key == "fallback_within_s":
            ok &= "fallback_s" in m and m["fallback_s"] <= budget
        elif key == "resume_within_s":
            ok &= "resume_s" in m and m["resume_s"] <= budget
        elif key == "fio2_unchanged_while_holding":
            ok &= bool(m.get("fio2_unchanged_while_holding"))
        elif key == "no_fallback":
            ok &= not m.get("fallback_seen", False)
        elif key == "no_step_at_resume":
            ok &= m.get("step_at_resume", 1.0) <= 0.011
        elif key == "refused_fraction":
            ok &= m.get("refused_fraction", 0.0) >= budget
        elif key == "alarm_if_gap_over_s":
            # An alarm is required only if the cot went longer than the hold threshold without its process.
            ok &= m.get("gap_s", budget + 1) <= budget or "alarm_s" in m
        elif key in ("missed_ticks", "missed_ticks_on_watched_cots"):
            ok &= all(v == 0 for v in m.get("late_ticks", {0: -1}).values())
        elif key == "no_crash":
            continue
    return ok


def closed_loop(cots: dict[int, CotTwin], start: float, end: float) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for n, twin in cots.items():
        rows = [(s, f) for t, s, fl, f in twin.seconds if start <= t <= end and s is not None and fl == 0]
        out[str(n)] = {
            "seconds_valid": len(rows),
            "time_in_target_pct": round(100.0 * sum(in_target(s, f) for s, f in rows) / max(len(rows), 1), 1),
            "scenario": f"{twin.scn.family}:{twin.scn.seed}",
        }
    return out


async def main_async(a: argparse.Namespace) -> None:
    rec = Recorder()
    cots = {n: CotTwin(n, rec) for n in range(1, a.cots + 1)}
    for twin in cots.values():
        await twin.start()
    guest = Guest(types=a.types)

    async def ticker() -> None:
        nxt = time.monotonic()
        while True:
            nxt += 1.0
            await asyncio.sleep(max(nxt - time.monotonic(), 0.0))
            for twin in cots.values():
                twin.tick()

    tick_task = asyncio.ensure_future(ticker())
    print(f"bench: listening on ports {PORT_BASE}-{PORT_BASE + 2 * a.cots - 1}, warm-up {a.warmup} s", flush=True)
    await asyncio.sleep(a.warmup)
    connected = {n: bool(rec.since(0, n, "bl")) for n in cots}
    print(f"bench: frames from the guest per cot after warm-up: {connected}", flush=True)
    warm_end = rec.now()
    matrix = json.loads(FAULTS.read_text(encoding="utf-8"))
    wanted = set(a.cases.split(",")) if a.cases else None
    results = []
    for case in matrix["cases"]:
        if wanted and case["id"] not in wanted:
            continue
        r = await run_case(case, cots, rec, guest)
        print(f"bench: {r['id']} {r['fault']}: {'pass' if r['pass'] else 'FAIL'} {r['measured']}", flush=True)
        results.append(r)
        await asyncio.sleep(a.gap)
    tick_task.cancel()
    summary = {
        "target": "QNX 8.0 in QEMU (software emulation), x86_64, 4 CPUs",
        "clock": "host monotonic clock, outside the guest",
        "types": a.types,
        "warmup_s": a.warmup,
        "closed_loop_during_warmup": closed_loop(cots, 30.0, warm_end),
        "closed_loop_whole_session_undisturbed_cots": {
            k: v for k, v in closed_loop(cots, 30.0, rec.now()).items() if k != "2"
        },
        "cases": results,
        "passed": sum(1 for r in results if r["pass"]),
        "total": len(results),
    }
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(summary, indent=1) + "\n", encoding="utf-8")
    events = ROOT / "data" / "runs" / (out.stem + "_events.jsonl")
    events.parent.mkdir(parents=True, exist_ok=True)
    with events.open("w", encoding="utf-8") as fh:
        for e in rec.events:
            fh.write(json.dumps({"t": round(e.t, 3), "cot": e.cot, "kind": e.kind, **e.data}) + "\n")
    print(f"bench: {summary['passed']} of {summary['total']} cases passed -> {out}", flush=True)


def main() -> None:
    ap = argparse.ArgumentParser(prog="python -m bench.live")
    ap.add_argument("--cots", type=int, default=3)
    ap.add_argument("--warmup", type=float, default=360.0)
    ap.add_argument("--gap", type=float, default=45.0, help="seconds of recovery between cases")
    ap.add_argument("--cases", help="comma-separated case ids, default all")
    ap.add_argument("--types", action="store_true", help="the guest runs under the security policy (on -T)")
    ap.add_argument("--out", default=str(ROOT / "eval" / "results" / "faults.json"))
    a = ap.parse_args()
    asyncio.run(main_async(a))


if __name__ == "__main__":
    main()

