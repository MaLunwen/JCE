/* One private actual-context cache. Query support and API admission are separate. */
// Augment link-time roots after consumer settings are loaded; preserve their list.
if (DEFAULT_LIBRARY_FUNCS_TO_INCLUDE.indexOf('$jceWebTerminalAckBootstrap') === -1)
  DEFAULT_LIBRARY_FUNCS_TO_INCLUDE.push('$jceWebTerminalAckBootstrap');
addToLibrary({
  // Explicit Web diagnostics reuse the existing Engine terminal owner and ENV.
  $jceWebTerminalAckBootstrap__deps: ['$ENV', '$addOnPreRun'],
  $jceWebTerminalAckBootstrap__postset: 'jceWebTerminalAckBootstrap();',
  $jceWebTerminalAckBootstrap: function() {
    if (typeof location === 'undefined' || !location.hash) return;
    var fragment = location.hash.slice(1);
    var tokens = new URLSearchParams(fragment).getAll('jce_terminal_ack');
    if (!tokens.length) return;
    var token = tokens[0];
    // Decode names to reject duplicate aliases, but admit only a raw canonical pair.
    if (tokens.length !== 1 || !/^[0-9a-f]{32}$/.test(token) ||
        fragment.split('&').indexOf('jce_terminal_ack=' + token) === -1)
      throw new Error('JCE Web terminal ACK requires one canonical 32-lowercase-hex fragment token');
    addOnPreRun(function() {
      if (Object.prototype.hasOwnProperty.call(ENV, 'JCE_TERMINAL_ACK_TOKEN'))
        throw new Error('JCE Web terminal ACK conflicts with an existing environment token');
      // This explicit diagnostic ticket survives ordinary and nested preRun writers.
      // It is enumerable for Emscripten's existing getEnvStrings owner.
      Object.defineProperty(ENV, 'JCE_TERMINAL_ACK_TOKEN', {
        enumerable: true,
        configurable: false,
        get: function() { return token; },
        set: function() {
          throw new Error('JCE Web terminal ACK conflicts with an existing environment token');
        }
      });
    });
  },
  $JceWebGlQueryContext: {context: null, extensions: 0, contextId: 0, epoch: 0,
    kind: 0, api: '', shading: '', renderer: '', canvas: null, lost: null, restored: null},
  $jceWebGlInvalidateContext__deps: ['$JceWebGlQueryContext'],
  $jceWebGlInvalidateContext: function() {
    var next = (JceWebGlQueryContext.epoch + 1) >>> 0;
    if (!next) next = 1;
    JceWebGlQueryContext.epoch = next;
    JceWebGlQueryContext.context = null;
    JceWebGlQueryContext.extensions = 0;
    JceWebGlQueryContext.contextId = 0;
    JceWebGlQueryContext.kind = 0;
    JceWebGlQueryContext.api = JceWebGlQueryContext.shading = JceWebGlQueryContext.renderer = '';
  },
  $jceWebGlObserveActualContext__deps: ['$GL', '$JceWebGlQueryContext', '$jceWebGlInvalidateContext'],
  $jceWebGlObserveActualContext: function() {
    var cache = JceWebGlQueryContext;
    if (!GLctx || !GL.currentContext || GLctx.isContextLost()) {
      if (JceWebGlQueryContext.context) jceWebGlInvalidateContext();
      return 0;
    }
    var kind = GL.currentContext.version, id = GL.currentContext.handle >>> 0;
    if (kind !== 1 && kind !== 2) {
      if (cache.context) jceWebGlInvalidateContext();
      return 0;
    }
    var changed = cache.context !== GLctx || cache.contextId !== id || cache.kind !== kind;
    if (changed) {
      jceWebGlInvalidateContext();
      if (cache.canvas) {
        cache.canvas.removeEventListener('webglcontextlost', cache.lost);
        cache.canvas.removeEventListener('webglcontextrestored', cache.restored);
      }
      cache.canvas = null; cache.lost = cache.restored = null;
      var canvas = GLctx.canvas;
      if (canvas && typeof canvas.addEventListener === 'function') {
        cache.canvas = canvas;
        cache.lost = function() {if (cache.canvas === canvas) jceWebGlInvalidateContext();};
        cache.restored = function() {if (cache.canvas === canvas) jceWebGlInvalidateContext();};
        canvas.addEventListener('webglcontextlost', cache.lost);
        canvas.addEventListener('webglcontextrestored', cache.restored);
      }
      cache.context = GLctx; cache.contextId = id; cache.kind = kind;
      cache.api = String(GLctx.getParameter(GLctx.VERSION) || '');
      cache.shading = String(GLctx.getParameter(GLctx.SHADING_LANGUAGE_VERSION) || '');
      cache.renderer = String(GLctx.getParameter(GLctx.RENDERER) || '');
      var flags = 0;
      if (kind === 2) {
        if (GLctx.getExtension('EXT_color_buffer_half_float')) flags |= 1;
        if (GLctx.getExtension('EXT_color_buffer_float')) flags |= 2;
        if (GLctx.getExtension('EXT_texture_norm16')) flags |= 4;
      }
      cache.extensions = flags;
    }
    return changed ? 2 : 1;
  },
  jce_webgl_observe_query_context__deps: ['$jceWebGlObserveActualContext', '$JceWebGlQueryContext', '$stringToUTF8'],
  jce_webgl_observe_query_context__sig: 'iiiiii',
  jce_webgl_observe_query_context: function(extensions, version, versionSize, renderer, rendererSize) {
    HEAPU32[extensions >> 2] = 0;
    var observed = jceWebGlObserveActualContext(), cache = JceWebGlQueryContext;
    if (!observed || cache.kind !== 2) return 0;
    HEAPU32[extensions >> 2] = cache.extensions;
    if (observed === 2) {
      stringToUTF8(cache.api, version, versionSize);
      stringToUTF8(cache.renderer, renderer, rendererSize);
    }
    return observed;
  },
  jce_webgl_read_context_snapshot__deps: ['$jceWebGlObserveActualContext', '$JceWebGlQueryContext', '$stringToUTF8'],
  jce_webgl_read_context_snapshot__sig: 'ii',
  jce_webgl_read_context_snapshot: function(out) {
    var observed = jceWebGlObserveActualContext(), cache = JceWebGlQueryContext;
    if (!observed) return 0;
    HEAPU32[out >> 2] = cache.contextId;
    HEAPU32[(out + 4) >> 2] = cache.epoch;
    HEAPU32[(out + 12) >> 2] = cache.kind;
    stringToUTF8(cache.api, out + 16, 192);
    stringToUTF8(cache.shading, out + 208, 192);
    stringToUTF8(cache.renderer, out + 400, 192);
    HEAPU32[(out + 8) >> 2] = cache.epoch;
    return 1;
  },
  jce_webgl_reset_context_cache__deps: ['$JceWebGlQueryContext', '$jceWebGlInvalidateContext'],
  jce_webgl_reset_context_cache__sig: 'v',
  jce_webgl_reset_context_cache: function() {
    var cache = JceWebGlQueryContext;
    jceWebGlInvalidateContext();
    if (cache.canvas) {
      cache.canvas.removeEventListener('webglcontextlost', cache.lost);
      cache.canvas.removeEventListener('webglcontextrestored', cache.restored);
    }
    cache.canvas = null; cache.lost = cache.restored = null;
  },
  jce_webgl_query_error__deps: ['$GL'],
  jce_webgl_query_error__sig: 'vi',
  jce_webgl_query_error: function(error) {GL.recordError(error);}
});
