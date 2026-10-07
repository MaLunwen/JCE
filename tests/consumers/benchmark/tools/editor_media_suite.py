#!/usr/bin/env python3
"""Functional real editor video-viewer controls; ambient, never formal timing."""
import argparse
import json
import math
from pathlib import Path
import re
from gbuffer_asset_suite import sha256_file, write
from check_asset_pixels import read_png
from tools.benchmark_suite import source_identity, read_frames
from tools.jce_determinism import assert_backend
from tools.perf_bench import run

TRACE = re.compile(r"viewer trace wall=([\d.]+) media=([\d.]+) pts=(-?[\d.]+) audio=(-?[\d.]+) playing=([01]) dec=(\d+) disp=(\d+) drop=(\d+) q=(\d+) decode_ms=([\d.]+) convert_ms=([\d.]+) pop_ms=([\d.]+) upload_ms=([\d.]+) gaps25=(\d+) gaps50=(\d+) max_gap_ms=(\d+)")
REQUEST = re.compile(r"seek requested target=([\d.]+) from=([\d.]+)")
WAIT = re.compile(r"seek waiting target=([\d.]+) media=([\d.]+) pts=(-?[\d.]+) audio=(-?[\d.]+) voice=([01])")
READY = re.compile(r"seek ready target=([\d.]+) media=([\d.]+) pts=(-?[\d.]+) audio=(-?[\d.]+) voice=([01]) wait_ms=(\d+)")



def trace_metadata(log):
    records = []
    suffix = re.compile(r"upload_width=(\d+) upload_height=(\d+) preview_limit=(\d+)(?: buffering=([01]) stall_ms=(\d+)(?: requested_playing=([01]) effective_transport_active=([01]) voice_playing=(-1|0|1))?)?")
    for line in log.splitlines():
        if "viewer trace " not in line: continue
        match = TRACE.search(line)
        if not match: raise ValueError("malformed viewer trace")
        tail = line[match.end():].split(" at ", 1)[0].strip()
        if not tail:
            records.append({"schema": "legacy-clock-only", "upload_width": None,
                "upload_height": None, "preview_limit": None, "buffering": None, "stall_ms": None,
                "requested_playing": None, "effective_transport_active": None, "voice_playing": None})
            continue
        extra = suffix.fullmatch(tail)
        if not extra: raise ValueError("unknown viewer trace extension")
        records.append({"schema": "actual-transport" if extra[6] is not None else ("upload-and-buffer" if extra[4] is not None else "upload-dimensions"),
            **{key: int(value) if value is not None else None for key,value in zip(
                ("upload_width", "upload_height", "preview_limit", "buffering", "stall_ms",
                 "requested_playing", "effective_transport_active", "voice_playing"), extra.groups())}})
    return records


def progress_segments(log):
    segments, current = [], []
    def finish():
        if len(current) < 2: return
        first, last = current[0], current[-1]
        elapsed = last[0]-first[0]
        segments.append({"wall_interval_s": elapsed, "media_progress_s": last[1]-first[1],
            "picture_progress_s": last[2]-first[2], "audio_progress_s": last[3]-first[3],
            "effective_media_speed": (last[1]-first[1])/elapsed if elapsed>0 else None,
            "trace_samples": len(current)})
    for line in log.splitlines():
        if REQUEST.search(line):
            finish(); current = []
        match = TRACE.search(line)
        if match: current.append(tuple(float(v) for v in match.groups()))
    finish()
    return segments




BUFFER = re.compile(r"buffer event=(enter|hold|pre_resume|resume|cancel) id=(\d+) epoch=(\d+) wall_ms=(\d+) elapsed_ms=(\d+) media=([\d.]+) pts=(-?[\d.]+) audio=(-?[\d.]+) voice_playing=(-1|0|1)(?: quiescent=(-1|0|1))?")


def buffer_lifecycle(log):
    episodes, active, seek_epoch, previous_id = [], None, 0, 0
    for line in log.splitlines():
        if REQUEST.search(line): seek_epoch += 1
        event = BUFFER.search(line)
        if "buffer event=" in line and not event: raise ValueError("malformed buffer lifecycle")
        if not event:
            trace = TRACE.search(line)
            if trace and active and trace_metadata(line)[0]["effective_transport_active"] == 1:
                raise ValueError("effective transport before buffer resume")
            continue
        kind, identifier, epoch, wall, elapsed, media, pts, audio, voice, quiescent = event.groups()
        row = {"event":kind, "id":int(identifier), "epoch":int(epoch), "wall_ms":int(wall),
            "elapsed_ms":int(elapsed), "media":float(media), "pts":float(pts), "audio":float(audio), "voice_playing":int(voice), "quiescent":int(quiescent) if quiescent is not None else None}
        if not all(math.isfinite(row[k]) for k in ("media","pts","audio")):
            raise ValueError("nonfinite buffer cursor")
        if kind == "enter":
            if active or row["id"] != previous_id+1 or row["epoch"] != seek_epoch or row["voice_playing"] != 0:
                raise ValueError("buffer enter identity/actual pause mismatch")
            active = {"id":row["id"], "epoch":row["epoch"], "events":[row]}
            previous_id = row["id"]
        else:
            if not active or (row["id"],row["epoch"]) != (active["id"],active["epoch"]):
                raise ValueError("buffer event without matching enter")
            prior = active["events"][-1]
            if row["wall_ms"] < prior["wall_ms"] or row["elapsed_ms"] < prior["elapsed_ms"]:
                raise ValueError("buffer clock moved backwards")
            if abs((row["wall_ms"]-active["events"][0]["wall_ms"])-(row["elapsed_ms"]-active["events"][0]["elapsed_ms"]))>1:
                raise ValueError("buffer elapsed clock mismatch")
            if kind in ("hold", "pre_resume") and row["voice_playing"] != 0:
                raise ValueError("actual PCM consuming during buffer hold")
            if kind == "pre_resume" and prior["event"] not in ("enter", "hold"):
                raise ValueError("buffer pre-resume order mismatch")
            if kind == "resume" and (row["voice_playing"] != 1 or row["epoch"] != seek_epoch
                    or prior["event"] != "pre_resume"):
                raise ValueError("buffer resume actual voice/seek/pre-effect mismatch")
            active["events"].append(row)
            if kind in ("resume","cancel"):
                active["terminal"] = kind; episodes.append(active); active = None
    if active:
        active["terminal"] = "UNOBSERVED_AT_EXIT"; episodes.append(active)
    for episode in episodes:
        if any(r["quiescent"] is not None for r in episode["events"]):
            paused_events = [r for r in episode["events"] if r["event"]!="resume"]
            if any(r["quiescent"] is None for r in episode["events"]):
                raise ValueError("mixed quiescence lifecycle schema")
            acknowledged = next((i for i,r in enumerate(paused_events) if r["quiescent"]==1),None)
            suffix = paused_events[acknowledged:] if acknowledged is not None else []
            complete = episode["terminal"] in ("resume","cancel")
            if episode["terminal"]=="resume" and paused_events[-1]["quiescent"]!=1:
                raise ValueError("resume without actual stream quiescence acknowledgement")
            if suffix and any(r["quiescent"]!=1 or r["voice_playing"]!=0 for r in suffix):
                raise ValueError("stream acknowledgement lost while paused")
            if suffix and max(r["audio"] for r in suffix)-min(r["audio"] for r in suffix)>.000002:
                raise ValueError("consumed PCM cursor moved after actual quiescence acknowledgement")
            episode["stability"] = "ACKNOWLEDGED_STREAM_QUIESCENCE_OBSERVED" if suffix and complete else "INCONCLUSIVE_QUIESCENCE_UNAVAILABLE"
            episode["initial_inflight_cursor_progress_s"] = ((suffix[0]["audio"]-paused_events[0]["audio"]) if suffix else None)
            episode["quiescence_acknowledged"] = bool(suffix)
            episode["scope"] = "actual public admission/callback acknowledgement; cursor frozen only after acknowledgement, no guessed settling"
            continue
        holds = [r for r in episode["events"] if r["event"]=="hold"]
        stable = len(holds)>=2 and holds[-1]["wall_ms"]>holds[-2]["wall_ms"] and abs(holds[-1]["audio"]-holds[-2]["audio"])<=.002
        pre = [r for r in episode["events"] if r["event"]=="pre_resume"]
        paused = [r for r in episode["events"] if r["event"] in ("enter","hold","pre_resume","cancel")]
        short_proof = ((episode["terminal"]=="resume" and len(pre)==1
            or episode["terminal"]=="cancel") and bool(holds)
            and all(r["voice_playing"]==0 for r in paused)
            and max(r["audio"] for r in paused)-min(r["audio"] for r in paused)<=.000002)
        if any(b["audio"]+.000002<a["audio"] for a,b in zip(paused,paused[1:])):
            raise ValueError("same-epoch consumed PCM cursor moved backward")
        episode["stability"] = (("SHORT_CANCEL_CURSOR_STABLE_OBSERVED" if episode["terminal"]=="cancel"
             else "SHORT_CURSOR_STABLE_OBSERVED") if short_proof else
            "SAME_EPISODE_PCM_STABLE_OBSERVED" if stable else "INCONCLUSIVE_SHORT_OR_UNSETTLED")
        episode["short_cursor_proof"] = short_proof
        episode["cursor_log_quantization_s"] = .000002
        episode["scope"] = "same-epoch monotonic consumed cursor with actual paused voice; pre_resume read before resume effect or cancel before destruction, no settling allowance"
    return {"episodes":episodes, "status":"LIFECYCLE_OBSERVED" if episodes and all(
        e["stability"] in ("SAME_EPISODE_PCM_STABLE_OBSERVED", "SHORT_CURSOR_STABLE_OBSERVED", "SHORT_CANCEL_CURSOR_STABLE_OBSERVED", "ACKNOWLEDGED_STREAM_QUIESCENCE_OBSERVED") and e["terminal"]!="UNOBSERVED_AT_EXIT" for e in episodes) else "INCONCLUSIVE"}


def state_contract(traces, metadata, log):
    if len(traces) != len(metadata): raise ValueError("state trace count mismatch")
    unavailable, buffer_groups, group = False, [], []
    for row, state in zip(traces, metadata):
        effective, voice = state["effective_transport_active"], state["voice_playing"]
        if effective is None:
            unavailable = True
            continue
        if state["requested_playing"] != int(row[4]) or effective and (not row[4] or state["buffering"] or voice != 1):
            raise ValueError("contradictory actual transport state")
        if state["buffering"]:
            if effective or voice == 1:
                raise ValueError("PCM still playing during buffer gate")
            group.append((row, state))
        elif group:
            buffer_groups.append(group); group = []
    if group: buffer_groups.append(group)
    controls = []
    for rows in buffer_groups:
        stable = any(b[0][0] > a[0][0] and abs(b[0][3]-a[0][3]) <= .002
                     and a[1]["voice_playing"] == b[1]["voice_playing"] == 0
                     for a,b in zip(rows,rows[1:]))
        controls.append({"samples": len(rows), "status": "INCONCLUSIVE",
            "snapshot_pcm_stable": stable, "same_episode_unavailable": True,
            "scope": "snapshot adjacency cannot prove one episode; explicit lifecycle events required", "rows": [r[0] for r in rows]})
    active_segments, samples = [], []
    for line in log.splitlines():
        if REQUEST.search(line):
            if samples: active_segments.append(samples)
            samples = []
        match = TRACE.search(line)
        if match:
            state = trace_metadata(line)[0]
            if state["effective_transport_active"] == 1:
                samples.append(tuple(float(v) for v in match.groups()))
    if samples: active_segments.append(samples)
    segment_ok = len(active_segments) == len(list(REQUEST.finditer(log)))+1 and all(
        len(rows)>=2 and any(b[1]>a[1]+.01 and b[3]>a[3]+.01 for a,b in zip(rows,rows[1:]))
        for rows in active_segments)
    lifecycle = buffer_lifecycle(log)
    has_buffer = bool(buffer_groups)
    lifecycle_complete = (not has_buffer and not lifecycle["episodes"]) or lifecycle["status"]=="LIFECYCLE_OBSERVED"
    return {"status": "INCONCLUSIVE" if unavailable or not segment_ok or not lifecycle_complete else "ACTUAL_STATE_OBSERVED",
        "buffer_lifecycle": lifecycle,
        "legacy_actual_state_unavailable": unavailable, "active_segment_count": len(active_segments),
        "active_segment_progress_complete": segment_ok, "buffer_controls": controls}


def validate_timeline(log, seeks, paused=False):
    if "] ERROR -" in log or any(marker in log for marker in
        ("load failed", "video seek did not reach", "invalid metadata", "failed to create")):
        raise ValueError("viewer failure")
    loaded = re.findall(r"loaded .* (\d+)x(\d+) ([\d.]+)fps codec=(\S+) metadata_only=(\d+) dur=([\d.]+)s", log)
    if len(loaded) != 1 or int(loaded[0][4]) or min(int(loaded[0][0]), int(loaded[0][1])) < 1:
        raise ValueError("actual decoded viewer unavailable")
    metadata = trace_metadata(log)
    fps = float(loaded[0][2])
    if not math.isfinite(fps) or fps <= 0:
        raise ValueError("positive source FPS unavailable")
    picture_budget = 1.0 / fps + .002
    traces = [tuple(float(value) for value in match.groups()) for match in TRACE.finditer(log)]
    if len(traces) < 2 or any(not all(math.isfinite(v) for v in row) for row in traces):
        raise ValueError("missing actual viewer trace")
    if paused:
        if seeks or any(row[4] or row[3] > .05 for row in traces) or REQUEST.search(log):
            raise ValueError("paused viewer advanced transport")
        return {"status": "PAUSED_CONTROL_OBSERVED", "traces": traces, "trace_metadata": metadata, "scope": "paused GPU preview; no active PCM"}
    mute = re.findall(r"preview mute requested=1 master=([\d.]+)", log)
    if not mute or any(float(value) != 0 for value in mute):
        raise ValueError("actual viewer instance mute proof unavailable")
    first_playing = next((match.start() for match in TRACE.finditer(log) if match[5] == "1"), None)
    proof = re.search(r"preview mute requested=1 master=([\d.]+)", log)
    if first_playing is not None and proof.start() > first_playing:
        raise ValueError("mute proof follows playback")
    active = [row for row in traces if row[4] == 1]
    if len(active) < 2 or not any(row[12] > 0 and row[6] > 0 for row in active):
        raise ValueError("playing GPU uploads unavailable")
    if not any(b[3] > a[3]+.01 for a,b in zip(active, active[1:]) if b[1] >= a[1]):
        raise ValueError("active audio clock progress unavailable")
    pending, completed, events = None, [], []
    for line in log.splitlines():
        requested, waiting, ready = REQUEST.search(line), WAIT.search(line), READY.search(line)
        if requested:
            if pending is not None:
                raise ValueError("seek overlap without readiness")
            pending = float(requested[1]); events.append({"event": "requested", "target": pending})
        if waiting:
            if pending is None or abs(float(waiting[1])-pending) > 1e-5 or waiting[5] != "0":
                raise ValueError("audio voice ran before picture readiness")
            events.append({"event": "waiting", "target": pending, "pts": float(waiting[3])})
        if ready:
            values = tuple(float(v) for v in ready.groups())
            if (pending is None or abs(values[0]-pending) > 1e-5 or not all(math.isfinite(v) for v in values)
                or abs(values[2]-pending) > picture_budget or values[4] != 0
                or abs(values[3]-pending) > .002):
                raise ValueError("seek ready without target picture")
            completed.append(values); events.append({"event": "ready", "target": pending, "wait_ms": values[-1]})
            pending = None
    if pending is not None or len(completed) != len(seeks) or any(abs(row[0]-target)>1e-5 for row,target in zip(completed,seeks)):
        raise ValueError("requested seek sequence incomplete")
    if seeks:
        last_ready = list(READY.finditer(log))[-1]
        resumed = [tuple(float(v) for v in match.groups()) for match in TRACE.finditer(log, last_ready.end()) if match[5] == "1"]
        if len(resumed) < 2 or not any(b[3] > a[3]+.01 for a,b in zip(resumed,resumed[1:])):
            raise ValueError("post-seek audio playback progress unavailable")
    contract = state_contract(traces, metadata, log)
    lag_rows = [{"wall": row[0], "media_picture_s": abs(row[1]-row[2]),
                 "audio_picture_s": abs(row[3]-row[2]), "audio_media_s": abs(row[3]-row[1])}
                for row in active if row[2] >= 0 and row[3] >= 0]
    pending_sync, baseline, settled = False, None, []
    for line in log.splitlines():
        if REQUEST.search(line): pending_sync, baseline = True, None
        if READY.search(line): pending_sync, baseline = False, None
        match = TRACE.search(line)
        if not match or match[5] != "1" or pending_sync: continue
        state = trace_metadata(line)[0]
        if state["effective_transport_active"] != 1: continue
        row = tuple(float(v) for v in match.groups())
        if baseline is None: baseline = row[0]
        if row[0]-baseline >= 1.5 and row[2] >= 0 and row[3] >= 0:
            settled.append(row)
    synchronization = {"status": "INCONCLUSIVE" if len(settled)<2 or contract["status"] != "ACTUAL_STATE_OBSERVED" else "OBSERVED_WITHIN_EXPLICIT_BUDGET",
        "state_contract": contract,
        "settled_samples": len(settled), "excluded_grace_s": 1.5,
        "audio_media_budget_s": .15, "picture_budget_s": picture_budget,
        "max_active_audio_picture_s": max((r["audio_picture_s"] for r in lag_rows), default=None),
        "max_active_media_picture_s": max((r["media_picture_s"] for r in lag_rows), default=None),
        "legacy_max_active_fields_scope": "UI requested-playing rows, including buffered rows; not effective transport",
        "max_effective_transport_media_picture_s": max((abs(row[1]-row[2]) for row, state in zip(traces,metadata)
            if state["effective_transport_active"]==1 and row[2]>=0),default=None),
        "max_effective_transport_audio_picture_s": max((abs(row[3]-row[2]) for row, state in zip(traces,metadata)
            if state["effective_transport_active"]==1 and row[2]>=0 and row[3]>=0),default=None),
        "lag_rows": lag_rows, "progress_segments": progress_segments(log),
        "progress_segments_scope": "whole retained trace segments; includes buffering and stopped tails, not EOF throughput",
        "cumulative_buffer_stall_ms": max((row["stall_ms"] for row in metadata if row["stall_ms"] is not None), default=None),
        "stall_scope": "cumulative actual buffering time; legacy absent fields unavailable, no nominal FPS claim",
        "scope": "published clocks only; not independent displayed image PTS or acoustic sensor"}
    if any(abs(row[3]-row[1]) > .15 or abs(row[1]-row[2]) > picture_budget for row in settled):
        synchronization["status"] = "FAIL_EXPLICIT_BUDGET"
        raise ValueError("settled viewer clock/picture lag exceeds explicit budget: " + json.dumps(synchronization))
    return {"status": "EDITOR_MEDIA_FUNCTIONAL_OBSERVED", "loaded": loaded[0], "traces": traces, "trace_metadata": metadata,
            "synchronization": synchronization, "seek_audio_base_budget_s": .002,
            "seek_audio_scope": "exact seek resets cold consumed audio clock to base; ready logged before voice restart",
            "seeks": completed, "events": events, "mute_master_observed": list(map(float, mute)),
            "scope": "actual viewer trace/GPU upload and muted audio clocks; not acoustic output or independent image-PTS oracle"}


def validate_inactive_timeline(log):
    switch = re.search(r"preview tab event=switch elapsed_ms=[\d.]+",log)
    returned = re.search(r"preview tab event=return elapsed_ms=[\d.]+",log)
    if not switch or not returned or returned.start()<=switch.start():
        raise ValueError("inactive phase boundaries unavailable")
    pattern = r"loaded (.+?) (\d+)x(\d+) ([\d.]+)fps codec=(\S+) metadata_only=(\d+) dur=([\d.]+)s"
    loads=list(re.finditer(pattern,log))
    if len(loads)!=2 or loads[0].start()>=switch.start() or loads[1].start()<=returned.end() or loads[0].groups()!=loads[1].groups():
        raise ValueError("inactive return must reload exactly the same media metadata/path")
    before=log[:switch.start()]
    release_end=log.find("\n",log.find("preview focus active=0",switch.end()))
    # Keep cancel/release witnesses in the original instance, without returned clocks.
    first=validate_timeline(log[:release_end],[],False)
    second=validate_timeline(log[returned.end():],[],True)
    control=validate_inactive(log)
    first["inactive"]=control
    first["returned_instance"]=second
    first["scope"]="two independently validated viewer instances separated by actual release/return; clocks are not concatenated"
    return first


def validate_inactive(log):
    switch = list(re.finditer(r"preview tab event=switch elapsed_ms=([\d.]+)",log))
    returned = list(re.finditer(r"preview tab event=return elapsed_ms=([\d.]+)",log))
    if len(switch)!=1 or len(returned)!=1 or returned[0].start()<=switch[0].start():
        raise ValueError("one ordinary switch/return pair required")
    if float(returned[0][1])<=float(switch[0][1]): raise ValueError("nonmonotonic tab times")
    prefix=log[:switch[0].start()]
    if not any(m["effective_transport_active"]==1 for m in trace_metadata(prefix)):
        raise ValueError("actual playing before tab switch unavailable")
    between=log[switch[0].end():returned[0].start()]
    release=list(re.finditer(r"preview focus active=0 requested_playing=([01]) voice_playing=(-1|0|1) media=([\d.]+) audio=([\d.]+) released=1",between))
    if len(release)!=1 or release[0][1]!="0" or release[0][2]!="0":
        raise ValueError("actual valid voice pause before inactive resource release required")
    if TRACE.search(between): raise ValueError("inactive video emitted active viewer samples")
    after=log[returned[0].end():]
    rows=[tuple(float(v) for v in m.groups()) for m in TRACE.finditer(after)]
    states=trace_metadata(after)
    stamps = [re.match(r"(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)",line)[1]
        for line in after.splitlines() if TRACE.search(line) and re.match(r"\d{4}-",line)]
    sampled_later = rows and rows[-1][0]>rows[0][0] or len(stamps)>=2 and stamps[-1]>stamps[0]
    if len(rows)<2 or len(states)!=len(rows) or not sampled_later:
        raise ValueError("two returned-tab samples required")
    if any(m["requested_playing"]!=0 or m["effective_transport_active"]!=0
        or m["voice_playing"]!=-1 or m["buffering"]!=0 or abs(r[3])>.000002
        for r,m in zip(rows,states)):
        raise ValueError("returned tab auto-played or retained an unexpected voice")
    return {"status":"INACTIVE_TAB_RELEASE_AND_NO_AUTOPLAY_OBSERVED",
        "switch_elapsed_ms":float(switch[0][1]),"return_elapsed_ms":float(returned[0][1]),
        "returned_samples":len(rows),"release_media_s":float(release[0][3]),
        "release_audio_s":float(release[0][4]),
        "scope":"ordinary tab selection; valid voice paused before release; return is new loaded state with no voice, not frozen released clock"}


def validate_eof(timeline):
    duration = float(timeline["loaded"][5]); fps = float(timeline["loaded"][2])
    if not math.isfinite(duration) or duration<=0 or not math.isfinite(fps) or fps<=0:
        raise ValueError("finite positive EOF duration/fps required")
    budget = 1/fps+.002
    rows = timeline["traces"]; states = timeline["trace_metadata"]
    if len(rows)!=len(states) or len(rows)<3:
        raise ValueError("complete EOF state samples required")
    if not any(m["effective_transport_active"]==1 for m in states[:-2]):
        raise ValueError("actual playback before EOF unavailable")
    terminal = rows[-2:]; final_states = states[-2:]
    if (terminal[1][0]<=terminal[0][0] or any(
        m["requested_playing"]!=0 or m["effective_transport_active"]!=0
        or m["voice_playing"]!=0 or m["buffering"]!=0 for m in final_states)
        or any(not all(math.isfinite(v) for v in r[:4])
            or abs(r[1]-duration)>budget or abs(r[2]-(duration-1/fps))>budget
            or abs(r[3]-duration)>budget for r in terminal)
        or any(abs(terminal[1][i]-terminal[0][i])>.000002 for i in (1,2,3))):
        raise ValueError("EOF endpoint/state/stability proof incomplete")
    active_indices = [i for i,m in enumerate(states) if m["effective_transport_active"]==1]
    start = active_indices[0]
    first_end = next((i for i in range(start+1,len(rows))
        if states[i]["requested_playing"]==0 and states[i]["effective_transport_active"]==0
        and states[i]["voice_playing"]==0 and states[i]["buffering"]==0
        and abs(rows[i][1]-duration)<=budget and abs(rows[i][2]-(duration-1/fps))<=budget
        and abs(rows[i][3]-duration)<=budget), None)
    if first_end is None or rows[first_end][0]<=rows[start][0]:
        raise ValueError("first actual EOF progress interval unavailable")
    interval = rows[first_end][0]-rows[start][0]
    completed_progress = rows[first_end][1]-rows[start][1]
    progress = {"start_wall_s":rows[start][0], "start_media_s":rows[start][1],
        "first_eof_wall_s":rows[first_end][0], "first_eof_media_s":rows[first_end][1],
        "wall_interval_s":interval, "media_progress_s":completed_progress,
        "effective_media_speed":completed_progress/interval,
        "scope":"first actual active sample to first EOF endpoint; includes intervening buffering, excludes subsequent stopped tail; sampled endpoint timing"}
    return {"status":"PUBLIC_ENDPOINT_EOF_OBSERVED", "duration_s":duration,
        "source_frame_budget_s":budget,"terminal_rows":terminal,
        "endpoint_progress":progress,
        "scope":"actual viewer clocks and stopped valid audio voice; not independent image/acoustic EOF sensor; absent-audio state unsupported"}


def frame_perf(log, timeline, requested=False):
    if not requested: return {"requested":False,"status":"NOT_REQUESTED"}
    pattern=re.compile(r"frame perf context=(owner|worker) samples=(\d+) source=(\d+)x(\d+) preview=(\d+)x(\d+) scale_ms=(\S+) color_ms=(\S+)")
    matches=list(pattern.finditer(log))
    if not matches: raise ValueError("requested actual frame performance observations unavailable")
    source=(int(timeline["loaded"][0]),int(timeline["loaded"][1]))
    metadata=timeline["trace_metadata"]
    previews={(r["upload_width"],r["upload_height"]) for r in metadata if r.get("upload_width",0) and r.get("upload_height",0)}
    records=[];previous={}
    for match in matches:
        context=match[1]
        n,w,h,pw,ph=map(int,match.groups()[1:6]);scale,color=map(float,match.groups()[6:])
        if (n<=previous.get(context,0) or (w,h)!=source or min(pw,ph)<1 or pw>w or ph>h
                or (pw,ph) not in previews or not math.isfinite(scale) or not math.isfinite(color)
                or scale<0 or color<0):
            raise ValueError("invalid actual frame perf dimensions/counter/timing")
        previous[context]=n;records.append({"context":context,"samples":n,"source":[w,h],"preview":[pw,ph],"scale_ms":scale,"color_ms":color})
    summaries={}
    for context in previous:
        group=[r for r in records if r["context"]==context]
        summaries[context]={"logged_observations":len(group),"last_samples":previous[context],
            "last":group[-1],"max_logged_scale_ema_ms":max(r["scale_ms"] for r in group),
            "max_logged_color_ema_ms":max(r["color_ms"] for r in group)}
    return {"requested":True,"status":"DIAGNOSTIC_EMA_OBSERVED", "contexts":summaries,
        "scope":"separate owner/worker sampled EMA: scale includes plane allocation/copy/scale; color only converter; not frame percentiles or throughput acceptance"}



def producer_perf(log, codec, requested=False):
    """Retain per-call HEVC EMAs; unequal phase populations are not additive."""
    if not requested:
        return {"requested": False, "status": "NOT_REQUESTED"}
    if codec not in ("hvc1", "hev1", "hevc", "h265"):
        return {"requested": True, "status": "NOT_APPLICABLE"}
    counters = ("io_samples", "annexb_samples", "codec_calls", "publish_samples")
    timings = ("io_ms", "annexb_ms", "codec_ms", "publish_ms")
    pattern = re.compile(
        r"producer perf decoder=((?:0x)?[0-9a-fA-F]+) "
        r"io_samples=(\d+) annexb_samples=(\d+) codec_calls=(\d+) publish_samples=(\d+) "
        r"io_ms=(\S+) annexb_ms=(\S+) codec_ms=(\S+) publish_ms=(\S+) "
        r"codec_empty=(\d+) codec_no_output=(\d+)")
    records = []
    previous = {}
    epochs = {}
    for line in log.splitlines():
        if "producer perf " not in line:
            continue
        match = pattern.search(line)
        tail = line[match.end():].strip() if match else ""
        if not match or (tail and not tail.startswith("at ")):
            raise ValueError("malformed producer perf record")
        decoder = match[1]
        counts = list(map(int, match.groups()[1:5]))
        values = list(map(float, match.groups()[5:9]))
        empty, no_output = map(int, match.groups()[9:])
        if (counts[2] == 0 or empty > counts[2] or no_output > counts[2]
                or any(not math.isfinite(v) or v < 0 for v in values)
                or any(n == 0 and v != 0 for n, v in zip(counts, values))):
            raise ValueError("invalid producer perf count/timing")
        # Allocator reuse after seek can reuse an address; no lifecycle ID exists.
        reset = decoder in previous and any(a < b for a, b in zip(counts, previous[decoder]))
        epochs[decoder] = epochs.get(decoder, 0) + int(reset)
        previous[decoder] = counts
        records.append({"decoder_address": decoder, "counter_epoch": epochs[decoder],
                        "address_reuse_or_counter_reset": reset,
                        **dict(zip(counters, counts)), **dict(zip(timings, values)),
                        "codec_empty": empty, "codec_no_output": no_output})
    if not records:
        raise ValueError("requested HEVC producer performance observations unavailable")
    return {"requested": True, "status": "DIAGNOSTIC_EMA_OBSERVED", "records": records,
            "observed_stages": [key for key in counters if any(r[key] for r in records)],
            "scope": "decoder-owned per-call sampled EMA; IO includes source lock/cache/file read; "
                     "AnnexB includes buffer growth; codec is actual API call including header/seek/drain; "
                     "publish includes VUI and optional legacy RGBA. Unequal sample populations must not "
                     "be summed as per-picture time, percentiles or throughput acceptance; decoder address "
                     "is not a stable lifecycle identity. Zero-count stage timing is unavailable."}


def hevc_diagnostics(log, codec, threads=None, keep_active=None, generic=False):
    requested={"cores":threads,"keep_threads_active":keep_active,"generic":bool(generic)}
    override = threads is not None or keep_active is not None or generic
    if not override and codec not in ("hvc1","hev1","hevc","h265"):
        return {"requested":requested,"resolved":None,"status":"NOT_APPLICABLE","requested_match":None}
    if codec not in ("hvc1","hev1","hevc","h265"):
        raise ValueError("HEVC diagnostic override on non-HEVC media")
    records=[{"cores":int(a),"keep_threads_active":int(b),"processor_arch":int(c) if c else None} for a,b,c in
        re.findall(r"HEVC worker cores=(\d+) keep_threads_active=([01])(?: processor_arch=(\d+))?",log)]
    if not records:
        if override: raise ValueError("actual HEVC decoder initialization diagnostic unavailable")
        return {"requested":requested,"resolved":None,"status":"UNAVAILABLE","requested_match":None,
            "scope":"actual configuration witness absent; no source-default assumption"}
    if any(not 1<=r["cores"]<=8 for r in records):
        raise ValueError("invalid actual HEVC decoder configuration")
    if generic and any(r["processor_arch"]!=256 for r in records):
        raise ValueError("HEVC generic override lacks actual GENERIC processor acknowledgement")
    return {"requested":requested,"resolved":records,
        "requested_match":all((threads is None or r["cores"]==threads) and (keep_active is None or r["keep_threads_active"]==int(keep_active)) for r in records) if override else None,
        "status":"ACTUAL_CONFIGURATION_OBSERVED",
        "scope":"actual decoder configuration, not worker utilization or throughput guarantee"}


def trace_environment(native_trace=False):
    env = {"JCE_DBG_FILE_PREVIEW_TRACE": "1", "JCE_DBG_FILE_PREVIEW_TRACE_MS": "100"}
    if native_trace:
        env.update(JCE_VIDEO_TRACE="1", JCE_AUDIO_TRACE="1")
    return env


def add_hevc_thread_options(parser):
    parser.add_argument("--hevc-threads", type=int, choices=range(1, 9))
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--hevc-keep-threads-active", dest="hevc_keep_threads_active",
                       action="store_const", const=True, default=None)
    group.add_argument("--hevc-keep-threads-idle", dest="hevc_keep_threads_active",
                       action="store_const", const=False)


def hevc_thread_environment(threads, keep_active):
    env = {}
    if threads is not None:
        env["JCE_DBG_HEVC_THREADS"] = str(threads)
    if keep_active is not None:
        env["JCE_DBG_HEVC_KEEP_THREADS_ACTIVE"] = "1" if keep_active else "0"
    return env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--file", type=Path, required=True)
    parser.add_argument("--backend", choices=("d3d11", "d3d12", "vulkan", "gl"), default="d3d11")
    parser.add_argument("--mute", action="store_true", help="required for active controls, proof enforced")
    parser.add_argument("--paused", action="store_true")
    parser.add_argument("--frame-perf", action="store_true", help="require actual private frame conversion diagnostics")
    parser.add_argument("--hevc-generic", action="store_true", help="require actual generic HEVC processor dispatch")
    add_hevc_thread_options(parser)
    parser.add_argument("--native-trace", action="store_true", help="opt in to verbose native video/audio diagnostics")
    seek_group = parser.add_mutually_exclusive_group()
    seek_group.add_argument("--seeks", default=None)
    seek_group.add_argument("--no-seeks", action="store_true")
    parser.add_argument("--expect-eof", action="store_true")
    parser.add_argument("--inactive-control", action="store_true")
    parser.add_argument("--switch-text", type=Path)
    parser.add_argument("--switch-ms", type=int, default=1000)
    parser.add_argument("--return-ms", type=int, default=3000)
    parser.add_argument("--frames", type=int, default=100000)
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.seeks = args.seeks if args.seeks is not None else "3.4,1.0"
    seeks = [] if args.paused or args.no_seeks else [float(v) for v in args.seeks.split(",")]
    if (not args.exe.is_file() or not args.file.is_file() or not args.paused and not args.mute
        or args.expect_eof and args.paused
        or args.inactive_control and (args.paused or not args.no_seeks or args.expect_eof
            or not args.switch_text or not args.switch_text.is_file()
            or not 100<=args.switch_ms<args.return_ms<=600000)
        or not 200 <= args.frames <= 1000000 or not 5 <= args.seconds <= 300
        or not args.seconds+10 <= args.timeout <= 600 or len(seeks)>8
        or any(not math.isfinite(v) or v<0 for v in seeks)):
        parser.error("existing inputs, active mute, bounded duration and finite seeks required")
    args.out = args.out.resolve(); args.out.mkdir(parents=True, exist_ok=False)
    profile = args.out / "profile"; profile.mkdir()
    hashes = {"executable": sha256_file(args.exe), "media": sha256_file(args.file)}
    env = {"JCE_BACKEND": args.backend, "JCE_MULTI_INSTANCE": "1", "JCE_WINDOW_HIDDEN": "1",
        "JCE_EDITOR_PROFILE_DIR": str(profile), "JCE_DBG_FILE_PREVIEW": str(args.file.resolve()),
        "JCE_WINCAP_FRAME": "150",
        "JCE_WINCAP_PATH": str(args.out / "editor.png")}
    env.update(trace_environment(args.native_trace))
    if args.frame_perf: env["JCE_DBG_VIDEO_FRAME_PERF"] = "1"
    if args.hevc_generic: env["JCE_DBG_HEVC_GENERIC"] = "1"
    env.update(hevc_thread_environment(args.hevc_threads, args.hevc_keep_threads_active))
    if args.inactive_control:
        hashes["switch_text"] = sha256_file(args.switch_text)
        env.update(JCE_DBG_FILE_PREVIEW_SWITCH_TEXT=str(args.switch_text.resolve()),
            JCE_DBG_FILE_PREVIEW_SWITCH_MS=str(args.switch_ms),JCE_DBG_FILE_PREVIEW_RETURN_MS=str(args.return_ms))
    if args.paused: env["JCE_DBG_FILE_PREVIEW_PAUSED"] = "1"
    else:
        env["JCE_DBG_FILE_PREVIEW_MUTE"] = "1"
        if not args.no_seeks: env["JCE_DBG_FILE_PREVIEW_SEEKS"] = args.seeks
    result = {"status": "INCOMPLETE", "source": source_identity(), "environment": env,
        "input": {"executable": str(args.exe.resolve()), "media": str(args.file.resolve()), "sha256": hashes},
        "trace_scope": "viewer plus verbose native/audio" if args.native_trace else "viewer only",
        "scope": "hidden real editor viewer; functional ambient observation, no formal frame/4K performance claim"}
    write(args.out / "intent.json", result)
    try:
        process = run(env, args.out / "engine.log", frames=args.frames, max_seconds=args.seconds,
            timeout=args.timeout, exe=args.exe.resolve(), samples_path=args.out / "frames.csv", sample_interval=.1)
        if process["process"]["returncode"] != 0 or process["process"]["timed_out"]:
            raise ValueError("abnormal editor exit")
        log = (args.out / "engine.log").read_text(encoding="utf-8")
        actual = assert_backend(log, args.backend)
        observed_hashes = {"executable":sha256_file(args.exe), "media":sha256_file(args.file)}
        if args.inactive_control: observed_hashes["switch_text"] = sha256_file(args.switch_text)
        if actual != process["backend_actual"] or hashes != observed_hashes:
            raise ValueError("backend/input identity changed")
        pixels = read_png(args.out / "editor.png")
        if min(pixels[0],pixels[1]) < 1 or len(pixels[3]) != pixels[0]*pixels[1]*pixels[2]:
            raise ValueError("incomplete editor PNG")
        frames = read_frames(args.out / "frames.csv", warmup=0, sample_count=None)
        timeline = validate_inactive_timeline(log) if args.inactive_control else validate_timeline(log,seeks,args.paused)
        timeline["hevc_diagnostics"] = hevc_diagnostics(log,timeline["loaded"][3],args.hevc_threads,args.hevc_keep_threads_active,args.hevc_generic)
        if timeline["hevc_diagnostics"]["requested_match"] is False:
            raise ValueError("actual HEVC configuration does not match requested controls")
        timeline["frame_perf"] = frame_perf(log,timeline,args.frame_perf)
        timeline["producer_perf"] = producer_perf(log,timeline["loaded"][3],args.frame_perf)
        if args.expect_eof: timeline["eof"] = validate_eof(timeline)
        if args.inactive_control: timeline["inactive"] = validate_inactive(log)
        stall_ms = timeline.get("synchronization", {}).get("cumulative_buffer_stall_ms")
        if stall_ms is not None and stall_ms > process["process"]["elapsed_s"]*1000+1:
            raise ValueError("buffer stall exceeds actual process lifetime")
        result.update(status="FUNCTIONAL_OBSERVATIONS", process=process, actual_backend=actual,
            timeline=timeline, completed_frames=len(frames["frame_ms"]))
    except Exception as error:
        result.update(status="FAIL", error=str(error)); raise
    finally:
        result["raw_sha256"] = {str(p.relative_to(args.out)):sha256_file(p) for p in args.out.rglob("*") if p.is_file() and p.name not in ("result.json",)}
        write(args.out / "result.json",result)


if __name__ == "__main__":
    main()
