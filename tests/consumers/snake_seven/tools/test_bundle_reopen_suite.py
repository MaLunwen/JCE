import unittest
from bundle_reopen_suite import validate
class WitnessTests(unittest.TestCase):
 def test_boot_or_flat_pixels_cannot_claim_gameplay(self):
  log='\n'.join('SNAKE_PORTABLE '+r+' Lua started' for r in ('manager','input','board','body','food','HUD'))+'\n'+'\n'.join(f'SNAKETRACE tick={i} state=1 score=0 len=3 head=15,{9-i}' for i in range(3))
  scene={'entities':[{'name':'GameState','components':[{'type':'Transform','posX':3}]}]}
  image=(160,160,3,bytes(i%256 for i in range(160*160*3)))
  validate(log,scene,image)
  for bad in [log.replace('body Lua started','body missing'),log.replace('state=1','state=0'),log+'] ERROR - script']:
   with self.assertRaises(ValueError):validate(bad,scene,image)
  with self.assertRaises(ValueError):validate(log,scene,(160,160,3,bytes(160*160*3)))
if __name__=='__main__':unittest.main()
