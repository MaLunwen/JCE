import json
from pathlib import Path
import tempfile
import unittest
from service_fixture import generate
class ServiceFixtureTests(unittest.TestCase):
    def test_two_distinct_phases_preserve_template(self):
        source={"entities":[{"id":1,"components":[{"type":"Script","scriptPath":"old.lua"}]}]}
        with tempfile.TemporaryDirectory() as d:
            out=Path(d)/"new";generate(source,out)
            writer=(out/"writer/scripts/RuntimeServiceProbe.lua").read_text();reader=(out/"reader/scripts/RuntimeServiceProbe.lua").read_text()
            self.assertIn("stat_set('service_probe',123456)",writer);self.assertIn("stat_flush()",writer);self.assertIn("stat_get('service_probe',0)==123456",reader);self.assertNotIn('stat_set',reader)
            self.assertEqual(source['entities'][0]['components'][0]['scriptPath'],'old.lua')
            for key,value in [('bad;key',1),('good',2147483648)]:
                with self.assertRaises(ValueError):generate(source,Path(d)/'bad',key,value)
if __name__=='__main__':unittest.main()
