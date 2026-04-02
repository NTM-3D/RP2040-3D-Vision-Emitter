#!/usr/bin/env python
"""Replay USB captures through a software model of the firmware timing path.

This script parses host->device packets from pcapng via tshark and simulates the
current scheduler behavior in src/emitter.c + src/ir_emitter.c:
- control-out writes that enable/disable driver mode
- swap-out packets (AA FE / AA FF)
- PLL scheduling
- frame alarm queueing
- deferred frame fire callback
- holdover generation

It produces:
- textual summary
- CSV of emitted frames (timestamp, eye, source)

Usage:
  python tools/replay_firmware.py --pcap max_20s_7_times.pcapng --discard-start 20 --discard-end 3
    python tools/replay_firmware.py --pcap max_20s_7_times.pcapng --mode dup-suppress
"""

from __future__ import annotations

import argparse
import csv
import dataclasses
import os
import re
import subprocess
import sys
from collections import Counter
from typing import List, Optional

# Firmware constants mirrored from code
FRAME_ALARM_DELAY_US = 3000
PLL_NOMINAL_PERIOD_US = 8333
PLL_GAIN_SHIFT = 3
PLL_FAST_GAIN_SHIFT = 1
PLL_FAST_FRAMES = 4
PLL_RESYNC_THRESHOLD = 2000
PLL_VALID_MIN_PERIOD_US = 7600
PLL_VALID_MAX_PERIOD_US = 9000

DRIVER_EXIT_HOLD_MS = 250
HOLDOVER_START_MS = 6
HOLDOVER_DEFAULT_PERIOD_US = 8333
HOLDOVER_MIN_PERIOD_US = 7600
HOLDOVER_MAX_PERIOD_US = 9000
HOLDOVER_SMOOTH_SHIFT = 2
ENABLE_HOLDOVER = False
STALE_REPEAT_NUM = 7
STALE_REPEAT_DEN = 5
SUPPRESS_FEFF_FOLLOWUP_US = 5000
MASTER_START_DELAY_US = 3000
MASTER_PHASE_GAIN_SHIFT = 3
MASTER_PHASE_CLAMP_US = 600
MASTER_SCHEDULE_EARLY_US = 300

# Approximate waveform active duration for busy-window modeling.
# 3D Vision token pair timing + small overhead.
WAVEFORM_US_TOKEN0 = 570
WAVEFORM_US_TOKEN2 = 650

TSHARK_CANDIDATES = [
    os.environ.get("TSHARK_PATH", "").strip(),
    r"C:\Program Files\Wireshark\tshark.exe",
    "tshark",
]


@dataclasses.dataclass
class UsbFrame:
    frame_no: int
    t_us: int
    endpoint: Optional[int]
    src: str
    dst: str
    transfer_type: str
    data_len: int
    capdata: bytes
    setup_bmrequesttype: Optional[int]
    setup_brequest: Optional[int]
    setup_wvalue: Optional[int]
    setup_wlength: Optional[int]


@dataclasses.dataclass
class PacketEvent:
    frame_no: int
    t_us: int
    endpoint: int
    capdata: bytes


@dataclasses.dataclass
class EmitEvent:
    t_us: int
    eye: int
    token: int
    source: str
    packet_index: Optional[int]


@dataclasses.dataclass
class PendingFrame:
    fire_time_us: int
    token: int
    source: str
    packet_index: Optional[int]


@dataclasses.dataclass
class AnomalyEvent:
    kind: str
    t_us: int
    gap_us: int
    prev_eye: int
    eye: int
    prev_source: str
    source: str
    packet_index: Optional[int]


@dataclasses.dataclass
class AnomalyBurst:
    start_us: int
    end_us: int
    count: int
    kind_counts: Counter[str]
    has_holdover: bool


class FirmwareSim:
    def __init__(self, mode: str, driver_enabled_at_start: bool) -> None:
        self.mode = mode

        # emitter.c state
        self.g_last_packet_ms = 0
        self.g_last_packet_us = 0
        self.g_driver_enabled = driver_enabled_at_start
        self.g_holdover_active = False
        self.g_next_holdover_us = 0
        self.g_holdover_eye = 0
        self.g_holdover_period_us = HOLDOVER_DEFAULT_PERIOD_US
        self.g_last_packet_had_feff = False
        self.g_master_active = False
        self.g_master_next_frame_us = 0
        self.g_master_next_eye = 0
        self.reg1b = 0

        # ir_emitter.c state
        self.g_swap_eyes = 0
        self.g_cur_eye = 0
        self.g_next_eye = 0

        self.g_frame_alarm_active = False
        self.pending_frame: Optional[PendingFrame] = None
        self.g_dma_busy_until_us = 0

        self.g_phase_us = 0
        self.g_pll_locked = False
        self.g_pll_frame_count = 0
        self.g_last_valid_pll_period_us = PLL_NOMINAL_PERIOD_US

        # time
        self.now_us = 0
        self.g_millis_passed = 0

        # output / stats
        self.emits: List[EmitEvent] = []
        self.swap_packets_seen = 0
        self.swap_packets_accepted = 0
        self.swap_packets_dup_suppressed = 0
        self.swap_packets_rejected_busy = 0
        self.pll_resync_count = 0

    def _set_time(self, new_us: int) -> None:
        if new_us < self.now_us:
            raise ValueError("time moved backwards")
        self.now_us = new_us
        self.g_millis_passed = self.now_us // 1000

    def _ir_emitter_is_busy(self) -> bool:
        return self.g_frame_alarm_active or (self.now_us < self.g_dma_busy_until_us)

    def _ir_emitter_set_eye(self, eye: int) -> None:
        self.g_next_eye = (eye ^ self.g_swap_eyes) & 1

    def _holdover_filter_period(self, current_us: int, new_us: int) -> int:
        if new_us < HOLDOVER_MIN_PERIOD_US or new_us > HOLDOVER_MAX_PERIOD_US:
            return current_us
        if current_us < HOLDOVER_MIN_PERIOD_US or current_us > HOLDOVER_MAX_PERIOD_US:
            return new_us
        error = new_us - current_us
        return int(current_us + (error >> HOLDOVER_SMOOTH_SHIFT))

    def _waveform_duration_us(self, token: int) -> int:
        return WAVEFORM_US_TOKEN0 if token in (0, 1) else WAVEFORM_US_TOKEN2

    def _do_fire_waveform(self, frame: PendingFrame) -> None:
        token = frame.token
        self.g_cur_eye = (token >> 1) & 1
        self.g_dma_busy_until_us = self.now_us + self._waveform_duration_us(token)
        self.emits.append(
            EmitEvent(
                t_us=self.now_us,
                eye=self.g_cur_eye,
                token=token,
                source=frame.source,
                packet_index=frame.packet_index,
            )
        )

        # In phase-master mode, keep the local cadence running even when no
        # host packets arrive by chaining the next master frame at fire time.
        if (
            self.mode == "phase-master"
            and frame.source == "master"
            and self.g_driver_enabled
            and self.g_master_active
            and self.g_holdover_period_us > 0
        ):
            self.g_master_next_frame_us += self.g_holdover_period_us
            self.g_master_next_eye ^= 1
            self._ir_emitter_set_eye(self.g_master_next_eye)
            self._ir_emitter_start_frame_at(self.g_master_next_frame_us, source="master", packet_index=None)

    def _fire_alarm_if_due(self, up_to_us: int) -> None:
        while self.g_frame_alarm_active and self.pending_frame and self.pending_frame.fire_time_us <= up_to_us:
            fire_time = self.pending_frame.fire_time_us
            frame = self.pending_frame
            self._set_time(fire_time)
            self.g_frame_alarm_active = False
            self.pending_frame = None
            self._do_fire_waveform(frame)

    def advance_to(self, target_us: int) -> None:
        self._fire_alarm_if_due(target_us)
        self._set_time(target_us)

    def _schedule_frame_for_target(self, token: int, target_us: int, source: str, packet_index: Optional[int]) -> bool:
        delay = target_us - self.now_us
        if delay < 50:
            delay = 50

        if self.g_frame_alarm_active:
            self.swap_packets_rejected_busy += 1
            return False

        self.pending_frame = PendingFrame(
            fire_time_us=self.now_us + delay,
            token=token,
            source=source,
            packet_index=packet_index,
        )
        self.g_frame_alarm_active = True
        return True

    def _ir_emitter_start_frame(self, source: str, packet_index: Optional[int]) -> bool:
        token = self.g_next_eye * 2

        now = self.now_us

        if not self.g_pll_locked:
            target = now + FRAME_ALARM_DELAY_US
            self.g_phase_us = target
            self.g_pll_locked = True
            self.g_pll_frame_count = 0
        else:
            prev_phase = self.g_phase_us
            target = self.g_phase_us + PLL_NOMINAL_PERIOD_US
            ideal = now + FRAME_ALARM_DELAY_US
            error = ideal - target

            if error > PLL_RESYNC_THRESHOLD or error < -PLL_RESYNC_THRESHOLD:
                target = ideal
                self.g_pll_frame_count = 0
                self.pll_resync_count += 1
            else:
                shift = PLL_FAST_GAIN_SHIFT if self.g_pll_frame_count < PLL_FAST_FRAMES else PLL_GAIN_SHIFT
                target += error >> shift
                period = target - prev_phase
                if PLL_VALID_MIN_PERIOD_US <= period <= PLL_VALID_MAX_PERIOD_US:
                    self.g_last_valid_pll_period_us = int(period)

            self.g_phase_us = target
            if self.g_pll_frame_count < PLL_FAST_FRAMES:
                self.g_pll_frame_count += 1

        return self._schedule_frame_for_target(token, int(target), source, packet_index)

    def _ir_emitter_start_frame_at(self, target_us: int, source: str, packet_index: Optional[int]) -> bool:
        token = self.g_next_eye * 2
        return self._schedule_frame_for_target(token, target_us, source, packet_index)

    def emitter_task(self) -> None:
        cur_time_ms = self.g_millis_passed

        if self.g_driver_enabled and self.g_last_packet_ms != 0:
            since_last_packet_ms = cur_time_ms - self.g_last_packet_ms

            if since_last_packet_ms > DRIVER_EXIT_HOLD_MS:
                self.g_last_packet_ms = 0
                self.g_last_packet_us = 0
                self.g_holdover_active = False
                self.g_next_holdover_us = 0
                self.g_master_active = False
                self.g_master_next_frame_us = 0
                self.g_pll_locked = False
                self.g_frame_alarm_active = False
                self.pending_frame = None
            elif ENABLE_HOLDOVER and since_last_packet_ms >= HOLDOVER_START_MS:
                now_us = self.now_us
                if not self.g_holdover_active:
                    self.g_holdover_active = True
                    self.g_next_holdover_us = self.g_last_packet_us + self.g_holdover_period_us

                if now_us >= self.g_next_holdover_us and self.g_holdover_period_us > 0:
                    behind_us = now_us - self.g_next_holdover_us
                    slots = (behind_us // self.g_holdover_period_us) + 1
                    next_eye = self.g_holdover_eye ^ (slots & 1)
                    self._ir_emitter_set_eye(next_eye)
                    if self._ir_emitter_start_frame_at(self.g_next_holdover_us, source="holdover", packet_index=None):
                        self.g_next_holdover_us += slots * self.g_holdover_period_us
                        self.g_holdover_eye = next_eye

        if self.g_driver_enabled and self.g_master_active and self.g_holdover_period_us > 0:
            now_us = self.now_us
            if (not self.g_frame_alarm_active) and ((now_us + MASTER_SCHEDULE_EARLY_US) >= self.g_master_next_frame_us):
                self._ir_emitter_set_eye(self.g_master_next_eye)
                self._ir_emitter_start_frame_at(self.g_master_next_frame_us, source="master", packet_index=None)

    def handle_control_out(self, payload: bytes) -> None:
        if len(payload) < 3:
            return

        command = payload[0]
        offset = payload[1]
        amount = payload[2]

        if command & 0x01:
            if offset == 0x1B and amount >= 1 and len(payload) >= 5:
                was_enabled = self.g_driver_enabled
                self.reg1b = payload[4]
                self.g_driver_enabled = (self.reg1b & 0x04) != 0
                if not self.g_driver_enabled:
                    self.g_last_packet_ms = 0
                    self.g_last_packet_us = 0
                    self.g_holdover_active = False
                    self.g_next_holdover_us = 0
                    self.g_master_active = False
                    self.g_master_next_frame_us = 0
                    self.g_pll_locked = False
                    self.g_frame_alarm_active = False
                    self.pending_frame = None
                elif not was_enabled:
                    self.g_last_packet_ms = 0
                    self.g_last_packet_us = 0
                    self.g_holdover_active = False
                    self.g_next_holdover_us = 0
                    self.g_master_active = False
                    self.g_master_next_frame_us = 0

    def handle_swap_out(self, payload: bytes, packet_index: int) -> None:
        self.swap_packets_seen += 1

        if len(payload) != 8:
            return
        if not self.g_driver_enabled:
            return
        if payload[0] != 0xAA or (payload[1] & 0xFE) != 0xFE:
            return

        new_eye = payload[1] & 0x01
        packet_has_feff = payload[6] == 0xFE and payload[7] == 0xFF

        if (
            self.g_last_packet_us != 0
            and new_eye == self.g_holdover_eye
            and (self.now_us - self.g_last_packet_us)
            > ((self.g_holdover_period_us * STALE_REPEAT_NUM) // STALE_REPEAT_DEN)
        ):
            self.swap_packets_dup_suppressed += 1
            return
        if (
            self.g_last_packet_us != 0
            and not packet_has_feff
            and self.g_last_packet_had_feff
            and new_eye == self.g_holdover_eye
            and (self.now_us - self.g_last_packet_us) <= SUPPRESS_FEFF_FOLLOWUP_US
        ):
            self.swap_packets_dup_suppressed += 1
            return

        self.swap_packets_accepted += 1

        self.g_last_packet_ms = self.g_millis_passed
        self.g_last_packet_us = self.now_us

        pll_period_us = self.g_last_valid_pll_period_us
        if pll_period_us == 0:
            pll_period_us = HOLDOVER_DEFAULT_PERIOD_US
        self.g_holdover_period_us = self._holdover_filter_period(self.g_holdover_period_us, pll_period_us)

        self.g_holdover_active = False
        self.g_next_holdover_us = 0
        self.g_holdover_eye = new_eye
        self.g_last_packet_had_feff = packet_has_feff

        if self.mode == "phase-master":
            target = self.now_us + MASTER_START_DELAY_US
            if not self.g_master_active:
                self.g_master_active = True
                self.g_master_next_eye = new_eye
                self.g_master_next_frame_us = target
                self._ir_emitter_set_eye(self.g_master_next_eye)
                self._ir_emitter_start_frame_at(self.g_master_next_frame_us, source="master", packet_index=None)
            else:
                error = target - self.g_master_next_frame_us
                if error > MASTER_PHASE_CLAMP_US:
                    error = MASTER_PHASE_CLAMP_US
                elif error < -MASTER_PHASE_CLAMP_US:
                    error = -MASTER_PHASE_CLAMP_US
                self.g_master_next_frame_us += error >> MASTER_PHASE_GAIN_SHIFT
                if new_eye != self.g_master_next_eye:
                    self.g_master_next_eye = new_eye
                    self.g_master_next_frame_us = target
                if not self.g_frame_alarm_active:
                    self._ir_emitter_set_eye(self.g_master_next_eye)
                    self._ir_emitter_start_frame_at(self.g_master_next_frame_us, source="master", packet_index=None)
            return

        if self.mode == "dup-suppress":
            if new_eye == self.g_cur_eye and not self._ir_emitter_is_busy():
                self.swap_packets_dup_suppressed += 1
                return

        self._ir_emitter_set_eye(new_eye)
        self._ir_emitter_start_frame(source="swap", packet_index=packet_index)


def find_tshark() -> str:
    for cand in TSHARK_CANDIDATES:
        if not cand:
            continue
        try:
            r = subprocess.run([cand, "-v"], capture_output=True, text=True, timeout=5)
            if r.returncode == 0 or "TShark" in (r.stdout + r.stderr):
                return cand
        except Exception:
            continue
    raise RuntimeError("tshark not found. Set TSHARK_PATH or install Wireshark/tshark.")


def _parse_int_field(value: str) -> Optional[int]:
    value = value.strip()
    if not value:
        return None
    try:
        return int(value, 0)
    except ValueError:
        return None


def parse_pcap_frames(pcap_path: str, tshark: str) -> List[UsbFrame]:
    cmd = [
        tshark,
        "-r",
        pcap_path,
        "-T",
        "fields",
        "-e",
        "frame.number",
        "-e",
        "frame.time_relative",
        "-e",
        "usb.endpoint_address",
        "-e",
        "usb.src",
        "-e",
        "usb.dst",
        "-e",
        "usb.transfer_type",
        "-e",
        "usb.data_len",
        "-e",
        "usb.capdata",
        "-e",
        "usb.setup.bRequest",
        "-e",
        "usb.setup.wValue",
        "-e",
        "usb.setup.wLength",
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"tshark failed: {r.stderr.strip()}")

    frames: List[UsbFrame] = []
    for raw in r.stdout.splitlines():
        parts = raw.split("\t")
        if len(parts) < 10:
            parts.extend([""] * (10 - len(parts)))
        frame_s, ts_s, ep_s, src_s, dst_s, transfer_s, data_len_s, data_s, breq_s, wvalue_s, *rest = parts
        wlength_s = rest[0] if rest else ""
        frame_s = frame_s.strip()
        ts_s = ts_s.strip()
        if not frame_s or not ts_s:
            continue

        ep = None
        ep_s = ep_s.strip()
        if ep_s.lower().startswith("0x"):
            ep = int(ep_s, 16)

        data_s = data_s.strip()
        payload = b""
        if data_s and re.fullmatch(r"[0-9a-fA-F]+", data_s) and (len(data_s) % 2 == 0):
            payload = bytes.fromhex(data_s)

        frames.append(
            UsbFrame(
                frame_no=int(frame_s),
                t_us=int(round(float(ts_s) * 1_000_000.0)),
                endpoint=ep,
                src=src_s.strip().lower(),
                dst=dst_s.strip().lower(),
                transfer_type=transfer_s.strip().lower(),
                data_len=_parse_int_field(data_len_s) or 0,
                capdata=payload,
                setup_bmrequesttype=None,
                setup_brequest=_parse_int_field(breq_s),
                setup_wvalue=_parse_int_field(wvalue_s),
                setup_wlength=_parse_int_field(wlength_s),
            )
        )

    frames.sort(key=lambda frame: (frame.t_us, frame.frame_no))
    return frames


def select_replay_events(frames: List[UsbFrame]) -> List[PacketEvent]:
    events: List[PacketEvent] = []
    for frame in frames:
        if frame.src != "host":
            continue
        if frame.endpoint not in (0x01, 0x02):
            continue
        if not frame.capdata:
            continue
        events.append(PacketEvent(frame_no=frame.frame_no, t_us=frame.t_us, endpoint=frame.endpoint, capdata=frame.capdata))
    return events


def summarize_usb_frames(frames: List[UsbFrame]) -> dict:
    endpoint_counts: Counter[str] = Counter()
    control_requests: Counter[str] = Counter()
    payload_frames = 0
    for frame in frames:
        ep_label = f"0x{frame.endpoint:02X}" if frame.endpoint is not None else "none"
        direction = f"{frame.src or '?'}->{frame.dst or '?'}"
        transfer = frame.transfer_type or "unknown"
        endpoint_counts[f"{direction} {transfer} {ep_label}"] += 1
        if frame.capdata:
            payload_frames += 1
        if frame.setup_brequest is not None:
            control_requests[
                f"bm=0x{frame.setup_bmrequesttype or 0:02X} bReq=0x{frame.setup_brequest:02X} "
                f"wValue=0x{frame.setup_wvalue or 0:04X} wLen={frame.setup_wlength or 0}"
            ] += 1

    return {
        "total_frames": len(frames),
        "payload_frames": payload_frames,
        "endpoint_counts": endpoint_counts,
        "control_requests": control_requests,
    }


def summarize_emits(emits: List[EmitEvent]) -> dict:
    if len(emits) < 2:
        return {
            "count": len(emits),
            "same_eye_pairs": 0,
            "max_gap_us": 0,
            "min_gap_us": 0,
            "gaps_over_10ms": 0,
            "gaps_over_15ms": 0,
        }

    same_eye = 0
    gaps = []
    for i in range(len(emits) - 1):
        if emits[i].eye == emits[i + 1].eye:
            same_eye += 1
        gaps.append(emits[i + 1].t_us - emits[i].t_us)

    return {
        "count": len(emits),
        "same_eye_pairs": same_eye,
        "max_gap_us": max(gaps),
        "min_gap_us": min(gaps),
        "gaps_over_10ms": sum(1 for g in gaps if g > 10_000),
        "gaps_over_15ms": sum(1 for g in gaps if g > 15_000),
    }


def find_anomalies(emits: List[EmitEvent], start_us: int, end_us: int, gap_threshold_us: int) -> List[AnomalyEvent]:
    anomalies: List[AnomalyEvent] = []
    for i in range(1, len(emits)):
        prev_emit = emits[i - 1]
        emit = emits[i]
        if emit.t_us < start_us or emit.t_us > end_us:
            continue
        gap_us = emit.t_us - prev_emit.t_us
        if prev_emit.eye == emit.eye:
            anomalies.append(
                AnomalyEvent(
                    kind="same-eye",
                    t_us=emit.t_us,
                    gap_us=gap_us,
                    prev_eye=prev_emit.eye,
                    eye=emit.eye,
                    prev_source=prev_emit.source,
                    source=emit.source,
                    packet_index=emit.packet_index,
                )
            )
        if gap_us > gap_threshold_us:
            anomalies.append(
                AnomalyEvent(
                    kind=f"gap>{gap_threshold_us/1000.0:.1f}ms",
                    t_us=emit.t_us,
                    gap_us=gap_us,
                    prev_eye=prev_emit.eye,
                    eye=emit.eye,
                    prev_source=prev_emit.source,
                    source=emit.source,
                    packet_index=emit.packet_index,
                )
            )
    return anomalies


def cluster_anomalies(anomalies: List[AnomalyEvent], cluster_gap_us: int) -> List[AnomalyBurst]:
    if not anomalies:
        return []

    bursts: List[AnomalyBurst] = []
    start_us = anomalies[0].t_us
    end_us = anomalies[0].t_us
    kind_counts: Counter[str] = Counter([anomalies[0].kind])
    has_holdover = anomalies[0].prev_source == "holdover" or anomalies[0].source == "holdover"
    count = 1

    for event in anomalies[1:]:
        if event.t_us - end_us > cluster_gap_us:
            bursts.append(
                AnomalyBurst(
                    start_us=start_us,
                    end_us=end_us,
                    count=count,
                    kind_counts=kind_counts,
                    has_holdover=has_holdover,
                )
            )
            start_us = event.t_us
            end_us = event.t_us
            kind_counts = Counter([event.kind])
            has_holdover = event.prev_source == "holdover" or event.source == "holdover"
            count = 1
            continue

        end_us = event.t_us
        kind_counts[event.kind] += 1
        has_holdover = has_holdover or event.prev_source == "holdover" or event.source == "holdover"
        count += 1

    bursts.append(
        AnomalyBurst(
            start_us=start_us,
            end_us=end_us,
            count=count,
            kind_counts=kind_counts,
            has_holdover=has_holdover,
        )
    )
    return bursts


def write_csv(path: str, emits: List[EmitEvent]) -> None:
    with open(path, "w", newline="", encoding="ascii") as f:
        w = csv.writer(f)
        w.writerow(["index", "time_us", "time_s", "eye", "token", "source", "packet_index"])
        for i, e in enumerate(emits):
            w.writerow([i, e.t_us, f"{e.t_us / 1_000_000.0:.6f}", e.eye, e.token, e.source, e.packet_index if e.packet_index is not None else ""])


def write_anomaly_csv(path: str, anomalies: List[AnomalyEvent]) -> None:
    with open(path, "w", newline="", encoding="ascii") as f:
        w = csv.writer(f)
        w.writerow(["index", "kind", "time_us", "time_s", "gap_us", "prev_eye", "eye", "prev_source", "source", "packet_index"])
        for i, event in enumerate(anomalies):
            w.writerow([
                i,
                event.kind,
                event.t_us,
                f"{event.t_us / 1_000_000.0:.6f}",
                event.gap_us,
                event.prev_eye,
                event.eye,
                event.prev_source,
                event.source,
                event.packet_index if event.packet_index is not None else "",
            ])


def write_burst_csv(path: str, bursts: List[AnomalyBurst]) -> None:
    with open(path, "w", newline="", encoding="ascii") as f:
        w = csv.writer(f)
        w.writerow(["index", "start_us", "start_s", "end_us", "end_s", "duration_ms", "count", "has_holdover", "kind_counts"])
        for i, burst in enumerate(bursts):
            kind_counts = ", ".join(
                f"{kind}:{count}" for kind, count in burst.kind_counts.most_common()
            )
            w.writerow([
                i,
                burst.start_us,
                f"{burst.start_us / 1_000_000.0:.6f}",
                burst.end_us,
                f"{burst.end_us / 1_000_000.0:.6f}",
                f"{(burst.end_us - burst.start_us) / 1000.0:.3f}",
                burst.count,
                int(burst.has_holdover),
                kind_counts,
            ])


def run_one(
    pcap_path: str,
    discard_start_s: float,
    discard_end_s: float,
    mode: str,
    driver_enabled_at_start: bool,
    inspect_start_s: Optional[float],
    inspect_end_s: Optional[float],
    gap_threshold_us: int,
    cluster_gap_us: int,
    out_prefix: str,
) -> None:
    tshark = find_tshark()
    frames = parse_pcap_frames(pcap_path, tshark)
    usb_summary = summarize_usb_frames(frames)
    events = select_replay_events(frames)
    if not events:
        raise RuntimeError("no endpoint 0x01/0x02 host events found in pcap")

    end_us = events[-1].t_us - int(discard_end_s * 1_000_000.0)
    start_us = int(discard_start_s * 1_000_000.0)
    if end_us <= start_us:
        raise RuntimeError("invalid discard window")

    events = [e for e in events if start_us <= e.t_us <= end_us]
    if not events:
        raise RuntimeError("no events remain after discard window")

    sim = FirmwareSim(mode=mode, driver_enabled_at_start=driver_enabled_at_start)

    packet_idx = 0
    for e in events:
        sim.advance_to(e.t_us)

        if e.endpoint == 0x02:
            sim.handle_control_out(e.capdata)
        elif e.endpoint == 0x01:
            sim.handle_swap_out(e.capdata, packet_index=packet_idx)
            packet_idx += 1

        # Main loop calls emitter_task after tud_task packet handling
        sim.emitter_task()

    # Flush any final alarm pending close to stream end
    sim.advance_to(end_us + 50_000)
    sim.emitter_task()

    summary = summarize_emits(sim.emits)
    csv_path = f"{out_prefix}_{mode}_emits.csv"
    write_csv(csv_path, sim.emits)

    anomalies: List[AnomalyEvent] = []
    anomaly_csv_path = ""
    bursts: List[AnomalyBurst] = []
    burst_csv_path = ""
    if inspect_start_s is not None and inspect_end_s is not None:
        anomalies = find_anomalies(
            sim.emits,
            start_us=int(inspect_start_s * 1_000_000.0),
            end_us=int(inspect_end_s * 1_000_000.0),
            gap_threshold_us=gap_threshold_us,
        )
        anomaly_csv_path = f"{out_prefix}_{mode}_anomalies.csv"
        write_anomaly_csv(anomaly_csv_path, anomalies)
        bursts = cluster_anomalies(anomalies, cluster_gap_us=cluster_gap_us)
        burst_csv_path = f"{out_prefix}_{mode}_bursts.csv"
        write_burst_csv(burst_csv_path, bursts)

    print(f"pcap: {pcap_path}")
    print(f"usb frames total: {usb_summary['total_frames']}")
    print(f"usb frames with payload: {usb_summary['payload_frames']}")
    for label, count in usb_summary["endpoint_counts"].most_common():
        print(f"usb traffic: {label} -> {count}")
    if usb_summary["control_requests"]:
        for label, count in usb_summary["control_requests"].most_common():
            print(f"usb setup: {label} -> {count}")
    print(f"window: {start_us/1e6:.3f}s .. {end_us/1e6:.3f}s")
    print(f"mode: {mode}")
    print(f"driver_enabled_at_start: {driver_enabled_at_start}")
    print(f"events parsed: {len(events)}")
    print(f"swap packets seen: {sim.swap_packets_seen}")
    print(f"swap packets accepted: {sim.swap_packets_accepted}")
    print(f"dup suppressed: {sim.swap_packets_dup_suppressed}")
    print(f"busy-rejected start_frame calls: {sim.swap_packets_rejected_busy}")
    print(f"pll hard resync count: {sim.pll_resync_count}")
    print(f"emitted frames: {summary['count']}")
    print(f"emitted same-eye pairs: {summary['same_eye_pairs']}")
    print(f"emitted min gap: {summary['min_gap_us']/1000.0:.3f} ms")
    print(f"emitted max gap: {summary['max_gap_us']/1000.0:.3f} ms")
    print(f"emitted gaps >10ms: {summary['gaps_over_10ms']}")
    print(f"emitted gaps >15ms: {summary['gaps_over_15ms']}")
    print(f"csv: {csv_path}")
    if inspect_start_s is not None and inspect_end_s is not None:
        print(f"inspect window: {inspect_start_s:.3f}s .. {inspect_end_s:.3f}s")
        print(f"candidate anomalies: {len(anomalies)}")
        print(f"candidate bursts: {len(bursts)}")
        if anomalies:
            kind_counts = Counter(event.kind for event in anomalies)
            for kind, count in kind_counts.most_common():
                print(f"candidate {kind}: {count}")
            print("candidate examples:")
            for event in anomalies[:12]:
                print(
                    f"  {event.kind} at {event.t_us / 1_000_000.0:.6f}s "
                    f"gap={event.gap_us / 1000.0:.3f}ms eyes {event.prev_eye}->{event.eye} "
                    f"sources {event.prev_source}->{event.source} packet_index={event.packet_index}"
                )
        print(f"anomaly csv: {anomaly_csv_path}")
        if bursts:
            print("candidate bursts:")
            for burst in bursts:
                kinds = ", ".join(
                    f"{kind}:{count}" for kind, count in burst.kind_counts.most_common()
                )
                print(
                    f"  {burst.start_us / 1_000_000.0:.6f}s .. {burst.end_us / 1_000_000.0:.6f}s "
                    f"count={burst.count} holdover={int(burst.has_holdover)} kinds={kinds}"
                )
        print(f"burst csv: {burst_csv_path}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pcap", required=True)
    ap.add_argument("--discard-start", type=float, default=0.0)
    ap.add_argument("--discard-end", type=float, default=0.0)
    ap.add_argument("--mode", choices=["current", "dup-suppress", "phase-master"], default="current")
    ap.add_argument("--driver-enabled-at-start", action="store_true")
    ap.add_argument("--inspect-start", type=float)
    ap.add_argument("--inspect-end", type=float)
    ap.add_argument("--gap-threshold-ms", type=float, default=15.0)
    ap.add_argument("--cluster-gap-ms", type=float, default=3000.0)
    ap.add_argument("--out-prefix", default="replay")
    args = ap.parse_args()

    run_one(
        pcap_path=args.pcap,
        discard_start_s=args.discard_start,
        discard_end_s=args.discard_end,
        mode=args.mode,
        driver_enabled_at_start=args.driver_enabled_at_start,
        inspect_start_s=args.inspect_start,
        inspect_end_s=args.inspect_end,
        gap_threshold_us=int(args.gap_threshold_ms * 1000.0),
        cluster_gap_us=int(args.cluster_gap_ms * 1000.0),
        out_prefix=args.out_prefix,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
