"""Actual saved C/JS QA owner CPU ports; never browser, SDK or GPU proof."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT=Path(__file__).resolve().parents[1]


class UpdateCensusJavascriptControls(unittest.TestCase):
    def node(self,code,*args):
        node=shutil.which('node')
        if not node:self.skipTest('Node actual-owner CPU port unavailable')
        return subprocess.run([node,'-e',code,*map(str,args)],capture_output=True,text=True,timeout=30)

    def test_actual_em_js_bridge_memory_growth_types_and_ordinary_negatives(self):
        code=r"""
const fs=require('node:fs'),assert=require('node:assert/strict');
const source=fs.readFileSync(process.argv[1],'utf8');
const start='EM_JS(int,td_qa_bind_update_census,(const uint32_t *record),{';
assert.equal(source.split(start).length,2);
let body=source.slice(source.indexOf(start)+start.length);body=body.slice(0,body.indexOf('\n});'));
const variant=process.argv[2];
function replaceOnce(from,to){assert.equal(body.split(from).length,2);body=body.replace(from,to);}
if(variant==='cached_heap'){
 replaceOnce('var reader=function(out) {','var initialHeap=HEAPU32;\n    var reader=function(out) {');
 replaceOnce('var heap=HEAPU32, index=address>>>2;','var heap=initialHeap, index=address>>>2;');
}else if(variant==='missing_state'){
 replaceOnce('return out[0]===1 && out[1]!==0 && out[2]===1 && out[6]===0 && out[7]===0;','return true;');
}else if(variant==='missing_type'){
 replaceOnce('if (!(out instanceof Uint32Array) || out.length!==8 || (address&3)!==0) return false;',
             'if (!out || (address&3)!==0) return false;');
}
const bind=new Function('record',body);globalThis.Module={};globalThis.HEAPU32=new Uint32Array(64);
function record(values){HEAPU32.set(values,4);}
record([1,3,1,1,8,8,0,0]);assert.equal(bind(16),1);
const reader=Module.jceQaUpdateCensusRead,out=new Uint32Array(8);
assert.equal(reader(out),true);assert.deepEqual(Array.from(out),[1,3,1,1,8,8,0,0]);
assert.equal(bind(16),1);assert.equal(bind(20),0);assert.equal(Module.jceQaUpdateCensusRead,reader);
assert.equal(reader([]),false);assert.equal(reader(new Uint32Array(7)),false);assert.equal(reader(new Float64Array(8)),false);
assert.equal(reader(HEAPU32.subarray(4,12)),false);
const old=HEAPU32;globalThis.HEAPU32=new Uint32Array(128);record([1,3,1,1,12,12,0,0]);
assert.equal(reader(out),true);assert.equal(out[4],12);assert.equal(old[8],8);
for(const [word,value] of [[0,2],[1,0],[2,0],[6,1],[7,1]]){
 record([1,3,1,1,12,12,0,0]);HEAPU32[4+word]=value;assert.equal(reader(out),false);
}
record([1,4,1,0,0,0,0,0]);assert.equal(reader(out),true);assert.equal(out[3],0);
const foreign=()=>false;globalThis.Module={jceQaUpdateCensusRead:foreign};
assert.equal(bind(16),0);assert.equal(Module.jceQaUpdateCensusRead,foreign);
let getterCalls=0;globalThis.Module={};Object.defineProperty(Module,'jceQaUpdateCensusRead',{get:()=>{++getterCalls;return foreign;}});
assert.equal(bind(16),0);assert.equal(getterCalls,0);
console.log('Actual EM_JS owner CPU ports PASS; no Web compile or GPU');
"""
        header=ROOT/'src/qa_update_census.inc.h'
        positive=self.node(code,header,'normal')
        self.assertEqual(positive.returncode,0,positive.stdout+positive.stderr)
        for variant in ('cached_heap','missing_state','missing_type'):
            with self.subTest(variant=variant):
                negative=self.node(code,header,variant)
                self.assertEqual(negative.returncode,1,negative.stdout+negative.stderr)
                self.assertIn('AssertionError',negative.stderr)

    def test_actual_saved_observer_samples_update_and_program_draw_ports(self):
        code=r"""
const fs=require('node:fs'),assert=require('node:assert/strict');
const tool=eval('('+fs.readFileSync(process.argv[1],'utf8')+')');let captured=[];
const download={saveAs:async()=>{}};
const downloader={isClosed:()=>false,waitForEvent:async()=>download,evaluate:async()=>{}};
const context=()=>({request:{get:async()=>({ok:()=>true,status:()=>200,body:async()=>Buffer.from('CPU PORT ONLY'),headers:()=>({})})},
 route:async()=>{},addInitScript:async(fn,args)=>{captured.push({fn,args});throw Error('CPU ports: no browser/GPU');},
 close:async()=>{},newPage:async()=>downloader});
const browser={version:()=> 'CPU PORT',newContext:async()=>context(),close:async()=>{}};
const page={context:()=>({browser:()=>({browserType:()=>({launch:async()=>browser})})})};
const identity='a'.repeat(64),config={reference_commit:'784129a685627a18f3d3f8fa594208000d9313e8',protocol:'v3-work-consumed',
 run_id:'CPU_ONLY',output_dir:'D:/CPU_ONLY',jce_url:'http://127.0.0.1:8785/mock.html',source_url:'http://127.0.0.1:8770/mock.html',
 jce_artifact_sha256:identity,source_html_sha256:identity,source_application_sha256:identity,source_three_sha256:identity,source_probe_sha256:identity};
const csource=fs.readFileSync(process.argv[2],'utf8'),start='EM_JS(int,td_qa_bind_update_census,(const uint32_t *record),{';
let bridge=csource.slice(csource.indexOf(start)+start.length);bridge=bridge.slice(0,bridge.indexOf('\n});'));
(async()=>{
 const report=await tool(page,config,async()=>identity);
 assert.equal(report.status,'UNAVAILABLE');assert.equal(report.final_claims_allowed,false);assert.equal(captured.length,28);
 function scenario(host,mode){
  const owner=captured.find(row=>row.args.host===host&&row.args.observerMode===mode);assert(owner);
  const queue=[];let queries=0,draw=true,consume=true,generation=3,normal=true;
  globalThis.window=globalThis;globalThis.requestAnimationFrame=fn=>{queue.push(fn);return 1;};
  globalThis.performance={now:()=>0};globalThis.TowerDefense={counter:0};globalThis.ENV={};
  const program={},foreign={},shader={};
  const gl={TRIANGLES:4,SHADER_TYPE:1,FRAGMENT_SHADER:2,LINK_STATUS:3,COMPILE_STATUS:4,drawingBufferWidth:1280,drawingBufferHeight:720,
   getExtension:()=>null,getParameter:()=>{++queries;return 'CPU PORT';},getContextAttributes:()=>({}),
   getAttachedShaders:()=>{++queries;return [shader];},getShaderParameter:(shader,type)=>{++queries;return type===gl.SHADER_TYPE?2:true;},
   getShaderSource:()=>{++queries;return 'CPU SOURCE';},getProgramParameter:()=>{++queries;return true;},shaderSource:()=>{},
   useProgram:()=>{},linkProgram:()=>{},deleteProgram:()=>{},drawElements:()=>{},drawElementsInstanced:()=>{}};
  globalThis.HTMLCanvasElement=function(){this.width=1280;this.height=720;this.addEventListener=()=>{};};
  HTMLCanvasElement.prototype.getContext=function(){gl.canvas=this;return gl;};
  owner.fn(owner.args);const state=window.jcePerfObservation;
  globalThis.Module={preRun:[]};if(host==='jce')Module.preRun.forEach(fn=>fn());
  globalThis.HEAPU32=new Uint32Array(64);HEAPU32.set([1,generation,1,mode==='on'?1:0,0,0,0,0],4);
  if(host==='jce'){
   const bind=new Function('record',bridge);assert.equal(bind(16),1);
   Module._td_qa_update_census_snapshot=()=>16;
   state.readCensus=Module.jceQaUpdateCensusRead;state.censusGeneration=generation;
  }
  const canvas=new HTMLCanvasElement();canvas.getContext('webgl2');gl.shaderSource(shader,'CPU SOURCE');gl.linkProgram(program);
  function loop(){
   if(host==='source'){if(consume)++TowerDefense.counter;}
   else if(mode==='on'&&consume){HEAPU32[8]++;if(normal)HEAPU32[9]++;HEAPU32[5]=generation;}
   gl.useProgram(draw===2?foreign:program);if(draw)gl.drawElements(4,36,0,0);
   requestAnimationFrame(loop);
  }
  const tick=time=>{assert(queue.length);queue.shift()(time);};requestAnimationFrame(loop);tick(0);
  assert.equal(state.gameCallback,loop);state.programSources();state.selectPrograms([1]);state.stopSetup();
  state.phase='warmup';state.warmStart=0;state.resolve=()=>{state.done=true;};state.reject=()=>{state.failed=true;};
  const before=queries;
  return {state,tick,queries:()=>queries,before,gl,program,set:values=>{
    if('draw'in values)draw=values.draw;if('consume'in values)consume=values.consume;
    if('generation'in values)generation=values.generation;if('normal'in values)normal=values.normal;
  }};
 }
 for(const host of ['source','jce']){
  let probe=scenario(host,'on');probe.tick(3000);probe.tick(3010);
  assert.equal(probe.state.failed,undefined);assert.equal(probe.state.workSamples.length,2);
  assert(probe.state.workSamples.every(row=>row.world_draw_calls===1&&row.world_program_mask===2));
  assert.equal(probe.queries(),probe.before); // No sample query/readback.
  for(const control of [{consume:false},{draw:false},{draw:2},...(host==='jce'?[{normal:false},{generation:4}]:[])]){
   probe=scenario(host,'on');probe.set(control);probe.tick(3000);
   assert.equal(probe.state.failed,true);assert.equal(probe.state.workSamples.length,1);
  }
  probe=scenario(host,'on');probe.gl.linkProgram(probe.program);probe.tick(3000);assert.equal(probe.state.failed,true);
  probe=scenario(host,'off');probe.tick(3000);probe.tick(3010);
  assert.equal(probe.state.workSamples.length,0);assert.equal(probe.queries(),probe.before);
 }
 console.log('Actual saved observer CPU ports PASS: source/JCE kinds, missing update/return/draw, generation, relink, off control; no browser/GPU');
})().catch(error=>{console.error(error);process.exitCode=1;});
"""
        result=self.node(code,ROOT/'tools/verify_web_performance.js',ROOT/'src/qa_update_census.inc.h')
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('Actual saved observer CPU ports PASS',result.stdout)


if __name__=='__main__':unittest.main()
