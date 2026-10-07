import unittest
from service_reopen_suite import validate_phase
class ServiceReopenTests(unittest.TestCase):
    def test_actual_marker_process_and_backend(self):
        log='jce_renderer: renderer: Direct3D 11\nRUNTIME_SERVICE_PROBE writer PASS key=service_probe value=123456'
        proc={'process':{'returncode':0,'timed_out':False},'backend_actual':'Direct3D 11'}
        validate_phase(log,proc,'writer','service_probe',123456,'d3d11')
        for bad in [log.replace('123456','123455'),log+'\n] ERROR - flush failed',log.replace('Direct3D 11','missing')]:
            with self.assertRaises((ValueError,RuntimeError)):validate_phase(bad,proc,'writer','service_probe',123456,'d3d11')
        for badproc in [{'process':{'returncode':1,'timed_out':False},'backend_actual':'Direct3D 11'},{'process':{'returncode':0,'timed_out':True},'backend_actual':'Direct3D 11'}]:
            with self.assertRaises((ValueError,RuntimeError)):validate_phase(log,badproc,'writer','service_probe',123456,'d3d11')
if __name__=='__main__':unittest.main()
