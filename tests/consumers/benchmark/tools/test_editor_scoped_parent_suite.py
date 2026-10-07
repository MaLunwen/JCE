import copy
import unittest
from editor_scoped_parent_suite import validate_states, validate_events

class ScopedParentTests(unittest.TestCase):
    def test_actual_scoped_parent_state_negative_controls(self):
        root={"entities":[{"id":1,"name":"Node0","components":[{"type":"Transform","posX":2}]},
            {"id":2,"name":"Node1","components":[{"type":"Transform","posX":10,"scaleX":2}]},
            {"id":3,"name":"Node2","parentId":1,"components":[{"type":"Transform","posY":1}]},
            {"id":4,"name":"Duplicate","components":[{"type":"Transform","posX":17}]},
            {"id":5,"name":"Duplicate","components":[{"type":"Transform","posX":29}]}]}
        after=copy.deepcopy(root);after["entities"][0].update(parentId=2);after["entities"][0]["components"][0]["posX"]=-4
        documents=[root,after,copy.deepcopy(root),copy.deepcopy(after)]
        validate_states(documents,5)
        for index, field, value in ((2,"parentId",2),(3,"id",6)):
            bad=copy.deepcopy(documents);bad[index]["entities"][0][field]=value
            with self.assertRaises(ValueError):validate_states(bad,5)
        bad=copy.deepcopy(documents);bad[2]["entities"][0]["components"][0]["posX"]=99
        with self.assertRaises(ValueError):validate_states(bad,5)
        bad=copy.deepcopy(documents);bad[3]["entities"][2]["parentId"]=2
        with self.assertRaises(ValueError):validate_states(bad,5)

    def test_missing_duplicate_or_unrelated_state_is_rejected(self):
        root={"entities":[{"id":i+1,"name":name,"components":[{"type":"Transform","posX":i}]} for i,name in enumerate(("Node0","Node1","Node2","Duplicate","Duplicate","Unrelated"))]}
        root["entities"][2]["parentId"]=1
        after=copy.deepcopy(root);after["entities"][0]["parentId"]=2
        documents=[root,after,copy.deepcopy(root),copy.deepcopy(after)]
        validate_states(documents,6)
        for index, field, value in ((2,"parentId",999),(2,"id",2)):
            bad=copy.deepcopy(documents);bad[index]["entities"][0][field]=value
            with self.assertRaises(ValueError):validate_states(bad,6)
        bad=copy.deepcopy(documents);bad[3]["entities"][-1]["components"][0]["posX"]=99
        with self.assertRaises(ValueError):validate_states(bad,6)
        with self.assertRaises(ValueError):validate_states(documents[:3],6)

    def test_actual_command_and_inspector_identity_guards(self):
        text="\n".join(["editor-command: operation=reparent entity=456 frame=80 correct=1 parent=457 name=Node0", "history-entity-restore: token=1 source_id=456 current_id=456 reason=undo name=Node0 parent=0", "history-entity-restore: token=1 source_id=456 current_id=456 reason=redo name=Node0 parent=457", "inspector-content-observation: frame=150 entity=456 valid=1 display_name=Node0"])
        events = validate_events(text)
        # Previous process evidence is JSON: current evidence must survive the same round trip.
        import json
        self.assertEqual(events, json.loads(json.dumps(events)))
        for bad in (text.replace("correct=1","correct=0"),text.replace("reason=redo name=Node0 parent=457","reason=redo name=Node0 parent=458"),text.replace("token=1 source_id=456 current_id=456 reason=redo","token=2 source_id=456 current_id=456 reason=redo"),text+"\n"+text.splitlines()[-1]):
            with self.assertRaises(ValueError):validate_events(bad)

if __name__=="__main__":unittest.main()
