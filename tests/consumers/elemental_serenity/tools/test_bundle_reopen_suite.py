import copy
import unittest
from bundle_reopen_suite import validate
class PolicyWitnessTests(unittest.TestCase):
 def test_liveness_cannot_replace_rain_motion_or_mute(self):
  def entity(n,k,**v):return {'name':n,'components':[dict(type=k,**v)]}
  before={'entities':[entity('Ground','MeshRenderer',albedoTex='spring_night.png'),entity('flower_0','Transform',rotZ=0),entity('Firefly_0','Transform',posX=0),entity('CampfireLight','Light',intensity=4),entity('CampfireLight2','Light',intensity=2)]}
  after=copy.deepcopy(before);after['entities'][0]['components'][0]['albedoTex']='rainy_night.png';after['entities'][1]['components'][0]['rotZ']=1;after['entities'][2]['components'][0]['posX']=1
  for e in after['entities'][3:]:e['components'][0]['intensity']=0
  log='ES_QA_CONTENT_MUTE master_policy=0\nes_director online:\n'+'\n'.join('ES_PORTABLE '+r+' Lua started' for r in ('fireflies','campfire','flowers','surfaces'))
  image=(160,160,3,bytes(i%256 for i in range(160*160*3)))
  validate(log,before,after,image)
  for bad in [log.replace('master_policy=0','missing'),log.replace('surfaces Lua started','missing'),log+'] ERROR - fail']:
   with self.assertRaises(ValueError):validate(bad,before,after,image)
  with self.assertRaises(ValueError):validate(log,before,before,image)
  with self.assertRaises(ValueError):validate(log,before,after,(160,160,3,bytes(160*160*3)))
if __name__=='__main__':unittest.main()
