'use strict';
// Complete actual JS owner, counted context/heap/listener/error ports, no GPU.
const assert = require('node:assert/strict'), fs = require('node:fs'), vm = require('node:vm');
const bytes = new Uint8Array(2048), words = new Uint32Array(bytes.buffer), errors = [];
let library;
const bootstrapRoot = '$jceWebTerminalAckBootstrap', retainedRoots = ['$consumerStartup'];
const ports = {DEFAULT_LIBRARY_FUNCS_TO_INCLUDE:retainedRoots, GLctx:null, GL:{currentContext:null,recordError(error){errors.push(error);}}, HEAPU32:words,
 stringToUTF8(text,pointer,capacity){const b=Buffer.from(text);const n=Math.min(b.length,capacity-1);bytes.set(b.subarray(0,n),pointer);bytes[pointer+n]=0;},
 addToLibrary(value){library=value;}};
const source = fs.readFileSync(process.argv[2],'utf8');
vm.runInNewContext(source,ports,{filename:process.argv[2]});
assert.deepEqual(retainedRoots, ['$consumerStartup', bootstrapRoot], 'bootstrap root preserves consumer roots');
// Link-time library evaluation augments the caller's array once, without replacing it.
for (const prior of [[], ['$consumerStartup'], ['$consumerStartup', bootstrapRoot],
    ['$consumerStartup', '$consumerStartup']]) {
  const roots = [...prior];
  const compiler = {DEFAULT_LIBRARY_FUNCS_TO_INCLUDE:roots, addToLibrary() {}};
  vm.runInNewContext(source, compiler, {filename:process.argv[2]});
  vm.runInNewContext(source, compiler, {filename:process.argv[2]});
  assert.equal(compiler.DEFAULT_LIBRARY_FUNCS_TO_INCLUDE, roots);
  assert.deepEqual(roots, prior.includes(bootstrapRoot) ? prior : [...prior, bootstrapRoot]);
}

ports.JceWebGlQueryContext=library.$JceWebGlQueryContext;
ports.jceWebGlInvalidateContext=library.$jceWebGlInvalidateContext;
ports.jceWebGlObserveActualContext=library.$jceWebGlObserveActualContext;
function textAt(p){let end=p;while(bytes[end])++end;return Buffer.from(bytes.subarray(p,end)).toString();}
function query(){return library.jce_webgl_observe_query_context(4,64,192,256,192);}
function snapshot(){return library.jce_webgl_read_context_snapshot(640);}
function context(enabled,api='WebGL 2.0 (OpenGL ES 3.0 port)',shading='WebGL GLSL ES 3.00',renderer='renderer port') {
 const listeners=new Map();return {lost:false,VERSION:1,SHADING_LANGUAGE_VERSION:2,RENDERER:3,extensions:[],parameters:[],
 canvas:{adds:0,removes:0,addEventListener(n,f){this.adds++;listeners.set(n,f);},removeEventListener(n,f){this.removes++;if(listeners.get(n)===f)listeners.delete(n);},emit(n){if(listeners.has(n))listeners.get(n)();},count(){return listeners.size;}},
 isContextLost(){return this.lost;},getExtension(n){this.extensions.push(n);return enabled.includes(n)?{}:null;},
 getParameter(n){this.parameters.push(n);return n===1?api:n===2?shading:renderer;}};
}
assert.equal(query(),0);assert.equal(snapshot(),0);
const one=context([], 'WebGL 1.0 (OpenGL ES 2.0 port)', 'WebGL GLSL ES 1.00');
ports.GLctx=one;ports.GL.currentContext={version:1,handle:11};
assert.equal(query(),0);assert.equal(snapshot(),1);assert.equal(words[163],1);
assert.equal(textAt(656),'WebGL 1.0 (OpenGL ES 2.0 port)');assert.equal(one.extensions.length,0);
const first=context(['EXT_color_buffer_float','EXT_color_buffer_half_float']);
ports.GLctx=first;ports.GL.currentContext={version:2,handle:12};
assert.equal(query(),2);assert.equal(words[1],3);assert.equal(snapshot(),1);
assert.equal(words[160],12);assert(words[161]>0);assert.equal(words[161],words[162]);
assert.equal(textAt(656),'WebGL 2.0 (OpenGL ES 3.0 port)');assert.equal(textAt(848),'WebGL GLSL ES 3.00');
assert.equal(first.parameters.length,3);assert.equal(first.extensions.length,3);
assert.equal(query(),1);assert.equal(snapshot(),1);assert.equal(first.parameters.length,3);
let epoch=words[161];first.lost=true;assert.equal(query(),0);assert.equal(snapshot(),0);assert.equal(words[1],0);
first.lost=false;assert.equal(query(),2);assert.equal(snapshot(),1);assert.notEqual(words[161],epoch);
assert.equal(first.parameters.length,6);assert.equal(first.extensions.length,6);
// Lost and restored events between reads still invalidate the same JS object.
epoch=words[161];first.canvas.emit('webglcontextlost');first.canvas.emit('webglcontextrestored');
assert.equal(snapshot(),1);assert.notEqual(words[161],epoch);assert.equal(first.parameters.length,9);
assert.equal(first.canvas.count(),2);
// A new actual handle cannot inherit metadata even if the JS object is reused.
epoch=words[161];ports.GL.currentContext.handle=13;assert.equal(snapshot(),1);
assert.equal(words[160],13);assert.notEqual(words[161],epoch);assert.equal(first.canvas.count(),2);
const second=context(['EXT_texture_norm16'],'WebGL 2.0 (OpenGL ES 3.0 second)','WebGL GLSL ES 3.00 second','second renderer');
ports.GLctx=second;ports.GL.currentContext.handle=14;assert.equal(query(),2);assert.equal(words[1],4);
assert.equal(first.canvas.count(),0);assert.equal(snapshot(),1);assert.equal(textAt(1040),'second renderer');
assert.equal(second.parameters.length,3);
library.jce_webgl_query_error(0x500);assert.deepEqual(errors,[0x500]);
epoch=words[161];library.jce_webgl_reset_context_cache();assert.equal(second.canvas.count(),0);
assert.equal(snapshot(),1);assert.notEqual(words[161],epoch);assert.equal(second.canvas.count(),2);
ports.GLctx=null;assert.equal(query(),0);assert.equal(snapshot(),0);assert.equal(words[1],0);
library.jce_webgl_reset_context_cache();assert.equal(second.canvas.count(),0);
// The actual library postset registers the existing Emscripten preRun queue.
// These ports model queue order and getenv input, not a browser or Engine tail.
function terminalBoot(hash, environment = {}, options = {}) {
  const queue = [], order = [], writes = [], defines = [];
  const values = {...environment};
  const env = new Proxy(values, {
    set(target, name, value) {target[name] = value; writes.push(name); return true;},
    defineProperty(target, name, descriptor) {
      Object.defineProperty(target, name, descriptor); defines.push(name); return true;
    }
  });
  const priorToken = values.JCE_TERMINAL_ACK_TOKEN;
  const host = () => {
    order.push('host');
    assert.equal(values.JCE_TERMINAL_ACK_TOKEN, options.hostSees === undefined ? priorToken : options.hostSees);
    if (Object.prototype.hasOwnProperty.call(options, 'hostToken'))
      env.JCE_TERMINAL_ACK_TOKEN = options.hostToken;
    if (Object.prototype.hasOwnProperty.call(options, 'hostNestedToken'))
      scope.addOnPreRun(() => {order.push('nested'); env.JCE_TERMINAL_ACK_TOKEN = options.hostNestedToken;});

  };
  const priorCallbacks = options.functionCallback ? host : [host];
  const printErr = () => {throw new Error('bootstrap must not replace or invoke stderr');};
  const module = {preRun: priorCallbacks, printErr};
  let actual;
  const scope = {ENV:env, Module:module, URLSearchParams, DEFAULT_LIBRARY_FUNCS_TO_INCLUDE:[],
    addOnPreRun(callback) {queue.push(callback);}, addToLibrary(value) {actual=value;}};
  if (!options.noLocation) scope.location = {hash, search:options.search || ''};
  const context = vm.createContext(scope);
  vm.runInContext(source, context, {filename:process.argv[2]});
  scope.jceWebTerminalAckBootstrap = actual.$jceWebTerminalAckBootstrap;
  assert.deepEqual(Array.from(actual.$jceWebTerminalAckBootstrap__deps), ['$ENV', '$addOnPreRun']);
  vm.runInContext(actual.$jceWebTerminalAckBootstrap__postset, context);
  assert.equal(module.preRun, priorCallbacks);
  assert.equal(module.printErr, printErr);
  return {env, values, queue, order, writes, defines, run() {
    const callbacks = typeof module.preRun === 'function' ? [module.preRun] : module.preRun;
    for (const callback of callbacks) queue.push(callback);
    while (queue.length) queue.shift()(module);
    order.push('main');
    assert.equal(values.JCE_TERMINAL_ACK_TOKEN, options.token, 'terminal token reaches main');
  }};
}
const token = '0123456789abcdef0123456789abcdef';
for (const [hash, options] of [ ['', {}], ['#', {}], ['#scene=overview', {}],
    ['', {search:'?jce_terminal_ack=' + token}], ['', {noLocation:true}] ]) {
  const boot = terminalBoot(hash, {KEEP:'unchanged'}, options);
  assert.equal(boot.queue.length, 0);
  assert.deepEqual(boot.writes, []); assert.deepEqual(boot.defines, []);
  assert.deepEqual(boot.values, {KEEP:'unchanged'});
  boot.run(); assert.deepEqual(boot.order, ['host', 'main']);
}
for (const functionCallback of [false, true]) {
  const boot = terminalBoot('#jce_terminal_ack=' + token, {KEEP:'unchanged'}, {token, hostSees:token, functionCallback});
  assert.equal(boot.queue.length, 1);
  assert.deepEqual(boot.writes, []);
  boot.run();
  assert.deepEqual(boot.order, ['host', 'main']);
  assert.deepEqual(boot.writes, []);
  assert.deepEqual(boot.defines, ['JCE_TERMINAL_ACK_TOKEN']);
  assert.equal(Object.prototype.hasOwnProperty.call(boot.env, 'JCE_TERMINAL_ACK_TOKEN'), true);
  const envStrings = [];
  for (const name in boot.env) envStrings.push(name + '=' + boot.env[name]);
  assert.deepEqual(envStrings, ['KEEP=unchanged', 'JCE_TERMINAL_ACK_TOKEN=' + token]);
  boot.env.KEEP = 'still mutable';
  assert.equal(boot.values.KEEP, 'still mutable');
  assert.throws(() => Object.defineProperty(boot.env, 'JCE_TERMINAL_ACK_TOKEN', {value:'other'}), TypeError);
  assert.throws(() => {delete boot.env.JCE_TERMINAL_ACK_TOKEN;}, TypeError);
}
const surrounded = terminalBoot('#scene=overview&jce_terminal_ack=' + token + '&view=1', {}, {token, hostSees:token});
surrounded.run();
for (const value of ['', '0'.repeat(31), '0'.repeat(33), 'A'.repeat(32), 'g'.repeat(32),
    token + '+', token + '%00', token + '=ignored']) {
  assert.throws(() => terminalBoot('#jce_terminal_ack=' + value), /canonical 32-lowercase-hex/);
}
for (const hash of [ '#jce_terminal_ack', '#jce_terminal_ack=' + token + '&jce_terminal_ack=' + token,
    '#jce_terminal_ack=' + token + '&jce_terminal_ack=',
    '#%6ace_terminal_ack=' + token, '#jce_terminal_ack=%30' + token.slice(1),
    '#jce_terminal_ack=' + token + '&%6ace_terminal_ack=' + token ]) {
  assert.throws(() => terminalBoot(hash), /canonical 32-lowercase-hex/);
}
for (const prior of [token, 'different', '', undefined, null]) {
  const boot = terminalBoot('#jce_terminal_ack=' + token, {JCE_TERMINAL_ACK_TOKEN:prior});
  assert.throws(() => boot.run(), /conflicts with an existing environment token/);
  assert.deepEqual(boot.order, []); assert.deepEqual(boot.writes, []);
  assert.deepEqual(boot.defines, []);
  assert.equal(boot.values.JCE_TERMINAL_ACK_TOKEN, prior);
}
for (const hostToken of [token, 'different', '', undefined, null]) {
  const boot = terminalBoot('#jce_terminal_ack=' + token, {}, {hostToken, hostSees:token});
  assert.throws(() => boot.run(), /conflicts with an existing environment token/);
  assert.deepEqual(boot.order, ['host']);
  assert.deepEqual(boot.writes, []); assert.deepEqual(boot.defines, ['JCE_TERMINAL_ACK_TOKEN']);
  assert.equal(boot.values.JCE_TERMINAL_ACK_TOKEN, token);
}
for (const hostNestedToken of [token, 'different']) {
  const boot = terminalBoot('#jce_terminal_ack=' + token, {}, {hostNestedToken, token, hostSees:token});
  assert.throws(() => boot.run(), /conflicts with an existing environment token/);
  assert.deepEqual(boot.order, ['host', 'nested']);
  assert.deepEqual(boot.writes, []); assert.deepEqual(boot.defines, ['JCE_TERMINAL_ACK_TOKEN']);
  assert.equal(boot.values.JCE_TERMINAL_ACK_TOKEN, token);
}
const hostOnly = terminalBoot('', {}, {hostToken:token, token});
hostOnly.run(); assert.deepEqual(hostOnly.writes, ['JCE_TERMINAL_ACK_TOKEN']);
const unchanged = terminalBoot('', {JCE_TERMINAL_ACK_TOKEN:token}, {token});
unchanged.run(); assert.deepEqual(unchanged.writes, []); assert.deepEqual(unchanged.defines, []);
console.log('actual Web interop CPU ports PASS: context snapshots and fragment terminal bootstrap before main');
