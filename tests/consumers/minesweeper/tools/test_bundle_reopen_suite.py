import unittest
from bundle_reopen_suite import validate_log,validate_scene,validate_background,interaction_fixture,validate_interaction

class BundleWitnessTests(unittest.TestCase):
    def test_pointer_fixture_buttons_and_actual_state_oracle(self):
        rows=interaction_fixture().splitlines()
        self.assertEqual(len(rows),12)
        self.assertEqual([int(r.split()[4]) for r in rows if r.split()[1]=="B" and r.split()[5]=="1"],[1,1,3,2])
        log="MINES_PORTABLE_START width=9 height=9 mines=10 seed=1\n"
        for b,i,f,r in [(1,40,0,54),(3,4,1,54),(2,13,1,55)]:
            log+=f"MINES_PORTABLE_ACTION button={b} index={i} state=1 flags={f} revealed={r}\n"
        log+="\n".join(f"Game event fixture frame={row.split()[0]} type={row.split()[1]}" for row in interaction_fixture().splitlines())
        validate_interaction(log)
        with self.assertRaises(ValueError):validate_interaction(log.replace("revealed=55","revealed=54"))

    def test_background_pixels_fail_transparent_sky_and_wrong_color(self):
        width,height=160,160
        correct=bytes((24,45,64))*width*height
        validate_background((width,height,3,correct))
        for pixel in [(128,180,230),(255,255,255),(0,0,0)]:
            with self.assertRaises(ValueError):validate_background((width,height,3,bytes(pixel)*width*height))

    def test_missing_boot_native_fallback_and_wrong_script_rejected(self):
        log="MINES_PORTABLE_BOOT Lua rules + pointer + Canvas\nMINES_DIRECTOR Lua started\nautoplay: entered Play"
        validate_log(log)
        for bad in (log.replace("MINES_PORTABLE_BOOT","missing"),log+"\n] ERROR - bad",log+"\nnative script module loaded: mines"):
            with self.assertRaises(ValueError):validate_log(bad)
        scene={"entities":[{"id":464,"name":"MinesController","components":[{"type":"Script","scriptPath":"scripts/MinesController.lua"}]}]}
        validate_scene(scene)
        scene["entities"][0]["components"].append({"type":"EditorMeta","name":"MinesController"})
        validate_scene({"scene":scene})
        scene["entities"].append(dict(scene["entities"][0]))
        with self.assertRaises(ValueError):validate_scene(scene)
        scene["entities"].pop()
        scene["entities"][0]["components"][0]["scriptPath"]="scripts/MinesController.jcec"
        with self.assertRaises(ValueError):validate_scene(scene)

class QuitAdmissionTests(unittest.TestCase):
    def test_exact_pre_stop_sequence_and_no_release_after_stop(self):
        from bundle_reopen_suite import interaction_fixture,validate_interaction
        prefix="MINES_PORTABLE_START width=9 height=9 mines=10 seed=1\n"
        for button,index,flags,revealed in [(1,40,0,54),(3,4,1,54),(2,13,1,55)]:
            prefix+=f"MINES_PORTABLE_ACTION button={button} index={index} state=1 flags={flags} revealed={revealed}\n"
        events=[f"Game event fixture frame={r.split()[0]} type={r.split()[1]}" for r in interaction_fixture(True).splitlines()]
        good=prefix+'\n'.join(events[:-1]);validate_interaction(good,True)
        for bad in (prefix+'\n'.join(events),prefix+'\n'.join(events[:-2]),good.replace('frame=208 type=M','frame=209 type=M')):
            with self.assertRaises(ValueError):validate_interaction(bad,True)

class EventTimingTests(unittest.TestCase):
    def test_motion_precedes_press_for_hover_refresh(self):
        rows=[line.split() for line in interaction_fixture(True).splitlines()]
        self.assertEqual(len(rows),18)
        for i in range(0,len(rows),3):
            motion,down,up=rows[i:i+3]
            self.assertEqual(motion[1],'M');self.assertEqual(down[1],'B')
            self.assertEqual(int(down[0])-int(motion[0]),2)
            self.assertEqual(int(up[0])-int(down[0]),2)
            self.assertEqual(motion[2:4],down[2:4])

class QuitControlTests(unittest.TestCase):
    def test_restoration_requires_actual_request_and_mutation(self):
        from bundle_reopen_suite import validate_quit
        a={'entities':[{'name':'Widget000','components':[{'type':'UIText','text':'authored'}]}]}
        b={'entities':[{'name':'Widget000','components':[{'type':'UIText','text':'runtime'}]}]}
        validate_quit('MINES_PORTABLE_QUIT requested',a,b,a)
        for log,before,during,after in [('',a,b,a),('MINES_PORTABLE_QUIT requested',a,a,a),('MINES_PORTABLE_QUIT requested',a,b,b)]:
            with self.assertRaises(ValueError):validate_quit(log,before,during,after)

if __name__=="__main__":unittest.main()
