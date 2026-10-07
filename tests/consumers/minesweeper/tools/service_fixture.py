"""Generate test-owned public runtime statistics service fixtures; no gameplay win claim."""
import argparse
import copy
import json
from pathlib import Path

def generate(template,out,key="service_probe",value=123456):
    if not key.replace("_", "").isalnum() or len(key)>40 or not -2147483648<=value<=2147483647:
        raise ValueError("bounded stable key and signed32 value required")
    out.mkdir(parents=True,exist_ok=False)
    for phase in ("writer","reader"):
        root=out/phase; (root/"scripts").mkdir(parents=True);(root/"scenes").mkdir()
        scene=copy.deepcopy(template); body=scene.get("scene",scene)
        candidates=[e for e in body["entities"] if any(c.get("type")=="Script" for c in e.get("components",[]))]
        if not candidates:raise ValueError("template has no script entity")
        target=candidates[0]
        for e in body["entities"]:e["components"]=[c for c in e.get("components",[]) if c.get("type")!="Script"]
        target["components"].append({"type":"Script","scriptPath":"scripts/RuntimeServiceProbe.lua"})
        check=(f"assert(jce.stat_set('{key}',{value})==true);assert(jce.stat_flush()==true)" if phase=="writer"
               else f"assert(jce.stat_get('{key}',0)=={value})")
        script="local M={}\nfunction M:on_start()\n assert(jce.stat_available()==true,'statistics host unavailable')\n "+check+"\n jce.log('RUNTIME_SERVICE_PROBE "+phase+" PASS key="+key+" value="+str(value)+"')\nend\nreturn M\n"
        (root/"scripts/RuntimeServiceProbe.lua").write_text(script,encoding="utf-8",newline="\n")
        (root/"scenes/service.scene.json").write_text(json.dumps(scene,indent=2)+"\n",encoding="utf-8",newline="\n")
    (out/"SCOPE.md").write_text("Public statistics service writer/reader integration only. Not gameplay victory or input validation. Run two separate hosts with the same explicitly configured runtime statistics directory. Require actual writer and reader PASS markers, normal exit and no errors.\n",encoding="utf-8",newline="\n")

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("--scene-template",type=Path,required=True);p.add_argument("--out",type=Path,required=True);p.add_argument("--key",default="service_probe");p.add_argument("--value",type=int,default=123456);a=p.parse_args()
    generate(json.loads(a.scene_template.read_text(encoding="utf-8")),a.out,a.key,a.value)
if __name__=="__main__":main()
