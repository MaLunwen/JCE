async (page, config, hashValue) => {
    // Saved formal initial-board observer. This never replaces either game loop.
    const assert = (ok, message) => { if (!ok) throw new Error(message); };
    assert(typeof hashValue === 'function', 'Shared verify_web_hash.js function argument required');
    const sha256 = value => hashValue(page, value, new URL('/__jce_qa_hash_' + config.run_id, config.jce_url).href);
    const pin = '784129a685627a18f3d3f8fa594208000d9313e8';
    assert(config && config.reference_commit === pin, 'The immutable reference pin is required');
    for (const key of ['jce_artifact_sha256', 'source_html_sha256', 'source_application_sha256',
        'source_three_sha256', 'source_probe_sha256']) {
        assert(/^[a-f0-9]{64}$/.test(config[key] || ''), 'Missing explicit identity: ' + key);
    }
    assert(/^[A-Za-z0-9_-]{1,64}$/.test(config.run_id || ''), 'Invalid run_id');
    assert(/^(?:[A-Za-z]:[\\/]|\/)/.test(config.output_dir || ''), 'output_dir must be absolute');
    const endpoints = {source: new URL(config.source_url), jce: new URL(config.jce_url)};
    for (const url of Object.values(endpoints)) {
        assert((url.protocol === 'http:' || url.protocol === 'https:') && !url.search && !url.hash,
            'Endpoints must be actual HTTP HTML URLs without query/hash');
    }
    const protocols = {
        standard: {id: 'td-web-initial-standard-v1', repeats: 3, warmup_ms: 2500,
            sample_ms: 5000, min_sample_intervals: 2, sample_max_ms: 30000},
        publication: {id: 'td-web-initial-publication-v2', repeats: 7, warmup_ms: 3000,
            sample_ms: 5000, min_sample_intervals: 600, sample_max_ms: 30000},
        'v3-work-consumed': {id: 'td-web-initial-publication-v3-work-consumed', repeats: 7, warmup_ms: 3000,
            sample_ms: 5000, min_sample_intervals: 600, sample_max_ms: 30000, work_observer: true}
    };
    const protocolName = config.protocol === undefined ? 'publication' : config.protocol;
    assert(typeof protocolName === 'string' && Object.prototype.hasOwnProperty.call(protocols, protocolName),
        'Unknown explicit Web timing protocol');
    const protocol = {...protocols[protocolName]};
    const workProtocol = protocol.work_observer === true;
    const warmup = protocol.warmup_ms, sample = protocol.sample_ms;
    const order = Array.from({length: protocol.repeats * (workProtocol ? 4 : 2)}, (_, index) => index % 2 === 0 ? 'source' : 'jce');
    const timeout = config.timeout_ms || 60000;
    assert(Number.isInteger(timeout) && timeout >= 10000 && timeout <= 120000, 'Invalid timeout_ms');
    const errors = [], runs = [], downloads = [];
    const report = {schema: workProtocol ? 2 : 1, status: 'NOT_RUN', complete: false, final_claims_allowed: false,
        generated_utc: new Date().toISOString(), reference_commit: pin, protocol,
        jce_artifact_sha256: config.jce_artifact_sha256, runs, errors,
        conditions: config.conditions || {contended: null},
        requested: {viewport: [1280, 720], dpr: 1, warmup_ms: warmup, sample_ms: sample,
            min_sample_intervals: protocol.min_sample_intervals, sample_max_ms: protocol.sample_max_ms, order},
        method: 'Actual game RAF callback entry intervals and callback CPU wall duration; neither GPU time nor uncapped throughput',
        instrumentation: ['The same requestAnimationFrame observer surrounds each original callback without replacing scheduling/render/update order.',
            workProtocol ? 'WebGL metadata/program source capture runs only during setup; on rows retain program/draw counters, off rows restore all GL wrappers.' :
                'WebGL metadata/36-index world callback identification run only during setup; all GL wrappers are restored before warmup.',
            'No GL queries, PNG, readbacks, trace, KPI export or screenshot occurs during warmup/sample.',
            'The source compatibility copy retains its disclosed 50 ms Object.observe fallback.',
            'The owned fixture probe performs initial setup/readback before READY; native render/Date.now/Math.random are then restored.'],
        limits: ['RAF intervals measure browser presentation scheduling, not completed GPU work.',
            'Refresh-capped FPS is not uncapped engine throughput or cross-platform commercial-engine parity.',
            'Caller scheduling evidence must independently exclude other CPU/GPU workloads.']};
    if (workProtocol) {
        report.method = 'Same-window callback work census and raw RAF/CPU clocks; no CPU submission or GPU completion getter';
        report.instrumentation.push('Each repeat retains off/off/on/on source/JCE observer controls on identical per-host HTTP artifacts.',
            'On rows count only actual 36-index TRIANGLES draws using linked program identities admitted during setup; no sample GL query/readback.',
            'JCE on rows read the public consumer-owned Emscripten census before/after each callback; disabled rows retain branch overhead.',
            'Census reads and witness construction are outside callback CPU spans; RAF intervals include bookkeeping. Keep both raw metrics; do not subtract cost.');
        report.submission_observation = {available: false, reason: 'No same-callback CPU submission getter in this consumer protocol'};
    }
    let quality = {available: false, reason: 'No current-artifact actual input quality evidence supplied'};
    if (config.jce_quality_evidence_json !== undefined) {
        try {
            assert(typeof config.jce_quality_evidence_json === 'string' &&
                await sha256(config.jce_quality_evidence_json) === config.jce_quality_evidence_sha256,
                'Input quality evidence bytes changed');
            const evidence = JSON.parse(config.jce_quality_evidence_json);
            assert(evidence.artifact_sha256 === config.jce_artifact_sha256, 'Input quality evidence artifact mismatch');
            assert(evidence.errors && evidence.errors.length === 0, 'Input quality evidence has errors/unavailable status');
            assert(typeof config.jce_quality_snapshot_json === 'string' &&
                await sha256(config.jce_quality_snapshot_json) === config.jce_quality_snapshot_sha256,
                'Preparsed quality snapshot bytes changed');
            const parsed = JSON.parse(config.jce_quality_snapshot_json);
            assert(parsed.available === true && parsed.artifact_sha256 === config.jce_artifact_sha256 &&
                parsed.evidence_sha256 === config.jce_quality_evidence_sha256, 'Shared parser evidence identity mismatch');
            quality = {available: true, effective: parsed.effective, protocol: parsed.protocol,
                source: config.jce_quality_evidence_path || 'Caller-provided actual input evidence',
                sha256: config.jce_quality_evidence_sha256, artifact_sha256: evidence.artifact_sha256,
                snapshot_path: config.jce_quality_snapshot_path || 'Caller-provided shared parser output',
                snapshot_sha256: config.jce_quality_snapshot_sha256,
                scope: 'Actual bootstrap render settings parsed by the shared QA helper from separate same-artifact input; no timing-window GL query'};
        } catch (error) { quality = {available: false, reason: String(error)}; }
    }
    report.jce_effective_quality = quality;
    // This is separate from bootstrap JSON: the final validator rereads the
    // original linked-source/uniform witness using its single QA authority.
    let renderQuality = {available: false, reason: 'No current-artifact actual WebGL quality evidence supplied'};
    if (config.jce_render_quality_evidence_json !== undefined) {
        try {
            assert(typeof config.jce_render_quality_evidence_json === 'string' &&
                await sha256(config.jce_render_quality_evidence_json) === config.jce_render_quality_evidence_sha256,
                'Actual WebGL quality evidence bytes changed');
            assert(/^(?:[A-Za-z]:[\\/]|\/)/.test(config.jce_render_quality_evidence_path || ''),
                'Actual WebGL quality evidence path must be absolute');
            const observed = JSON.parse(config.jce_render_quality_evidence_json);
            assert(observed.schema === 1 && observed.status === 'ACTUAL_WEB_GROUND_QUALITY_OBSERVED' &&
                Array.isArray(observed.errors) && observed.errors.length === 0,
                'Actual WebGL quality evidence is failed/incomplete');
            for (const key of ['artifact_sha256', 'served_artifact_sha256', 'executed_artifact_sha256']) {
                assert(observed[key] === config.jce_artifact_sha256, 'Actual WebGL quality artifact differs: ' + key);
            }
            renderQuality = {available: true, source: config.jce_render_quality_evidence_path,
                sha256: config.jce_render_quality_evidence_sha256, artifact_sha256: config.jce_artifact_sha256,
                observed, scope: 'Separate actual linked WebGL source and submitted ground uniforms; final raw-file validation required'};
        } catch (error) { renderQuality = {available: false, reason: String(error)}; }
    }
    report.jce_render_quality = renderQuality;
    const browser = await page.context().browser().browserType().launch({
        channel: config.browser_channel || 'chrome', headless: true,
        args: ['--enable-gpu', '--use-angle=d3d11', '--disable-background-timer-throttling',
            '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows']
    });
    let lastPage = null, lastContext = null, adapter = null;
    function statistics(values) {
        assert(values.length > 0 && values.every(value => Number.isFinite(value) && value >= 0), 'Invalid actual samples');
        const ordered = [...values].sort((a, b) => a - b);
        const percentile = p => ordered[Math.min(ordered.length - 1, Math.ceil(ordered.length * p) - 1)];
        return {samples: values.length, mean: values.reduce((a, b) => a + b, 0) / values.length,
            min: ordered[0], median: percentile(0.5), p95: percentile(0.95), p99: percentile(0.99), max: ordered.slice(-1)[0]};
    }
    try {
        report.browser_version = browser.version();
        for (let index = 0; index < order.length; ++index) {
            // Close the previous page/context before creating the next renderer.
            if (lastPage) { await lastPage.close(); lastPage = null; }
            if (lastContext) { await lastContext.close(); lastContext = null; }
            const host = order[index];
            const observerMode = workProtocol && index % 4 >= 2 ? 'on' : 'off';
            const row = {host, repeat: Math.floor(index / (workProtocol ? 4 : 2)), order: index, status: 'NOT_RUN', errors: [], logs: [], inputs: {}};
            if (workProtocol) row.observer_mode = observerMode;
            runs.push(row);
            const context = lastContext = await browser.newContext({viewport: {width: 1280, height: 720},
                deviceScaleFactor: 1, acceptDownloads: true, serviceWorkers: 'block'});
            try {
                const html = await context.request.get(endpoints[host].href, {maxRedirects: 0});
                assert(html.ok(), host + ' HTML HTTP status ' + html.status());
                row.inputs.html_sha256 = await sha256(await html.body());
                assert(row.inputs.html_sha256 === (host === 'jce' ? config.jce_artifact_sha256 : config.source_html_sha256),
                    host + ' served HTML identity changed');
                if (host === 'source') {
                    const paths = {application: 'js/application.js', three: 'js/lib/three.min.js', probe: 'reference_probe.js'};
                    for (const [kind, relative] of Object.entries(paths)) {
                        const url = new URL(relative, endpoints.source);
                        const response = await context.request.get(url.href, {maxRedirects: 0});
                        assert(response.ok(), 'Source ' + kind + ' unavailable');
                        const bytes = await response.body();
                        row.inputs[kind + '_sha256'] = await sha256(bytes);
                        assert(row.inputs[kind + '_sha256'] === config['source_' + kind + '_sha256'], 'Source ' + kind + ' bytes changed');
                        const headers = {...response.headers()};
                        delete headers['content-encoding'];
                        headers['content-length'] = String(bytes.length);
                        if (kind === 'application' || kind === 'three') {
                            await context.route(url.href, async route => { await route.fulfill({response, headers, body: bytes}); });
                        } else {
                            // Only the first-party QA probe receives this prefix.
                            // The original application bytes remain unchanged.
                            const prefix = 'window.jcePerfSourceRestore={render:render,dateNow:Date.now,random:Math.random};\n';
                            await context.route(url.href, async route => {
                                const body = prefix + bytes.toString('utf8');
                                await route.fulfill({response, headers: {...headers, 'content-length': String(Buffer.byteLength(body))}, body});
                            });
                        }
                    }
                    await context.route(new URL('evidence', endpoints.source).href, async route => {
                        assert(route.request().method() === 'POST', 'Unexpected reference evidence method');
                        row.setup_probe_posts = (row.setup_probe_posts || 0) + 1;
                        await route.fulfill({status: 200, contentType: 'text/plain', body: 'QA SETUP OBSERVED'});
                    });
                }
                await context.addInitScript(({host, protocol, observerMode}) => {
                    const work = protocol.work_observer === true, observedWork = work && observerMode === 'on';
                    const nativeRAF = window.requestAnimationFrame;
                    const state = window.jcePerfObservation = {phase: 'setup', currentCallback: null,
                        gameCallback: null, sawWorld: false, worldFrames: 0, ambiguous: false,
                        contexts: [], contextObservers: [], patches: [], workPatches: [], timestamps: [], cpu: [], measuredCallbacks: 0,
                        candidates: [], selectedPrograms: new Set(), workSamples: [], setupStopped: false, worldProgramChanged: false,
                        censusBefore: new Uint32Array(8), censusAfter: new Uint32Array(8)};
                    state.censusValid = value => value[0] === 1 && value[1] > 0 && value[2] === 1 && value[3] === 1 && value[6] === 0 && value[7] === 0;
                    window.requestAnimationFrame = function (callback, ...args) {
                        return nativeRAF.call(this, function (timestamp) {
                            const setup = state.phase === 'setup';
                            if (setup) { state.currentCallback = callback; state.sawWorld = false; }
                            const game = callback === state.gameCallback;
                            if (game && state.phase === 'warmup' && timestamp - state.warmStart >= protocol.warmup_ms) {
                                state.phase = 'sample'; state.sampleStart = timestamp;
                            }
                            const observing = game && (state.phase === 'warmup' || state.phase === 'sample');
                            const sampled = game && state.phase === 'sample';
                            const auditing = observedWork && observing;
                            let workError = null, sourceBefore = null;
                            if (auditing) {
                                state.currentCallback = callback; state.currentWorldDraws = 0; state.currentWorldMask = 0;
                                if (host === 'jce') {
                                    if (!state.readCensus || !state.readCensus(state.censusBefore) || !state.censusValid(state.censusBefore))
                                        workError = 'Actual JCE update census before callback is unavailable/invalid';
                                } else sourceBefore = TowerDefense.counter;
                            }
                            const begin = sampled ? performance.now() : 0;
                            try { return callback.apply(this, arguments); }
                            finally {
                                const cpu = sampled ? performance.now() - begin : 0;
                                if (auditing) {
                                    const witness = {update_kind: host === 'jce' ? 'td_census' : 'source_counter',
                                        timestamp_ms: timestamp, world_draw_calls: state.currentWorldDraws,
                                        world_program_mask: state.currentWorldMask};
                                    if (host === 'jce') {
                                        if (!state.readCensus || !state.readCensus(state.censusAfter) || !state.censusValid(state.censusAfter))
                                            workError = 'Actual JCE update census after callback is unavailable/invalid';
                                        witness.census_before = Array.from(state.censusBefore);
                                        witness.census_after = Array.from(state.censusAfter);
                                        if (state.censusBefore[1] !== state.censusGeneration || state.censusAfter[1] !== state.censusBefore[1] ||
                                            state.censusAfter[4] - state.censusBefore[4] !== 1 || state.censusAfter[5] - state.censusBefore[5] !== 1)
                                            workError = 'Actual JCE on_update was missing, reset or did not return normally once';
                                    } else {
                                        witness.source_update_before = sourceBefore; witness.source_update_after = TowerDefense.counter;
                                        if (!Number.isSafeInteger(sourceBefore) || !Number.isSafeInteger(TowerDefense.counter) || TowerDefense.counter - sourceBefore !== 1)
                                            workError = 'Actual source update was not consumed once';
                                    }
                                    if (state.currentWorldDraws < 1 || state.worldProgramChanged)
                                        workError = 'Actual setup-identified world draw missing or linked program changed';
                                    if (sampled) state.workSamples.push(witness);
                                    state.currentCallback = null;
                                }
                                if (setup && state.sawWorld) {
                                    if (!state.gameCallback) state.gameCallback = callback;
                                    else if (state.gameCallback !== callback) state.ambiguous = true;
                                    ++state.worldFrames;
                                }
                                if (setup) state.currentCallback = null;
                                if (observing) ++state.measuredCallbacks;
                                if (sampled) {
                                    state.timestamps.push(timestamp); state.cpu.push(cpu);
                                    const elapsed = timestamp - state.sampleStart;
                                    if (workError) {
                                        state.phase = 'unavailable'; state.reject(new Error(workError));
                                    } else if (elapsed > protocol.sample_max_ms) {
                                        state.phase = 'unavailable';
                                        state.reject(new Error('Actual game sample exceeded its bounded realtime window'));
                                    } else if (elapsed >= protocol.sample_ms && state.timestamps.length - 1 >= protocol.min_sample_intervals) {
                                        state.phase = 'complete';
                                        if (host === 'source') state.sourceCounterAfter = TowerDefense.counter;
                                        state.resolve();
                                    }
                                } else if (workError) { state.phase = 'unavailable'; state.reject(new Error(workError)); }
                            }
                        }, ...args);
                    };
                    const originalGetContext = HTMLCanvasElement.prototype.getContext;
                    const seen = new WeakSet();
                    HTMLCanvasElement.prototype.getContext = function (...args) {
                        const gl = originalGetContext.apply(this, args);
                        if (!gl || !/^(webgl2?|experimental-webgl)$/.test(String(args[0])) || seen.has(gl)) return gl;
                        seen.add(gl);
                        const debug = gl.getExtension('WEBGL_debug_renderer_info');
                        const record = {kind: String(args[0]), version: gl.getParameter(gl.VERSION),
                            shading_language: gl.getParameter(gl.SHADING_LANGUAGE_VERSION), attributes: gl.getContextAttributes(),
                            vendor: debug ? gl.getParameter(debug.UNMASKED_VENDOR_WEBGL) : null,
                            renderer: debug ? gl.getParameter(debug.UNMASKED_RENDERER_WEBGL) : null,
                            canvas_on_creation: [gl.canvas.width, gl.canvas.height], context_lost: false, caps: {}};
                        for (const cap of ['MAX_TEXTURE_SIZE', 'MAX_RENDERBUFFER_SIZE', 'MAX_VERTEX_ATTRIBS',
                            'MAX_VERTEX_UNIFORM_VECTORS', 'MAX_TEXTURE_IMAGE_UNITS', 'MAX_COMBINED_TEXTURE_IMAGE_UNITS', 'MAX_SAMPLES']) {
                            if (typeof gl[cap] === 'number') record.caps[cap] = gl.getParameter(gl[cap]);
                        }
                        state.contexts.push(record);
                        gl.canvas.addEventListener('webglcontextlost', () => { record.context_lost = true; });
                        state.contextObservers.push(() => {
                            record.canvas = [gl.canvas.width, gl.canvas.height];
                            record.drawing_buffer = [gl.drawingBufferWidth, gl.drawingBufferHeight];
                        });
                        let activeProgram = null;
                        const shaderSources = new WeakMap(), linkedPrograms = new WeakMap();
                        if (work) {
                            const originalSource = gl.shaderSource;
                            const sourceWrapper = function(shader, source) {
                                const value = originalSource.apply(this, arguments);
                                shaderSources.set(shader, String(source)); return value;
                            };
                            gl.shaderSource = sourceWrapper;
                            state.patches.push(() => { if (gl.shaderSource === sourceWrapper) gl.shaderSource = originalSource; });
                            const original = gl.useProgram;
                            const wrapper = function(program) { const value = original.apply(this, arguments); activeProgram = program; return value; };
                            gl.useProgram = wrapper;
                            state.workPatches.push(() => { if (gl.useProgram === wrapper) gl.useProgram = original; });
                            for (const name of ['linkProgram', 'deleteProgram']) {
                                const original = gl[name];
                                const wrapper = function(program) {
                                    // bgfx detaches shaders after linking: capture only at real setup link.
                                    const setupLink = name === 'linkProgram' && state.phase === 'setup' && !state.setupStopped;
                                    const shaders = setupLink ? gl.getAttachedShaders(program) || [] : [];
                                    const value = original.apply(this, arguments);
                                    if (setupLink) {
                                        const fragment = shaders.find(shader => gl.getShaderParameter(shader, gl.SHADER_TYPE) === gl.FRAGMENT_SHADER);
                                        linkedPrograms.set(program, {fragment_source: fragment ? shaderSources.get(fragment) : '',
                                            fragment_compiled: fragment ? gl.getShaderParameter(fragment, gl.COMPILE_STATUS) : false,
                                            linked: gl.getProgramParameter(program, gl.LINK_STATUS)});
                                    }
                                    if (state.setupStopped && state.selectedPrograms.has(program)) state.worldProgramChanged = true;
                                    return value;
                                };
                                gl[name] = wrapper;
                                state.workPatches.push(() => { if (gl[name] === wrapper) gl[name] = original; });
                            }
                        }
                        for (const name of ['drawElements', 'drawElementsInstanced']) {
                            if (typeof gl[name] !== 'function') continue;
                            const original = gl[name];
                            const wrapper = function (...values) {
                                const value = original.apply(this, values);
                                if (values[1] === 36 && state.currentCallback) {
                                    if (state.phase === 'setup' && !state.setupStopped) {
                                        state.sawWorld = true;
                                        if (work && activeProgram && values[0] === gl.TRIANGLES &&
                                            (name !== 'drawElementsInstanced' || values[4] > 0) &&
                                            !state.candidates.some(row => row.program === activeProgram)) {
                                            if (state.candidates.length >= 16) state.ambiguous = true;
                                            else state.candidates.push({id: state.candidates.length + 1, program: activeProgram,
                                                linked: linkedPrograms.get(activeProgram)});
                                        }
                                    } else if (observedWork && (state.phase === 'warmup' || state.phase === 'sample') && values[0] === gl.TRIANGLES &&
                                        (name !== 'drawElementsInstanced' || values[4] > 0) && state.selectedPrograms.has(activeProgram)) {
                                        ++state.currentWorldDraws;
                                        const row = state.candidates.find(row => row.program === activeProgram);
                                        state.currentWorldMask |= 1 << row.id;
                                    }
                                }
                                return value;
                            };
                            gl[name] = wrapper;
                            (work ? state.workPatches : state.patches).push(() => { if (gl[name] === wrapper) gl[name] = original; });
                        }
                        return gl;
                    };
                    state.programSources = () => state.candidates.map(row => ({id: row.id,
                        linked: row.linked ? row.linked.linked : false,
                        fragment_compiled: row.linked ? row.linked.fragment_compiled : false,
                        fragment_source: row.linked ? row.linked.fragment_source : ''}));
                    state.selectPrograms = ids => {
                        if (!ids.length || ids.some(id => !state.candidates.some(row => row.id === id))) throw new Error('Setup world programs unavailable');
                        state.candidates.forEach(row => { if (ids.includes(row.id)) state.selectedPrograms.add(row.program); });
                    };
                    state.stopSetup = () => {
                        state.contextObservers.forEach(observe => observe());
                        state.patches.forEach(restore => restore()); state.patches = [];
                        if (!observedWork) { state.workPatches.forEach(restore => restore()); state.workPatches = []; }
                        state.setupStopped = true;
                        HTMLCanvasElement.prototype.getContext = originalGetContext;
                    };
                    if (host !== 'jce') return;
                    let moduleValue;
                    const seenModules = new WeakSet();
                    Object.defineProperty(window, 'Module', {configurable: true,
                        get() { return moduleValue; }, set(value) {
                            moduleValue = value;
                            if (!value || typeof value !== 'object' || seenModules.has(value)) return;
                            seenModules.add(value);
                            const previous = value.preRun;
                            value.preRun = Array.isArray(previous) ? previous : typeof previous === 'function' ? [previous] : [];
                            value.preRun.push(function () {
                                if (typeof ENV === 'undefined') throw new Error('Emscripten ENV unavailable');
                                ENV.JCE_TRACE = '0'; ENV.JCE_PERF_LOG = '0';
                                if (observedWork) ENV.TD_UPDATE_CENSUS = '1'; else delete ENV.TD_UPDATE_CENSUS;
                                state.workEnvironment = work ? {update_census: observedWork ? 'on' : 'off'} : null;
                                for (const key of ['JCE_TRACE_STREAM', 'JCE_TRACE_EXPORT', 'JCE_KPI_FRAME_LOG', 'JCE_MAX_FRAMES',
                                    'TD_TRACE', 'TD_TIMING', 'TD_TIMING_START', 'TD_LEGACY_FRAME_MODE']) delete ENV[key];
                                state.effective_environment = {JCE_TRACE: ENV.JCE_TRACE, JCE_PERF_LOG: ENV.JCE_PERF_LOG,
                                    trace_stream: false, trace_export: false, kpi: false, game_trace: false, game_timing: false};
                            });
                        }});
                }, {host, protocol, observerMode});
                const app = lastPage = await context.newPage();
                app.on('console', message => {
                    row.logs.push(message.text());
                    if (message.type() === 'error') row.errors.push({kind: 'console', message: message.text()});
                });
                app.on('pageerror', error => row.errors.push({kind: 'pageerror', message: String(error)}));
                app.on('requestfailed', request => row.errors.push({kind: 'requestfailed', url: request.url(),
                    message: request.failure() && request.failure().errorText}));
                const url = new URL(endpoints[host]); url.searchParams.set('fixture', 'initial');
                row.url = url.href;
                if (workProtocol) await context.route(url.href, async route => {
                    const response = await route.fetch({maxRedirects: 0});
                    assert(response.ok(), 'Executed HTML unavailable');
                    const bytes = await response.body(); row.inputs.executed_html_sha256 = await sha256(bytes);
                    assert(row.inputs.executed_html_sha256 === row.inputs.html_sha256, 'Executed/served timing artifact changed');
                    await route.fulfill({response, body: bytes});
                });
                await app.goto(url.href, {waitUntil: 'domcontentloaded', timeout});
                if (host === 'source') {
                    await app.waitForFunction(() => document.getElementById('probe-status') &&
                        /READY initial/.test(document.getElementById('probe-status').textContent), null, {timeout});
                    row.source_restore = await app.evaluate(() => {
                        const saved = window.jcePerfSourceRestore;
                        if (!saved || typeof saved.render !== 'function' || String(THREE.REVISION) !== '68')
                            throw new Error('Pinned normal render/Three68 unavailable');
                        if (TowerDefense.objects.some(object => object && (object.type === 'ENEMY' || object.type === 'BULLET')))
                            throw new Error('Initial fixture contains moving actors');
                        if (typeof gameRender === 'number') cancelAnimationFrame(gameRender);
                        render = saved.render; Date.now = saved.dateNow; Math.random = saved.random;
                        document.getElementById('probe').style.display = 'none';
                        render();
                        return {render_restored: render === saved.render, date_now_restored: Date.now === saved.dateNow,
                            random_restored: Math.random === saved.random, revision: THREE.REVISION,
                            scope: 'Original normal render schedules RAF, renders scene, then updates game; fixture clock is no longer used'};
                    });
                }
                await app.waitForFunction(() => window.jcePerfObservation.gameCallback &&
                    window.jcePerfObservation.worldFrames >= 20, null, {timeout});
                await app.evaluate(() => document.fonts.ready);
                if (workProtocol) {
                    const programs = await app.evaluate(() => window.jcePerfObservation.programSources());
                    const expected = host === 'jce' && renderQuality.available ?
                        new Set(renderQuality.observed.ground_draws.map(draw => draw.fragment_source.sha256)) : null;
                    if (host === 'jce') assert(expected && expected.size > 0, 'Same-artifact linked ground quality identity required for v3');
                    const selected = [];
                    for (const program of programs) {
                        assert(program.linked === true && program.fragment_compiled === true && program.fragment_source, 'Actual setup compiled/linked fragment source unavailable');
                        const identity = await sha256(program.fragment_source);
                        if (host === 'source' || expected.has(identity)) selected.push({id: program.id, fragment_sha256: identity,
                            fragment_bytes: new TextEncoder().encode(program.fragment_source).length});
                    }
                    assert(selected.length > 0, 'No setup world linked program matches actual quality identity');
                    await app.evaluate(ids => window.jcePerfObservation.selectPrograms(ids), selected.map(row => row.id));
                    row.world_programs = selected;
                }
                row.setup = await app.evaluate(({host, workProtocol, observerMode}) => {
                    const state = window.jcePerfObservation;
                    if (state.ambiguous || !state.gameCallback) throw new Error('Actual world game callback is ambiguous/unavailable');
                    if (host === 'source' && state.gameCallback !== window.jcePerfSourceRestore.render)
                        throw new Error('Source callback is not the restored original render');
                    if (workProtocol && host === 'jce') {
                        state.readCensus = Module.jceQaUpdateCensusRead;
                        if (typeof state.readCensus !== 'function' || typeof Module._td_qa_update_census_snapshot !== 'function' ||
                            Module._td_qa_update_census_snapshot() !== state.readCensus.jceTdCensusAddress ||
                            !state.readCensus(state.censusBefore)) throw new Error('Public consumer-owned Emscripten census binding unavailable');
                        const value = state.censusBefore;
                        if (value[3] !== (observerMode === 'on' ? 1 : 0)) throw new Error('Actual census enabled mode differs');
                        state.censusGeneration = value[1]; state.censusSetup = Array.from(value);
                    }
                    state.stopSetup();
                    const setup = {world_frames: state.worldFrames, callback_name: state.gameCallback.name || '<anonymous>',
                        context: state.contexts, viewport: [innerWidth, innerHeight], dpr: devicePixelRatio,
                        visibility: document.visibilityState, browser: navigator.userAgent,
                        effective_environment: state.effective_environment || null,
                        compatibility: host === 'source' ? {object_observe_type: typeof Object.observe,
                            observer_uses_interval: typeof Object.observe === 'function' && /setInterval/.test(String(Object.observe)),
                            declared_fallback_interval_ms: 50, scope: 'The compatibility shim remains part of the source workload'} : null,
                        quality: host === 'source' ? {available: true, settings: TowerDefense.settings,
                            gamma_input: TowerDefense.renderer.gammaInput, gamma_output: TowerDefense.renderer.gammaOutput,
                            shadow_enabled: TowerDefense.renderer.shadowMapEnabled, shadow_soft: TowerDefense.renderer.shadowMapSoft,
                            lights: TowerDefense.scene.children.filter(object => object instanceof THREE.DirectionalLight).map(light => ({
                                shadow_map_width: light.shadowMapWidth, shadow_map_height: light.shadowMapHeight,
                                shadow_bias: light.shadowBias, shadow_darkness: light.shadowDarkness, cast_shadow: light.castShadow}))} : null};
                    if (workProtocol && host === 'jce') {
                        setup.work_environment = state.workEnvironment; setup.census_setup = state.censusSetup;
                    }
                    return setup;
                }, {host, workProtocol, observerMode});
                assert(row.setup.viewport[0] === 1280 && row.setup.viewport[1] === 720 && row.setup.dpr === 1 &&
                    row.setup.visibility === 'visible', 'Viewport/DPR/visibility differs');
                assert(row.setup.context.length === 1 && row.setup.context[0].renderer && row.setup.context[0].vendor,
                    'Actual single graphics context/adapter unavailable');
                const graphics = row.setup.context[0];
                assert(graphics.canvas[0] === 1280 && graphics.canvas[1] === 720 &&
                    graphics.drawing_buffer[0] === 1280 && graphics.drawing_buffer[1] === 720 && !graphics.context_lost,
                    'Actual canvas/drawing buffer size or graphics liveness differs');
                const currentAdapter = graphics.vendor + '\n' + graphics.renderer;
                if (adapter === null) adapter = currentAdapter;
                assert(adapter === currentAdapter, 'Actual GPU adapter changed between runs');
                row.effective_quality = host === 'source' ? row.setup.quality : quality;
                if (host === 'jce' && renderQuality.available) {
                    const contexts = renderQuality.observed.contexts;
                    assert(Array.isArray(contexts) && contexts.length === 1 &&
                        ['kind', 'version', 'shading_language', 'vendor', 'renderer'].every(key =>
                            contexts[0][key] === graphics[key]),
                        'Actual WebGL quality GPU/context differs from the measured JCE context');
                }
                const raw = await app.evaluate(({host, timeout}) => new Promise((resolve, reject) => {
                    const state = window.jcePerfObservation;
                    state.timestamps = []; state.cpu = []; state.workSamples = []; state.measuredCallbacks = 0;
                    if (host === 'source') state.sourceCounterBefore = TowerDefense.counter;
                    state.warmStart = performance.now(); state.phase = 'warmup';
                    const timer = setTimeout(() => { state.phase = 'unavailable'; reject(new Error('Actual game RAF did not complete window')); }, timeout);
                    state.reject = error => { clearTimeout(timer); reject(error); };
                    state.resolve = () => {
                        clearTimeout(timer);
                        resolve({timestamps: state.timestamps, cpu: state.cpu, work_samples: state.workSamples, measured_callbacks: state.measuredCallbacks,
                            source_counter_before: state.sourceCounterBefore, source_counter_after: state.sourceCounterAfter,
                            warm_start: state.warmStart, sample_start: state.sampleStart});
                    };
                }), {host, timeout});
                assert(raw.timestamps.length > 2 && raw.timestamps.length === raw.cpu.length,
                    'Insufficient actual game callback observations');
                const intervals = raw.timestamps.slice(1).map((value, i) => value - raw.timestamps[i]);
                row.raw = {raf_timestamps_ms: raw.timestamps, callback_cpu_ms: raw.cpu, interval_ms: intervals,
                    warm_start_ms: raw.warm_start, sample_start_ms: raw.sample_start};
                assert(intervals.every(value => Number.isFinite(value) && value > 0), 'Duplicate/reversed actual game RAF timestamps');
                const elapsed = raw.timestamps.slice(-1)[0] - raw.timestamps[0];
                assert(elapsed >= sample && elapsed <= protocol.sample_max_ms && intervals.length >= protocol.min_sample_intervals,
                    'Actual sample window/count is short or exceeds its realtime bound');
                if (host === 'source') {
                    row.normal_update_observation = {before: raw.source_counter_before, after: raw.source_counter_after,
                        delta: raw.source_counter_after - raw.source_counter_before, callbacks: raw.measured_callbacks};
                    assert(row.normal_update_observation.delta === raw.measured_callbacks,
                        'Normal source update was not observed once per actual game callback');
                }
                if (workProtocol) row.work_observation = {schema: 1, mode: observerMode,
                    update_kind: host === 'jce' ? 'td_census' : 'source_counter',
                    samples: raw.work_samples, scope: 'Actual callback update and setup-program GL calls; not CPU submission/GPU completion'};
                row.elapsed_ms = elapsed; row.fps = intervals.length * 1000 / elapsed;
                row.interval_ms = statistics(intervals); row.cpu_ms = statistics(raw.cpu);
                const contextLost = await app.evaluate(() => window.jcePerfObservation.contexts.some(context => context.context_lost));
                assert(!contextLost && row.errors.length === 0, 'Actual context loss/browser errors occurred');
                row.status = 'ACTUAL_GAME_RAF_OBSERVED';
            } catch (error) {
                row.status = 'UNAVAILABLE'; row.errors.push({kind: 'acceptance', message: String(error)});
                // Failed/timeout windows retain all observed callbacks before
                // the context closes; no partial FPS or filtered raw is invented.
                if (lastPage && !lastPage.isClosed()) {
                    try {
                        row.partial_raw = await lastPage.evaluate(() => {
                            const state = window.jcePerfObservation;
                            if (!state) return null;
                            return {phase: state.phase, raf_timestamps_ms: state.timestamps,
                                callback_cpu_ms: state.cpu, warm_start_ms: state.warmStart,
                                sample_start_ms: state.sampleStart, measured_callbacks: state.measuredCallbacks,
                                source_counter_before: state.sourceCounterBefore,
                                source_counter_after: state.sourceCounterAfter, work_samples: state.workSamples};
                        });
                    } catch (partialError) { row.partial_raw_unavailable = String(partialError); }
                }
                errors.push({host, order: index, message: String(error)});
            }
        }
        report.completed_utc = new Date().toISOString();
        report.complete = runs.length === order.length && runs.every(row => row.status === 'ACTUAL_GAME_RAF_OBSERVED') && errors.length === 0;
        report.final_claims_allowed = (protocolName === 'publication' || workProtocol) && report.complete && quality.available && renderQuality.available && report.conditions.contended === false;
        if (workProtocol) {
            report.observer_control = {schema: 1, pairs: [], scope: 'Same-artifact off/on workload delta; do not subtract observer cost or infer isolated instruction cost'};
            for (let index = 0; index < runs.length; index += 4) for (let hostIndex = 0; hostIndex < 2; ++hostIndex) {
                const off = runs[index + hostIndex], on = runs[index + 2 + hostIndex];
                if (off && on && off.status === 'ACTUAL_GAME_RAF_OBSERVED' && on.status === 'ACTUAL_GAME_RAF_OBSERVED')
                    report.observer_control.pairs.push({host: off.host, repeat: off.repeat, off_order: off.order, on_order: on.order,
                        callback_cpu_mean_delta_ms: on.cpu_ms.mean - off.cpu_ms.mean,
                        interval_mean_delta_ms: on.interval_ms.mean - off.interval_ms.mean, callback_fps_delta: on.fps - off.fps});
            }
        }
        report.status = report.final_claims_allowed ? (workProtocol ? 'MATCHED_INITIAL_GAME_WORK_CONSUMED_OBSERVED' : 'MATCHED_INITIAL_GAME_RAF_OBSERVED') :
            report.complete ? 'RAW_TIMING_OBSERVED_FINAL_SCOPE_INCOMPLETE' : 'UNAVAILABLE';
    } catch (error) {
        errors.push({kind: 'suite', message: String(error)});
        report.status = 'UNAVAILABLE'; report.complete = false; report.final_claims_allowed = false;
    } finally {
        try {
            if (!lastPage || lastPage.isClosed()) {
                lastContext = lastContext || await browser.newContext({acceptDownloads: true});
                lastPage = await lastContext.newPage();
            }
            if (lastPage && !lastPage.isClosed()) {
                const filename = 'web-performance-' + config.run_id + '.json';
                const pending = lastPage.waitForEvent('download', {timeout});
                // If page evaluation fails, closing the owned browser must not
                // leave the separate download event promise unhandled.
                pending.catch(() => {});
                await lastPage.evaluate(({filename, text}) => {
                    const url = URL.createObjectURL(new Blob([text], {type: 'application/json'}));
                    const anchor = document.createElement('a'); anchor.href = url; anchor.download = filename;
                    document.body.appendChild(anchor); anchor.click(); anchor.remove();
                    setTimeout(() => URL.revokeObjectURL(url), 1000);
                }, {filename, text: JSON.stringify(report, null, 2) + '\n'});
                const file = await pending;
                const path = config.output_dir.replace(/[\\/]+$/, '') + '/' + filename;
                await file.saveAs(path); downloads.push({path});
            }
        } finally { await browser.close(); }
    }
    return {status: report.status, protocol: report.protocol, complete: report.complete, final_claims_allowed: report.final_claims_allowed,
        quality: {bootstrap_available: quality.available, actual_ground_available: renderQuality.available},
        runs: runs.map(row => ({host: row.host, repeat: row.repeat, status: row.status, fps: row.fps})), errors, downloads};
}
