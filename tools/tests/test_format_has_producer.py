"""CPU controls for direct and explicit shared format-producer ownership."""
import importlib.util
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('format_producer',ROOT/'tools/lint/check_format_has_producer.py')
gate=importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class ProducerControls(unittest.TestCase):
    def test_direct_and_explicit_shared_sources(self):
        leaf='jce_terrain_format.c'
        direct='add_executable(jce_cook "'+leaf+'")'
        self.assertIn(leaf,gate.producer_sources(direct,'jce_cook'))
        root='add_executable(jce_cook cli.c)\ninclude("${CMAKE_CURRENT_SOURCE_DIR}/cmake/JCEHostAssetSources.cmake")\njce_host_asset_sources(jce_cook)'
        helper='function(jce_host_asset_sources target)\ntarget_sources(${target} PRIVATE "'+leaf+'")\nendfunction()'
        self.assertIn(leaf,gate.producer_sources(root,'jce_cook',helper))
        for cmake, sources in ((root.replace('jce_host_asset_sources(jce_cook)',''),helper),
                (root,helper.replace(leaf,'other.c')),
                (root,helper.replace(leaf,'other.c')+'\nset(unrelated "'+leaf+'")'),
                (root.replace('include(', '# include('),helper),
                (root.replace('jce_host_asset_sources(jce_cook)','jce_host_asset_sources(other)'),helper)):
            self.assertNotIn(leaf,gate.producer_sources(cmake,'jce_cook',sources))


if __name__=='__main__':unittest.main()
