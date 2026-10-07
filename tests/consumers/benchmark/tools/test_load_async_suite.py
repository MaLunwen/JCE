import unittest
from load_async_suite import validate_async

class AsyncLoadControls(unittest.TestCase):
    def test_actual_thread_and_timeline(self):
        log = ('benchmark-load-async-executor: mode=1 workers=2\n'
               'benchmark-load-async: request_frame=20 completion_frame=75 worker_main=0 '
               'main_thread=1 worker_thread=2 queue_ns=3 worker_wall_ns=100 '
               'poll_gap_ns=7 ready_elapsed_ns=115 submit_ns=5 ok=1')
        self.assertEqual(validate_async(log,dict(submit_ns=5))['completion_frame'],75)
        for bad in (log.replace('mode=1','mode=2'),log.replace('workers=2','workers=0'),
                    log.replace('worker_main=0','worker_main=1'),
                    log.replace('worker_thread=2','worker_thread=1'),
                    log.replace('ready_elapsed_ns=115','ready_elapsed_ns=114'),
                    log.replace('completion_frame=75','completion_frame=200'),
                    log.replace('ok=1','ok=0'),log+'\n'+log):
            with self.assertRaises(ValueError):validate_async(bad,dict(submit_ns=5))

if __name__ == '__main__':unittest.main()
