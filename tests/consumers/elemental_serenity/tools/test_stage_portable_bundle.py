import json
from pathlib import Path
import tempfile
import unittest
from stage_portable_bundle import stage,export_command
class StageTests(unittest.TestCase):
 def test_excludes_fonts_unused_scripts_private_and_preserves_original(self):
  with tempfile.TemporaryDirectory() as d:
   source=Path(d)/'source';source.mkdir()
   for rel in ['fonts/unlicensed.ttf','scripts/es_director_portable.lua','scripts/native.jcec','scene_ai/private.txt','scenes/elemental_serenity.bundle.scene.json']:
    p=source/rel;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('{}')
   roots={'assets':['fonts/unlicensed.ttf','render_settings.json']};(source/'bundle_roots.json').write_text(json.dumps(roots))
   out=Path(d)/'out';manifest=stage(source,out,10000)
   self.assertFalse((out/'fonts').exists());self.assertFalse((out/'scene_ai').exists());self.assertFalse((out/'scripts/native.jcec').exists())
   self.assertEqual(json.loads((source/'bundle_roots.json').read_text()),roots)
   self.assertEqual(json.loads((out/'bundle_roots.json').read_text())['assets'],['render_settings.json'])
   with self.assertRaises(ValueError):stage(source,Path(d)/'tiny',1)
 def test_mute_is_only_transformed_staging_content(self):
  with tempfile.TemporaryDirectory() as d:
   source=Path(d)/'source';(source/'scripts').mkdir(parents=True);(source/'scenes').mkdir()
   script='local MASTER = 0.55\nfunction script.on_start(self)\njce.play_sound("audio/sounds/ui_interactions/click.mp3", 0.25)\nend\n'
   (source/'scripts/es_director_portable.lua').write_text(script)
   scene={'entities':[{'components':[{'type':'AudioSource','volume':1}]}]}
   (source/'scenes/elemental_serenity.bundle.scene.json').write_text(json.dumps(scene))
   out=Path(d)/'out';stage(source,out,10000,True)
   self.assertIn('ES_QA_CONTENT_MUTE',(out/'scripts/es_director_portable.lua').read_text())
   self.assertNotIn('click.mp3", 0.25',(out/'scripts/es_director_portable.lua').read_text())
   self.assertEqual(json.loads((out/'scenes/elemental_serenity.bundle.scene.json').read_text())['entities'][0]['components'][0]['volume'],0)
   self.assertEqual((source/'scripts/es_director_portable.lua').read_text(),script)
 def test_selected_cli_path_is_absolute_and_exists_independent_of_cwd(self):
  with tempfile.TemporaryDirectory() as d:
   assets=Path(d)/'resources';selected=assets/'scenes/elemental_serenity.bundle.scene.json';selected.parent.mkdir(parents=True);selected.write_text('{}')
   cmd=export_command(Path(d)/'packer',assets,Path(d)/'out','windows')
   self.assertEqual(Path(cmd[cmd.index('--scene')+1]),selected.resolve())
   self.assertTrue(Path(cmd[cmd.index('--scene')+1]).is_absolute())
   self.assertIn('--cook-assets',cmd);self.assertIn('--single-file',cmd)
   selected.unlink()
   with self.assertRaises(ValueError):export_command(Path(d)/'packer',assets,Path(d)/'out','windows')
if __name__=='__main__':unittest.main()
