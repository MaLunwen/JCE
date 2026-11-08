"""Typed optional reader footer controls through the canonical trace owner."""
import copy
import hashlib
import contextlib
import io
import sys
from unittest import mock
import json
from pathlib import Path
import tempfile
import unittest
from tools import lifecycle_trace_report as trace
from tools.trace_reader_diagnostics import validate_reader_diagnostics as validate
from tools.tests.test_lifecycle_trace_report import capture


def snapshot(**changes):
    value=dict(write=3,earliest=1,next_before=1,next_after=4,count=3,stop=0,
               blocked=0,published=0,published_valid=False)
    value.update(changes)
    return value


def document():
    doc=capture()
    doc['jceTrace'].update(mode='continuous',consumer_mode='threaded',worker_available=True,
        write_ok=True,capture_complete=True,unexported=0,active_threads=0,
        active_work_items=0,active_waits=0,capacity=65536,
        reader_diagnostics=dict(version=1,clock_domain='process-perf-ns',read_calls=1,
            loss_reads=0,loss_events=0,stop_counts=[1,0,0,0,0],first_loss=None,
            last_loss=None,last_read=snapshot()))
    return doc


class ReaderDiagnosticsTests(unittest.TestCase):
    def test_optional_version_and_unknown_fields_are_not_silently_ignored(self):
        for key,value in [('version',2),('clock_domain','gpu'),('extra',0),('read_calls',True)]:
            doc=document();doc['jceTrace']['reader_diagnostics'][key]=value
            with self.subTest(key=key),self.assertRaisesRegex(ValueError,'reader diagnostic'):
                trace.analyze_trace(doc,['app.init'])

    def test_absent_legacy_and_valid_block_keep_canonical_authority(self):
        old=document();del old['jceTrace']['reader_diagnostics']
        trace.analyze_trace(old,['app.init'])
        new=document();before=copy.deepcopy(new)
        report=trace.analyze_trace(new,['app.init'])
        self.assertEqual(new,before)
        self.assertEqual(report['capture']['reader_diagnostics'],new['jceTrace']['reader_diagnostics'])
        with tempfile.TemporaryDirectory(prefix='jce-reader-diag-') as folder:
            path=Path(folder)/'trace.json';path.write_text(json.dumps(new),encoding='utf-8')
            report=trace.analyze_trace_file(path,['app.init'],history_db_path=Path(folder)/'history.sqlite',
                disk_history_max_records=10,disk_history_max_bytes=65536)
            self.assertTrue(report['validation_complete'])
            self.assertTrue(report['history_storage']['closed'])
            self.assertEqual(report['sha256'],report['source_sha256_after'])

    def test_all_stop_predicates_and_ahead_cursor(self):
        variants=[snapshot(),snapshot(write=0,earliest=0,next_before=0,next_after=0,count=0),
                  snapshot(next_before=100,next_after=100,count=0),
                  snapshot(write=600,next_after=513,count=512,stop=1),
                  snapshot(next_after=1,count=0,stop=2,blocked=1),
                  snapshot(next_after=2,count=1,stop=3,blocked=2,published=1,published_valid=True),
                  snapshot(write=0,earliest=0,next_before=99,next_after=99,count=0,stop=4)]
        for item in variants:
            meta=document()['jceTrace'];diag=meta['reader_diagnostics'];diag['last_read']=item
            diag['stop_counts']=[int(i==item['stop']) for i in range(5)]
            with self.subTest(stop=item['stop'],item=item):validate(meta)
        mutants=[(0,{'blocked':1}),(0,{'next_after':3}),(1,{'count':511}),
                 (1,{'next_after':601}),(2,{'published_valid':True}),
                 (2,{'blocked':2}),(3,{'published_valid':False}),
                 (3,{'published':2}),(4,{'count':1}),(4,{'next_after':100})]
        by_stop={item['stop']:item for item in variants}
        for stop,changes in mutants:
            meta=document()['jceTrace'];diag=meta['reader_diagnostics'];item=copy.deepcopy(by_stop[stop]);item.update(changes)
            diag['last_read']=item;diag['stop_counts']=[int(i==stop) for i in range(5)]
            with self.subTest(stop=stop,changes=changes),self.assertRaisesRegex(ValueError,'reader diagnostic'):validate(meta)

    def loss_metadata(self,two=False):
        meta=document()['jceTrace'];meta.update(capacity=4,lost=100,cursor_lost=100,recorded=103,capture_complete=False)
        item=snapshot(write=104,earliest=101,next_before=1,next_after=105,count=4)
        witness=dict(read=1,phase=0,clock_valid=True,observed_ns=(1<<63)+1,
                     loss_delta=100,lost_total=100,snapshot=item,previous=None)
        diag=meta['reader_diagnostics'];diag.update(loss_reads=1,loss_events=100,
            first_loss=witness,last_loss=copy.deepcopy(witness),last_read=copy.deepcopy(item))
        if two:
            prior=snapshot(write=105,earliest=102,next_before=105,next_after=106,count=1)
            last=snapshot(write=300,earliest=297,next_before=106,next_after=301,count=4)
            diag.update(read_calls=3,loss_reads=2,loss_events=291,stop_counts=[3,0,0,0,0],last_read=last,
                last_loss=dict(read=3,phase=2,clock_valid=True,observed_ns=(1<<63)+100,
                               loss_delta=191,lost_total=291,snapshot=copy.deepcopy(last),previous=prior))
            meta.update(lost=291,cursor_lost=291,recorded=294)
        return meta

    def test_exact_loss_witnesses_and_unknown_clock_do_not_promote_partial(self):
        for two in (False,True):
            meta=self.loss_metadata(two);before=copy.deepcopy(meta);validate(meta);self.assertEqual(meta,before)
            doc=capture();doc['jceTrace']=meta
            with self.assertRaisesRegex(ValueError,'failed capture_complete|completeness failed|incomplete'):
                trace.analyze_trace(doc,['app.init'])
        meta=self.loss_metadata()
        for field in ('first_loss','last_loss'):
            meta['reader_diagnostics'][field].update(clock_valid=False,observed_ns=0)
        validate(meta)
        meta['reader_diagnostics']['last_loss']['observed_ns']=1
        with self.assertRaisesRegex(ValueError,'clock'):validate(meta)
        meta=self.loss_metadata();meta['consumer_mode']='cooperative';meta['worker_available']=False
        for field in ('first_loss','last_loss'):meta['reader_diagnostics'][field]['phase']=1
        validate(meta)

    def test_zero_read_and_loss_accounting(self):
        meta=document()['jceTrace'];diag=meta['reader_diagnostics']
        diag.update(read_calls=0,stop_counts=[0]*5,last_read=None);validate(meta)
        for key,value in [('last_read',snapshot()),('loss_events',1),('loss_reads',1),('stop_counts',[1,0,0,0,0])]:
            bad=copy.deepcopy(meta);bad['reader_diagnostics'][key]=value
            with self.subTest(key=key),self.assertRaisesRegex(ValueError,'reader diagnostic'):validate(bad)
        for key,value in [('stop_counts',[0]*4),('stop_counts',[0]*5),('first_loss',{}),('loss_events',1)]:
            bad=document()['jceTrace'];bad['reader_diagnostics'][key]=value
            with self.subTest(key=key),self.assertRaisesRegex(ValueError,'reader diagnostic'):validate(bad)

    def test_loss_witness_order_previous_totals_and_types(self):
        mutations=[('read',0),('read',4),('phase',3),('phase',1),('loss_delta',0),
                   ('lost_total',290),('clock_valid',1),('observed_ns',1<<64),
                   ('previous',None),('snapshot',None),('extra',0)]
        for key,value in mutations:
            meta=self.loss_metadata(True);meta['reader_diagnostics']['last_loss'][key]=value
            with self.subTest(key=key),self.assertRaisesRegex(ValueError,'reader diagnostic'):validate(meta)
        for mutate in ('first_total','one_different','read_collision','clock_backwards','previous_collision','too_many_losses','phase_regression'):
            meta=self.loss_metadata(True);diag=meta['reader_diagnostics']
            if mutate=='first_total':diag['first_loss']['lost_total']+=1
            elif mutate=='one_different':diag['loss_reads']=1
            elif mutate=='read_collision':diag['last_loss']['read']=1
            elif mutate=='clock_backwards':diag['last_loss']['observed_ns']=1
            elif mutate=='previous_collision':
                diag['last_loss']['read']=2
                diag['last_loss']['previous']=snapshot(write=4,earliest=1,next_after=5,count=4)
            elif mutate=='phase_regression':
                diag['first_loss']['phase']=2;diag['last_loss']['phase']=0
            else:diag['loss_reads']=4
            with self.subTest(mutate=mutate),self.assertRaisesRegex(ValueError,'reader diagnostic'):validate(meta)

    def test_every_structural_field_unsigned_bounds_and_capacity(self):
        for key in ('write','earliest','next_before','next_after','count','stop','blocked','published'):
            for value in (-1,True,1<<32):
                meta=document()['jceTrace'];meta['reader_diagnostics']['last_read'][key]=value
                with self.subTest(key=key,value=value),self.assertRaisesRegex(ValueError,'reader diagnostic'):validate(meta)
        for key in ('read_calls','loss_reads','loss_events'):
            for value in (-1,True,1<<64):
                meta=document()['jceTrace'];meta['reader_diagnostics'][key]=value
                with self.subTest(key=key,value=value),self.assertRaisesRegex(ValueError,'reader diagnostic'):validate(meta)
        for field,value in [('capacity',0),('capacity',True),('mode','resident')]:
            meta=document()['jceTrace'];meta[field]=value
            with self.assertRaisesRegex(ValueError,'reader diagnostic'):validate(meta)
        for key,value in [('write',(1<<32)-1),('earliest',2),('count',513),('published_valid',0),('extra',0)]:
            meta=document()['jceTrace'];meta['reader_diagnostics']['last_read'][key]=value
            with self.subTest(key=key),self.assertRaisesRegex(ValueError,'reader diagnostic'):validate(meta)

    def test_file_cli_failure_retains_complete_fingerprints(self):
        doc=document();doc['jceTrace']['reader_diagnostics']['version']=2
        with tempfile.TemporaryDirectory(prefix='jce-reader-invalid-') as folder:
            path=Path(folder)/'trace.json';out=Path(folder)/'report.json';path.write_text(json.dumps(doc),encoding='utf-8')
            with self.assertRaises(trace.TraceFileError) as caught:trace.analyze_trace_file(path,['app.init'])
            evidence=caught.exception.evidence
            self.assertTrue(evidence['json_utf8_eof_validated'])
            self.assertEqual(evidence['sha256'],hashlib.sha256(path.read_bytes()).hexdigest())
            self.assertEqual(evidence['sha256'],evidence['source_sha256_after'])
            with mock.patch.object(sys,'argv',['lifecycle_trace_report',str(path),'--require','app.init','--out',str(out)]),contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(trace.main(),1)
            result=json.loads(out.read_text());self.assertFalse(result['validation_complete'])
            self.assertEqual(result['status'],'INVALID_OR_INCOMPLETE_CAPTURE')

    def test_ack_uses_same_optional_validator_and_never_promotes_trace_failure(self):
        from tools import terminal_ack_observation as ack
        from tools.tests.test_terminal_ack_observation import trace_document,packet,encoded,TOKEN
        doc=trace_document();doc['jceTrace'].update(capacity=65536,
            reader_diagnostics=dict(version=1,clock_domain='process-perf-ns',read_calls=1,loss_reads=0,
                loss_events=0,stop_counts=[1,0,0,0,0],first_loss=None,last_loss=None,
                last_read=snapshot(write=9,next_after=10,count=9)))
        with tempfile.TemporaryDirectory(prefix='jce-reader-ack-') as folder:
            path=Path(folder)/'trace.json';stderr=Path(folder)/'stderr.log'
            stderr.write_bytes(encoded(packet(trace_status='complete',trace_published=True,trace_write_ok=True,cleanup_scope='10',log_file_close='api_ok')))
            process=dict(timed_out=False,returncode=0,stderr_path=str(stderr),process_identity=dict(pid=123,creation_time_unix_s=None,creation_time_source='synthetic-unavailable'))
            path.write_text(json.dumps(doc),encoding='utf-8')
            result=ack.observe_terminal_ack(process,TOKEN,expected_stderr_path=stderr,trace_path=path,return_trace_report=True)
            self.assertTrue(result['trace_report']['validation_complete'])
            doc['jceTrace']['reader_diagnostics']['version']=2;path.write_text(json.dumps(doc),encoding='utf-8')
            with self.assertRaisesRegex(trace.TraceFileError,'reader diagnostic'):
                ack.observe_terminal_ack(process,TOKEN,expected_stderr_path=stderr,trace_path=path)

    def test_retained_cursor_progress_and_loss_delta_are_exact(self):
        impossible=[snapshot(write=0,earliest=0,next_before=0,next_after=1,count=1),
                    snapshot(write=600,next_after=0,count=512,stop=1),
                    snapshot(next_before=0,next_after=0,count=0,stop=2,blocked=0),
                    snapshot(next_after=5,count=3)]
        for item in impossible:
            meta=document()['jceTrace'];diag=meta['reader_diagnostics'];diag['last_read']=item
            diag['stop_counts']=[int(i==item['stop']) for i in range(5)]
            with self.subTest(item=item),self.assertRaisesRegex(ValueError,'reader diagnostic'):validate(meta)
        for mutation in ('delta','invalid_args_loss','cursor_chain','write_backwards'):
            meta=self.loss_metadata(True);diag=meta['reader_diagnostics']
            if mutation=='delta':diag['last_loss']['loss_delta']-=1
            elif mutation=='invalid_args_loss':
                diag['first_loss']['snapshot']=snapshot(write=0,earliest=0,count=0,next_before=1,next_after=1,stop=4)
                diag['stop_counts']=[2,0,0,0,1]
            elif mutation=='cursor_chain':
                diag['last_loss']['previous']['next_before']=104
                diag['last_loss']['previous']['count']=2
            else:
                # A retained previous read cannot precede its prior reservation highwater.
                diag['last_loss']['previous'].update(write=103,earliest=100,next_before=105,next_after=105,count=0)
                diag['last_loss']['snapshot']['next_before']=105
                diag['last_loss']['loss_delta']=192
                diag['last_loss']['lost_total']=292
                diag['loss_events']=292;meta['lost']=meta['cursor_lost']=292
            with self.subTest(mutation=mutation),self.assertRaisesRegex(ValueError,'reader diagnostic'):validate(meta)

    def test_retained_derived_losses_cannot_contradict_aggregate_or_witness_range(self):
        meta=document()['jceTrace'];meta['reader_diagnostics']['last_read']['count']=2
        with self.assertRaisesRegex(ValueError,'retained cursor losses'):validate(meta)
        meta=self.loss_metadata(True);diag=meta['reader_diagnostics']
        diag.update(read_calls=4,stop_counts=[4,0,0,0,0],
            last_read=snapshot(write=400,earliest=397,next_before=301,next_after=401,count=4))
        with self.assertRaisesRegex(ValueError,'retained cursor losses|outside'):validate(meta)
        meta=self.loss_metadata(True);diag=meta['reader_diagnostics']
        # Every one of these three reads is retained; invented extra loss cannot hide.
        diag.update(loss_reads=3,loss_events=292);diag['last_loss']['lost_total']=292
        meta['lost']=meta['cursor_lost']=292
        with self.assertRaisesRegex(ValueError,'fully retained'):validate(meta)

    def test_nonadjacent_known_write_highwater_cannot_go_backwards(self):
        meta=self.loss_metadata();diag=meta['reader_diagnostics']
        diag.update(read_calls=10,stop_counts=[10,0,0,0,0],
            last_read=snapshot(write=103,earliest=100,next_before=105,next_after=105,count=0))
        with self.assertRaisesRegex(ValueError,'known snapshot write'):validate(meta)


if __name__=='__main__':unittest.main()
