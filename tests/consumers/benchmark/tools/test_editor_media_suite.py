import unittest
from editor_media_suite import validate_timeline, trace_metadata, progress_segments, state_contract, TRACE, buffer_lifecycle, trace_environment, validate_eof, validate_inactive, validate_inactive_timeline, hevc_diagnostics, frame_perf, producer_perf


def trace(media, audio, playing=1):
    return f"viewer trace wall=1.000 media={media:.3f} pts={media:.3f} audio={audio:.3f} playing={playing} dec=10 disp=9 drop=0 q=2 decode_ms=1.0 convert_ms=1.0 pop_ms=1.0 upload_ms=1.0 gaps25=0 gaps50=0 max_gap_ms=10 upload_width=1280 upload_height=720 preview_limit=1280 buffering=0 stall_ms=0 requested_playing={playing} effective_transport_active={playing} voice_playing={playing}"


class MediaTimelineTests(unittest.TestCase):
    def test_actual_budget_failure_dominates_inconclusive_contract(self):
        log = "loaded clip.mp4 100x100 30.00fps codec=av1 metadata_only=0 dur=20.00s\npreview mute requested=1 master=0.000000\n"
        log += trace(.1,.1).replace("wall=1.000","wall=0.000")+"\n"
        log += trace(.2,.2).replace("wall=1.000","wall=2.000").replace("pts=0.200","pts=0.100")+"\n"
        log += trace(.3,.3).replace("wall=1.000","wall=3.000")
        # No complete three-segment/buffer lifecycle proof, but a real active violation wins.
        with self.assertRaisesRegex(ValueError, "FAIL_EXPLICIT_BUDGET"):
            validate_timeline(log, [])

    def test_frame_perf_real_dimensions_and_finite_values(self):
        timeline={"loaded":["1920","1080"],"trace_metadata":[{"upload_width":1280,"upload_height":720}]}
        log="frame perf context=owner samples=10 source=1920x1080 preview=1280x720 scale_ms=1.000000 color_ms=2.000000"
        self.assertEqual(frame_perf(log,timeline,True)["contexts"]["owner"]["last_samples"],10)
        interleaved=log+"\n"+log.replace("context=owner samples=10","context=worker samples=1")+"\n"+log.replace("samples=10","samples=11")
        groups=frame_perf(interleaved,timeline,True)["contexts"]
        self.assertEqual(groups["owner"]["last_samples"],11)
        self.assertEqual(groups["worker"]["last_samples"],1)
        for bad in ("",log.replace("source=1920","source=3840"),log.replace("preview=1280","preview=640"),
                    log.replace("1.000000","nan"),log.replace("1.000000","-1"),log+"\n"+log):
            with self.assertRaises(ValueError):frame_perf(bad,timeline,True)

    def test_producer_phase_scope_and_strict_negative_controls(self):
        initial = ("producer perf decoder=000000001ABC io_samples=0 annexb_samples=0 "
                   "codec_calls=1 publish_samples=0 io_ms=0 annexb_ms=0 codec_ms=36.5 "
                   "publish_ms=0 codec_empty=0 codec_no_output=1")
        later = ("producer perf decoder=000000001ABC io_samples=20 annexb_samples=20 "
                 "codec_calls=23 publish_samples=19 io_ms=0.1 annexb_ms=0.2 codec_ms=32 "
                 "publish_ms=0.01 codec_empty=2 codec_no_output=4")
        result = producer_perf(initial + "\n" + later + " at jce_h265_decode.c:210", "hvc1", True)
        self.assertEqual(result["records"][-1]["codec_calls"], 23)
        self.assertNotIn("total_ms", result)
        self.assertEqual(len(result["observed_stages"]), 4)
        self.assertEqual(producer_perf("", "av01", True)["status"], "NOT_APPLICABLE")
        self.assertEqual(producer_perf("", "hvc1")["status"], "NOT_REQUESTED")
        reset = producer_perf(later + "\n" + initial, "hvc1", True)["records"][-1]
        self.assertTrue(reset["address_reuse_or_counter_reset"])
        self.assertEqual(reset["counter_epoch"], 1)
        for bad in ("", later.replace("codec_ms=32", "codec_ms=nan"),
                    later.replace("codec_ms=32", "codec_ms=-1"),
                    later.replace("codec_no_output=4", "codec_no_output=24"),
                    later.replace("codec_calls=23", "codec_calls=0"),
                    initial.replace("io_ms=0 ", "io_ms=1 "),
                    later.replace("publish_samples=19 ", ""), later + " unknown=1"):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                producer_perf(bad, "hvc1", True)

    def test_generic_dispatch_requires_actual_processor_identity(self):
        observed=hevc_diagnostics("HEVC worker cores=8 keep_threads_active=1 processor_arch=256","hvc1",generic=True)
        self.assertEqual(observed["resolved"][0]["processor_arch"],256)
        for log in ("HEVC worker cores=8 keep_threads_active=1",
                    "HEVC worker cores=8 keep_threads_active=1 processor_arch=258"):
            with self.assertRaises(ValueError):hevc_diagnostics(log,"hvc1",generic=True)
        self.assertIsNone(hevc_diagnostics("HEVC worker cores=8 keep_threads_active=1","hvc1")["resolved"][0]["processor_arch"])

    def test_hevc_requested_and_actual_configuration_separate(self):
        default=hevc_diagnostics("HEVC worker cores=8 keep_threads_active=1","hvc1")
        self.assertEqual(default["resolved"][0]["cores"],8)
        self.assertIsNone(default["requested_match"])
        self.assertEqual(hevc_diagnostics("","hvc1")["status"],"UNAVAILABLE")
        result=hevc_diagnostics("HEVC worker cores=1 keep_threads_active=0","hvc1",8,True)
        self.assertFalse(result["requested_match"])
        self.assertEqual(result["resolved"][0]["cores"],1)
        self.assertTrue(hevc_diagnostics("HEVC worker cores=4 keep_threads_active=1","hvc1",4,True)["requested_match"])
        for codec,log in (("av01","HEVC worker cores=4 keep_threads_active=1"),("hvc1","")):
            with self.assertRaises(ValueError):hevc_diagnostics(log,codec,4,True)

    def test_actual_quiescence_ack_not_voice_pause_guess(self):
        def event(kind,wall,audio,ack,voice=0):
            return f"buffer event={kind} id=1 epoch=0 wall_ms={wall} elapsed_ms={wall-1000} media=1 pts=1 audio={audio:.6f} voice_playing={voice} quiescent={ack}"
        log="\n".join((event("enter",1000,1,0),event("hold",1000,1.01,1),
            event("pre_resume",1000,1.01,1),event("resume",1000,1.02,0,1)))
        result=buffer_lifecycle(log)
        self.assertEqual(result["status"],"LIFECYCLE_OBSERVED")
        self.assertAlmostEqual(result["episodes"][0]["initial_inflight_cursor_progress_s"],.01)
        for bad in (log.replace("event=pre_resume id=1 epoch=0","event=pre_resume id=1 epoch=1"),
            log.replace("audio=1.010000 voice_playing=0 quiescent=1\n"+event("resume",1000,1.02,0,1),
                "audio=1.020000 voice_playing=0 quiescent=1\n"+event("resume",1000,1.02,0,1)),
            log.replace("event=pre_resume id=1 epoch=0 wall_ms=1000 elapsed_ms=0 media=1 pts=1 audio=1.010000 voice_playing=0 quiescent=1",
                event("pre_resume",1000,1.01,0))):
            with self.assertRaises(ValueError):buffer_lifecycle(bad)

    def test_reload_metadata_path_and_instance_boundary_reject(self):
        for log in ("", "preview tab event=switch elapsed_ms=1\npreview tab event=return elapsed_ms=2\nloaded wrong.mp4 10x10 60fps codec=x metadata_only=0 dur=10s"):
            with self.assertRaises(ValueError):validate_inactive_timeline(log)

    def test_inactive_release_and_return_do_not_invent_old_clock(self):
        resumed=trace(0,0,0).replace("voice_playing=0","voice_playing=-1")
        log=trace(.2,.2)+"\npreview tab event=switch elapsed_ms=1000.000\npreview focus active=0 requested_playing=0 voice_playing=0 media=.2 audio=.2 released=1\npreview tab event=return elapsed_ms=3000.000\n"+resumed+"\n"+resumed.replace("wall=1.000","wall=2.000")
        self.assertEqual(validate_inactive(log)["returned_samples"],2)
        for bad in (log.replace("voice_playing=0 media", "voice_playing=-1 media"),
            log.replace("voice_playing=-1", "voice_playing=1"),
            log.replace("event=return", "event=switch"),log.rsplit("\n",1)[0]):
            with self.assertRaises(ValueError):validate_inactive(bad)

    def test_eof_progress_excludes_stopped_tail_and_respects_offsets(self):
        active={"effective_transport_active":1}
        stopped={"requested_playing":0,"effective_transport_active":0,"voice_playing":0,"buffering":0}
        timeline={"loaded":["100","100","60","av1","0","10"],
            "traces":[[2,1,1,1],[11,10,10-1/60,10],[21,10,10-1/60,10]],
            "trace_metadata":[active,dict(stopped),dict(stopped)]}
        result=validate_eof(timeline)["endpoint_progress"]
        self.assertEqual(result["wall_interval_s"],9)
        self.assertEqual(result["media_progress_s"],9)
        self.assertEqual(result["effective_media_speed"],1)

    def test_eof_requires_actual_stopped_terminal_endpoints(self):
        states=[{"effective_transport_active":1},{"requested_playing":0,
            "effective_transport_active":0,"voice_playing":0,"buffering":0}]
        timeline={"loaded":["100","100","60.00","av1","0","10.00"],
            "traces":[[0,.1,.1,.1],[11,10,10-1/60,10],[12,10,10-1/60,10]],
            "trace_metadata":[states[0],dict(states[1]),dict(states[1])]}
        self.assertEqual(validate_eof(timeline)["status"],"PUBLIC_ENDPOINT_EOF_OBSERVED")
        for key,value in (("requested_playing",1),("effective_transport_active",1),
                          ("voice_playing",1),("voice_playing",-1),("buffering",1)):
            old=timeline["trace_metadata"][-1][key];timeline["trace_metadata"][-1][key]=value
            with self.assertRaises(ValueError):validate_eof(timeline)
            timeline["trace_metadata"][-1][key]=old
        timeline["loaded"][-1]="20.00"
        with self.assertRaises(ValueError):validate_eof(timeline)
        timeline["loaded"][-1]="10.00";timeline["traces"].pop();timeline["trace_metadata"].pop()
        with self.assertRaises(ValueError):validate_eof(timeline)

    def test_viewer_only_trace_default_and_explicit_native(self):
        self.assertEqual(trace_environment(), {"JCE_DBG_FILE_PREVIEW_TRACE": "1",
            "JCE_DBG_FILE_PREVIEW_TRACE_MS": "100"})
        native = trace_environment(True)
        self.assertEqual(native["JCE_VIDEO_TRACE"], "1")
        self.assertEqual(native["JCE_AUDIO_TRACE"], "1")

    def test_real_seek_gate_and_mute_required(self):
        log = "loaded clip.mp4 1920x1080 30.00fps codec=av1 metadata_only=0 dur=20.00s\npreview mute requested=1 master=0.000000\n"+trace(.1,.1)+"\n"+trace(.2,.2)+"\nseek requested target=3.400000 from=.200000\nseek waiting target=3.400000 media=3.400000 pts=1.0 audio=3.4 voice=0\nseek ready target=3.400000 media=3.400000 pts=3.4 audio=3.4 voice=0 wait_ms=2500\n"+trace(3.5,3.5)+"\n"+trace(3.6,3.6)
        self.assertEqual(validate_timeline(log,[3.4])["seeks"][0][-1],2500)
        validate_timeline(log.replace("pts=3.4", "pts=3.433"), [3.4])
        with self.assertRaises(ValueError):
            validate_timeline(log.replace("pts=3.4", "pts=3.437"), [3.4])
        for bad in (log.replace("master=0.000000", "master=1.000000"),
                    log.replace("voice=0", "voice=1",1), log.replace("pts=3.4", "pts=1.0"),
                    log.split("seek ready")[0], log+" ] ERROR - error",
                    log.replace("pts=3.4", "pts=4.4"),
                    log.replace("30.00fps", "0.00fps"),
                    log.replace("audio=3.4 voice=0 wait_ms", "audio=0.0 voice=0 wait_ms"),
                    log.replace(trace(3.6,3.6),trace(3.6,3.5))):
            with self.assertRaises(ValueError): validate_timeline(bad,[3.4])

    def test_explicit_extensions_and_failure_status(self):
        line = trace(1,1).replace("buffering=0 stall_ms=0", "buffering=1 stall_ms=2000").replace("effective_transport_active=1 voice_playing=1", "effective_transport_active=0 voice_playing=0")
        self.assertEqual(trace_metadata(line)[0]["stall_ms"], 2000)
        self.assertEqual(trace_metadata(trace(1,1).split(" upload_width=")[0])[0]["schema"], "legacy-clock-only")
        with self.assertRaises(ValueError): trace_metadata(line+" unknown=4")
        log = "loaded clip.mp4 1920x1080 30.00fps codec=av1 metadata_only=0 dur=20.00s\npreview mute requested=1 master=0.000000\n"
        log += trace(1,1).replace("wall=1.000", "wall=0.000")+"\n"
        log += trace(2,2).replace("pts=2.000", "pts=1.000").replace("wall=1.000", "wall=2.000")+"\n"
        log += trace(3,3).replace("pts=3.000", "pts=1.000").replace("wall=1.000", "wall=3.000")
        with self.assertRaisesRegex(ValueError, "FAIL_EXPLICIT_BUDGET"):
            validate_timeline(log, [])
        segments = progress_segments(log)
        self.assertAlmostEqual(segments[0]["effective_media_speed"], 2/3)

    def test_buffer_contract_requires_actual_pcm_and_progress(self):
        first = trace(1,1).replace("wall=1.000", "wall=0.000")
        second = trace(2,2)
        buffered = trace(2,2).replace("buffering=0 stall_ms=0", "buffering=1 stall_ms=1000").replace("effective_transport_active=1 voice_playing=1", "effective_transport_active=0 voice_playing=0")
        later = buffered.replace("wall=1.000", "wall=2.000").replace("stall_ms=1000", "stall_ms=2000")
        log = "\n".join((first, second, buffered, later))
        rows=[tuple(float(v) for v in m.groups()) for m in TRACE.finditer(log)]
        observed=state_contract(rows,trace_metadata(log),log)
        self.assertEqual(observed["status"], "INCONCLUSIVE")
        self.assertTrue(observed["buffer_controls"][0]["snapshot_pcm_stable"])
        self.assertTrue(observed["buffer_controls"][0]["same_episode_unavailable"])
        isolated="\n".join((first,second,buffered))
        rows=[tuple(float(v) for v in m.groups()) for m in TRACE.finditer(isolated)]
        self.assertEqual(state_contract(rows,trace_metadata(isolated),isolated)["status"],"INCONCLUSIVE")
        bad=log.replace("effective_transport_active=0 voice_playing=0","effective_transport_active=0 voice_playing=1")
        with self.assertRaises(ValueError):state_contract(rows,trace_metadata(bad)[:len(rows)],bad)

    def test_explicit_buffer_episode_identity_and_pcm(self):
        def event(kind, identifier=1, epoch=0, wall=1000, elapsed=0, audio=1.0, voice=0):
            return f"buffer event={kind} id={identifier} epoch={epoch} wall_ms={wall} elapsed_ms={elapsed} media=1.0 pts=1.0 audio={audio:.6f} voice_playing={voice}"
        good="\n".join((event("enter"), event("hold",wall=1050,elapsed=50),
            event("hold",wall=1100,elapsed=100),event("pre_resume",wall=1150,elapsed=150),event("resume",wall=1150,elapsed=150,voice=1)))
        self.assertEqual(buffer_lifecycle(good)["status"],"LIFECYCLE_OBSERVED")
        short="\n".join((event("enter"),event("hold",wall=1001,elapsed=1),
            event("pre_resume",wall=1002,elapsed=2),event("resume",wall=1002,elapsed=2,audio=1.02,voice=1)))
        self.assertEqual(buffer_lifecycle(short)["episodes"][0]["stability"],"SHORT_CURSOR_STABLE_OBSERVED")
        with self.assertRaises(ValueError):
            buffer_lifecycle(short.replace(event("pre_resume",wall=1002,elapsed=2)+"\n", ""))
        canceled="\n".join((event("enter"),event("hold",wall=1001,elapsed=1),
            event("cancel",wall=1002,elapsed=2)))
        self.assertEqual(buffer_lifecycle(canceled)["episodes"][0]["stability"],
            "SHORT_CANCEL_CURSOR_STABLE_OBSERVED")
        for terminal in (event("cancel",wall=1002,elapsed=2,voice=1),
                         event("cancel",wall=1002,elapsed=2,audio=1.01)):
            bad_cancel="\n".join((event("enter"),event("hold",wall=1001,elapsed=1),terminal))
            self.assertEqual(buffer_lifecycle(bad_cancel)["status"],"INCONCLUSIVE")
        self.assertEqual(buffer_lifecycle(canceled.rsplit("\n",1)[0])["status"],"INCONCLUSIVE")
        moving_short=short.replace(event("pre_resume",wall=1002,elapsed=2),event("pre_resume",wall=1002,elapsed=2,audio=1.01))
        self.assertEqual(buffer_lifecycle(moving_short)["status"],"INCONCLUSIVE")
        moving=good.replace("audio=1.000000 voice_playing=0\n"+event("resume"),"audio=1.200000 voice_playing=0\n"+event("resume"))
        moving=good.replace(event("hold",wall=1100,elapsed=100),event("hold",wall=1100,elapsed=100,audio=1.2))
        with self.assertRaises(ValueError):buffer_lifecycle(moving)
        for bad in (good.split("\n",1)[1],good.replace("event=hold id=1","event=hold id=2"),
                    good.replace("event=resume id=1 epoch=0","event=resume id=1 epoch=1"),
                    good.replace("audio=1.000000 voice_playing=0","audio=1.000000 voice_playing=1",1)):
            with self.assertRaises(ValueError):buffer_lifecycle(bad)
        self.assertEqual(buffer_lifecycle(event("enter")+"\n"+event("cancel",wall=1010,elapsed=10))["status"],"INCONCLUSIVE")
        wrongactive=event("enter")+"\n"+trace(1,1)
        with self.assertRaises(ValueError):buffer_lifecycle(wrongactive)

    def test_paused_does_not_claim_pcm(self):
        log="loaded clip.mp4 100x100 30.00fps codec=av1 metadata_only=0 dur=20.00s\n"+trace(0,0,0)+"\n"+trace(0,0,0)
        self.assertEqual(validate_timeline(log,[],True)["status"],"PAUSED_CONTROL_OBSERVED")
        with self.assertRaises(ValueError):validate_timeline(log+"\n"+trace(1,1),[],True)


if __name__ == "__main__":unittest.main()

class HevcIdleControlTests(unittest.TestCase):
    def test_explicit_idle_and_default_are_distinct(self):
        from editor_media_suite import hevc_thread_environment, hevc_diagnostics
        self.assertEqual({}, hevc_thread_environment(None, None))
        self.assertEqual({"JCE_DBG_HEVC_THREADS": "8", "JCE_DBG_HEVC_KEEP_THREADS_ACTIVE": "0"},
                         hevc_thread_environment(8, False))
        self.assertEqual("1", hevc_thread_environment(8, True)["JCE_DBG_HEVC_KEEP_THREADS_ACTIVE"])
        self.assertTrue(hevc_diagnostics("HEVC worker cores=8 keep_threads_active=0", "hvc1", 8, False)["requested_match"])
        self.assertFalse(hevc_diagnostics("HEVC worker cores=8 keep_threads_active=1", "hvc1", 8, False)["requested_match"])
        self.assertIsNone(hevc_diagnostics("HEVC worker cores=8 keep_threads_active=1", "hvc1")["requested_match"])
        for codec, log in (("hvc1", ""), ("av01", "HEVC worker cores=8 keep_threads_active=0")):
            with self.assertRaises(ValueError):
                hevc_diagnostics(log, codec, 8, False)

    def test_cli_controls_are_mutually_exclusive(self):
        import argparse
        import contextlib
        import io
        from editor_media_suite import add_hevc_thread_options
        parser = argparse.ArgumentParser()
        add_hevc_thread_options(parser)
        self.assertIsNone(parser.parse_args([]).hevc_keep_threads_active)
        self.assertIs(parser.parse_args(["--hevc-keep-threads-idle"]).hevc_keep_threads_active, False)
        self.assertIs(parser.parse_args(["--hevc-keep-threads-active"]).hevc_keep_threads_active, True)
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                parser.parse_args(["--hevc-keep-threads-active", "--hevc-keep-threads-idle"])
