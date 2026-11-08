async (page, config, hashValue) => {
    // Run through Playwright's code tool with an explicit config argument.
    // This is acceptance instrumentation, never a production bootstrap.
    const assert = (ok, message) => { if (!ok) throw new Error(message); };
    assert(typeof hashValue === 'function', 'Shared verify_web_hash.js function argument required');
    const sha256 = value => hashValue(page, value, new URL('/__jce_qa_hash_' + config.run_id, config.artifact_url).href);
    const mode = config && config.mode;
    assert(mode === 'input' || mode === 'lifecycle', 'mode must be input or lifecycle');
    for (const key of ['artifact_sha256', 'request_sha256', 'nonce']) {
        assert(/^[0-9a-f]{64}$/.test(config[key] || ''), 'Missing SHA-256/nonce: ' + key);
    }
    assert(/^[A-Za-z0-9_-]{1,64}$/.test(config.run_id || ''), 'Invalid run_id');
    assert(/^(?:[A-Za-z]:[\\/]|\/)/.test(config.output_dir || ''), 'output_dir must be absolute');
    const requestText = config.request_json;
    assert(typeof requestText === 'string' && await sha256(requestText) === config.request_sha256,
        'Exact request JSON bytes do not match request_sha256');
    const request = JSON.parse(requestText);
    assert(request && request.schema === 1 && request.artifact_sha256 === config.artifact_sha256,
        'Request does not bind this artifact');
    assert(request.nonce === config.nonce && request.environment &&
        request.environment.JCE_NATIVE_REGISTRY_NONCE === config.nonce,
        'Request nonce/environment mismatch');
    assert(typeof request.environment.JCE_NATIVE_REGISTRY_REPORT === 'string' &&
        request.environment.JCE_NATIVE_REGISTRY_REPORT.startsWith('/'), 'Missing MEMFS report path');
    assert(Number.isInteger(request.selected_count) && request.selected_count > 0 &&
        Number.isInteger(request.factory_count) && request.factory_count > 0,
        'Request must select actual native factories');
    const base = new URL(config.artifact_url);
    assert(base.protocol === 'http:' || base.protocol === 'https:', 'HTTP(S) transport required');
    assert(!base.search && !base.hash, 'artifact_url must identify the unmodified HTML URL');
    assert(config.restrict_network === undefined || typeof config.restrict_network === 'boolean',
        'restrict_network must be a boolean');
    const restrictNetwork = config.restrict_network === true;
    const timeout = config.timeout_ms || 60000;
    assert(Number.isInteger(timeout) && timeout >= 5000 && timeout <= 120000, 'Invalid timeout_ms');
    const viewport = config.viewport || (mode === 'input' ? {width: 1280, height: 720} : {width: 640, height: 360});
    assert(Number.isInteger(viewport.width) && viewport.width > 0 &&
        Number.isInteger(viewport.height) && viewport.height > 0, 'Invalid viewport');
    if (mode === 'input') assert(viewport.width === 1280 && viewport.height === 720,
        'Input coordinates require the canonical 1280x720 DPR=1 viewport');
    const frames = config.frames || 1000;
    if (mode === 'lifecycle') assert(Number.isInteger(frames) && frames >= 1000 && frames <= 10000,
        'Lifecycle frames must be 1000..10000');
    const joinOutput = name => config.output_dir.replace(/[\\/]+$/, '') + '/' + name;
    const names = {
        result: 'web-' + (mode === 'input' ? 'input' : 'gpu-cleanup') + '-' + config.run_id + '.json',
        receipt: 'web-registry-' + mode + '-' + config.run_id + '.json',
        trace: 'web-lifecycle-' + config.run_id + '.json'
    };
    const browser = await page.context().browser().browserType().launch({
        channel: config.browser_channel || 'chrome', headless: true,
        args: ['--enable-gpu', '--use-angle=d3d11']
    });
    let app;
    const errors = [], logs = [], downloads = [], screenshots = [];
    const networkRequests = [], unexpectedNetwork = [];
    const networkByRequest = new WeakMap();
    function observeRequest(request) {
        const url = new URL(request.url());
        if (url.protocol !== 'http:' && url.protocol !== 'https:') return null;
        let row = networkByRequest.get(request);
        if (!row) {
            row = {source: 'browser', method: request.method(), url: request.url(), status: null};
            networkByRequest.set(request, row);
            networkRequests.push(row);
        }
        return row;
    }
    const result = {
        schema: 1, status: 'NOT_RUN', artifact_sha256: config.artifact_sha256,
        registry_request_json: requestText,
        request_sha256: config.request_sha256, artifact_url: base.href,
        run_id: config.run_id, mode, generated_utc: new Date().toISOString(),
        producer: 'Actual browser mouse/key/resize events',
        errors, logs, network: {restricted: restrictNetwork,
            allowed_origin: restrictNetwork ? base.origin : null,
            allowed_path: restrictNetwork ? base.pathname : null,
            requests: networkRequests, unexpected: unexpectedNetwork}
    };
    let envelope = null;
    async function downloadText(filename, value) {
        const text = typeof value === 'string' ? value : JSON.stringify(value, null, 2) + '\n';
        const pending = app.waitForEvent('download', {timeout});
        await app.evaluate(({filename, text}) => {
            const url = URL.createObjectURL(new Blob([text], {type: 'application/json'}));
            const anchor = document.createElement('a');
            anchor.href = url; anchor.download = filename; document.body.appendChild(anchor);
            anchor.click(); anchor.remove(); setTimeout(() => URL.revokeObjectURL(url), 1000);
        }, {filename, text});
        const file = await pending;
        const path = joinOutput(filename);
        await file.saveAs(path);
        downloads.push({path, sha256: await sha256(text), bytes: Buffer.byteLength(text)});
    }
    const nextFrames = count => app.evaluate(count => new Promise(resolve => {
        function step() { if (--count <= 0) resolve(); else requestAnimationFrame(step); }
        requestAnimationFrame(step);
    }), count);
    const matching = pattern => logs.filter(text => pattern.test(text));
    const latest = pattern => matching(pattern).slice(-1)[0];
    const field = (line, name) => {
        const found = String(line).match(new RegExp('(?:^|\\s)' + name + '=([^\\s]+)'));
        return found ? Number(found[1]) : null;
    };
    async function waitLog(pattern, start = 0) {
        const beginning = Date.now();
        while (Date.now() - beginning < timeout) {
            const line = logs.slice(start).find(text => pattern.test(text));
            if (line) return line;
            await app.waitForTimeout(25);
        }
        throw new Error('Missing actual runtime log: ' + pattern);
    }
    async function screenshot(label) {
        const path = joinOutput('web-input-' + config.run_id + '-' + label + '.png');
        const bytes = await app.screenshot({path});
        const observation = {path, sha256: await sha256(bytes), bytes: bytes.length};
        screenshots.push(observation);
        return observation;
    }
    try {
        const context = await browser.newContext({viewport, deviceScaleFactor: 1, acceptDownloads: true,
            ...(restrictNetwork ? {serviceWorkers: 'block'} : {})});
        context.on('request', request => { observeRequest(request); });
        context.on('response', response => {
            const row = observeRequest(response.request());
            if (row) row.status = response.status();
        });
        context.on('requestfailed', request => {
            const row = observeRequest(request);
            if (row) row.failure = request.failure() && request.failure().errorText;
        });
        if (restrictNetwork) {
            result.relocation = {
                transport: 'HTTP', scope: 'Unicode empty-directory single HTML with strict HTTP dependency isolation',
                directory_inventory: 'Caller must separately retain the sole-HTML filesystem inventory',
                unicode_url_path: /[^\x00-\x7f]/.test(decodeURI(base.pathname)),
                service_workers: 'Blocked to prevent routing bypass in this diagnostic',
                file_uri_status: 'UNAVAILABLE_TOOL_BROWSER_SECURITY_POLICY',
                offline_file_uri_verified: false
            };
            await context.route('**/*', async route => {
                const request = route.request(), url = new URL(request.url());
                if (url.protocol !== 'http:' && url.protocol !== 'https:') {
                    // Inline data/blob mechanisms retain normal browser behavior.
                    await route.continue();
                    return;
                }
                const row = observeRequest(request);
                if (url.origin === base.origin && url.pathname === base.pathname) {
                    await route.continue();
                    return;
                }
                row.blocked = true;
                row.block_reason = 'Unexpected HTTP dependency outside the selected HTML origin/path';
                unexpectedNetwork.push(row);
                errors.push({kind: 'unexpected_http_dependency', method: row.method, url: row.url});
                result.status = 'FAILED';
                await route.abort('blockedbyclient');
            });
        }
        // Check the served HTML bytes before running them; the caller retains
        // the exact request file whose SHA-256 is supplied separately.
        // APIRequestContext is not covered by browser routing. Its identity
        // fetch targets exactly the permitted HTML; strict mode never follows
        // a redirect into a second request outside the declared allowlist.
        const identityRequest = {source: 'artifact_identity_http', method: 'GET', url: base.href, status: null};
        networkRequests.push(identityRequest);
        const response = await context.request.get(base.href, restrictNetwork ? {maxRedirects: 0} : {});
        identityRequest.status = response.status();
        assert(response.ok(), 'Artifact HTTP status ' + response.status());
        const actualHash = await sha256(await response.body());
        result.served_artifact_sha256 = actualHash;
        assert(actualHash === config.artifact_sha256, 'Served artifact SHA-256 mismatch');
        await context.addInitScript(({mode, request}) => {
            window.jceRegistryEnvironment = {...request.environment};
            const inputEvents = window.jceWebInputEvents = [];
            window.jceWebInputEventsLost = 0;
            function observeInput(event) {
                if (inputEvents.length >= 2000) { ++window.jceWebInputEventsLost; return; }
                inputEvents.push({type: event.type, timestamp_ms: performance.now(), trusted: event.isTrusted,
                    key: event.key, code: event.code, repeat: event.repeat, button: event.button,
                    x: event.clientX, y: event.clientY, width: innerWidth, height: innerHeight});
            }
            for (const type of ['mousedown', 'mouseup', 'mousemove', 'click', 'keydown', 'keyup'])
                document.addEventListener(type, observeInput, true);
            window.addEventListener('resize', observeInput, true);
            let moduleValue;
            const injected = new WeakSet();
            function inject(module) {
                if (!module || typeof module !== 'object' || injected.has(module)) return;
                injected.add(module);
                const original = module.preRun;
                module.preRun = Array.isArray(original) ? original :
                    typeof original === 'function' ? [original] : [];
                module.preRun.push(function () {
                    if (typeof ENV === 'undefined') throw new Error('Emscripten ENV unavailable');
                    ENV.JCE_PERF_LOG = '0';
                    ENV.JCE_TRACE = mode === 'lifecycle' ? '1' : '0';
                    if (mode === 'lifecycle') {
                        ENV.JCE_TRACE_STREAM = '/jce-lifecycle.json';
                        ENV.JCE_TRACE_STREAM_MODE = 'cooperative';
                    } else {
                        delete ENV.JCE_TRACE_STREAM;
                        delete ENV.JCE_TRACE_EXPORT;
                    }
                });
            }
            Object.defineProperty(window, 'Module', {
                configurable: true, get() { return moduleValue; },
                set(value) { moduleValue = value; inject(value); }
            });
            const originalGetContext = HTMLCanvasElement.prototype.getContext;
            const seen = new WeakSet();
            const contexts = [];
            window.jceWebObservation = {world_draw_calls: 0, contexts};
            HTMLCanvasElement.prototype.getContext = function (...args) {
                const gl = originalGetContext.apply(this, args);
                if (!gl || !/^(webgl2?|experimental-webgl)$/.test(String(args[0])) || seen.has(gl)) return gl;
                seen.add(gl);
                const record = {kind: String(args[0]), requested_attributes: args[1] || null,
                    attributes: gl.getContextAttributes(), version: gl.getParameter(gl.VERSION),
                    shading_language: gl.getParameter(gl.SHADING_LANGUAGE_VERSION),
                    context_lost: false, resources: {}};
                const debug = gl.getExtension('WEBGL_debug_renderer_info');
                record.vendor = debug ? gl.getParameter(debug.UNMASKED_VENDOR_WEBGL) : null;
                record.renderer = debug ? gl.getParameter(debug.UNMASKED_RENDERER_WEBGL) : null;
                contexts.push(record);
                this.addEventListener('webglcontextlost', () => { record.context_lost = true; });
                for (const name of ['drawElements', 'drawElementsInstanced']) {
                    if (typeof gl[name] !== 'function') continue;
                    const original = gl[name];
                    gl[name] = function (...values) {
                        const out = original.apply(this, values);
                        if (values[1] === 36) ++window.jceWebObservation.world_draw_calls;
                        return out;
                    };
                }
                if (mode !== 'lifecycle') return gl;
                // One wrapper per actual context; getContext return/attributes
                // and every original create/delete result remain untouched.
                for (const kind of ['Program', 'Buffer', 'Texture', 'Shader', 'Framebuffer',
                    'Renderbuffer', 'Sampler', 'Query', 'VertexArray', 'TransformFeedback']) {
                    const create = gl['create' + kind], remove = gl['delete' + kind], valid = gl['is' + kind];
                    const owned = new Set(), all = new Set();
                    const row = record.resources[kind] = {available: typeof create === 'function',
                        created: 0, deleted: 0, delete_calls: 0, live: 0, valid_survivors: null};
                    if (typeof create !== 'function' || typeof remove !== 'function') continue;
                    gl['create' + kind] = function (...values) {
                        const object = create.apply(this, values);
                        if (object) { owned.add(object); all.add(object); ++row.created; row.live = owned.size; }
                        return object;
                    };
                    gl['delete' + kind] = function (...values) {
                        const out = remove.apply(this, values);
                        if (values[0]) ++row.delete_calls;
                        if (owned.delete(values[0])) { ++row.deleted; row.live = owned.size; }
                        return out;
                    };
                    row.inspect = () => {
                        row.valid_survivors = typeof valid === 'function' ?
                            [...all].filter(object => valid.call(gl, object)).length : null;
                    };
                }
                record.inspect_error = () => gl.getError();
                return gl;
            };
        }, {mode, request});
        app = await context.newPage();
        app.on('console', message => {
            logs.push(message.text());
            if (message.type() === 'error') errors.push({kind: 'console', message: message.text()});
        });
        app.on('pageerror', error => errors.push({kind: 'pageerror', message: String(error)}));
        app.on('requestfailed', request => errors.push({kind: 'requestfailed', url: request.url(),
            message: request.failure() && request.failure().errorText}));
        const runUrl = new URL(base.href);
        runUrl.searchParams.set('qa', '');
        if (mode === 'input') runUrl.searchParams.set('fixture', 'initial');
        else {
            runUrl.searchParams.set('autodefense', '');
            runUrl.searchParams.set('frames', String(frames));
        }
        result.run_url = runUrl.href;
        await app.goto(runUrl.href, {waitUntil: 'domcontentloaded', timeout});
        await waitLog(/TD_BOOT /);
        await app.waitForFunction(() => window.jceWebObservation.world_draw_calls >= 3,
            null, {timeout});
        await app.waitForFunction(path => {
            try { return !!Module.FS.readFile(path, {encoding: 'utf8'}); } catch (_) { return false; }
        }, request.environment.JCE_NATIVE_REGISTRY_REPORT, {timeout});
        const receipt = await app.evaluate(path => JSON.parse(Module.FS.readFile(path, {encoding: 'utf8'})),
            request.environment.JCE_NATIVE_REGISTRY_REPORT);
        assert(receipt.schema === request.schema && receipt.closure_sha256 === request.closure_sha256 &&
            receipt.nonce === config.nonce && receipt.target === request.target && receipt.ok === true &&
            receipt.selected_count === request.selected_count && receipt.resolved_count === request.selected_count &&
            receipt.factory_count === request.factory_count && receipt.registered_factory_count === request.factory_count,
            'Actual browser registry receipt mismatch');
        envelope = {request_sha256: config.request_sha256, artifact_sha256: config.artifact_sha256, receipt};
        result.receipt = receipt;
        result.browser = await app.evaluate(() => navigator.userAgent);
        await app.locator('#qa').evaluate(element => { element.style.display = 'none'; });
        if (mode === 'input') {
            async function select(delay, expected) {
                const start = logs.length;
                await app.mouse.click(640, 360, {delay});
                const line = await waitLog(/TD_TRACE action=select /, start);
                await nextFrames(3);
                const events = logs.slice(start).filter(text => /TD_TRACE action=select /.test(text));
                assert(events.length === 1 && field(line, 'selected') === expected,
                    'Tile click must dispatch once and pick cell ' + expected);
                return {delay_ms: delay, select_events: events.length, selected: field(line, 'selected'), passed: true};
            }
            result.quickClick = await select(0, 60);
            const deselect = await select(100, -1);
            const ordinary = await select(100, 60);
            result.ordinaryClick = {...ordinary, prior_same_tile_deselect: deselect};
            // Wait only for the authored 0.3 s card transition before a real
            // card hit. Frozen fixture simulation and card animation are separate.
            await app.waitForTimeout(350);
            let start = logs.length;
            await app.mouse.click(535, 665, {delay: 100});
            const placed = await waitLog(/TD_TRACE action=place /, start);
            assert(field(placed, 'resources') === 554 && field(placed, 'towers') === 1,
                'Basic tower must spend one resource and create one tower');
            result.placement = {trace: placed, resources: 554, towers: 1, cost: 1, passed: true};
            await nextFrames(3);
            result.before = latest(/TD_CAMERA /);
            const beforeImage = await screenshot('before-drag');
            start = logs.length;
            await app.mouse.move(640, 360);
            await app.mouse.down({button: 'left'});
            for (let i = 1; i <= 6; ++i) {
                await app.mouse.move(640 + 60 * i / 6, 360 - 30 * i / 6);
                await app.waitForTimeout(20);
            }
            await app.mouse.up({button: 'left'});
            await waitLog(/TD_TRACE action=camera /, start);
            await nextFrames(3);
            result.after = latest(/TD_CAMERA /);
            assert(['x', 'y', 'z'].some(name => field(result.before, name) !== field(result.after, name)),
                'Actual camera pose did not change after drag');
            const afterImage = await screenshot('after-drag');
            assert(beforeImage.sha256 !== afterImage.sha256, 'Actual before/after drag pictures are identical');
            result.drag = {from: [640, 360], to: [700, 330], steps: 6,
                pose_changed: true, pictures_differ: true,
                scope: 'Actual captured pictures differ; independent pixel inspection/comparison is still required'};
            const n = matching(/TD_TRACE action=spawn /).length;
            await app.keyboard.down('e');
            await app.waitForTimeout(200);
            const held = matching(/TD_TRACE action=spawn /).length - n;
            assert(held === 0, 'E key-down/hold must not spawn');
            start = logs.length;
            await app.keyboard.up('e');
            await waitLog(/TD_TRACE action=spawn /, start);
            await nextFrames(3);
            const released = matching(/TD_TRACE action=spawn /).length - n;
            assert(released === 1, 'E key-up must spawn exactly once');
            result.keyboard = {n, held, released};
            start = logs.length;
            await app.setViewportSize({width: 896, height: 504});
            result.resize = await waitLog(/TD_CAMERA .*width=896 height=504/, start);
            result.canvas = await app.evaluate(() => [Module.canvas.width, Module.canvas.height]);
            assert(result.canvas[0] === 896 && result.canvas[1] === 504, 'Canvas resize was not applied');
            await nextFrames(3);
            await screenshot('resize');
            result.screenshots = screenshots;
        } else {
            result.producer = 'Real 1000+ frame autodefense game with cooperative trace and actual WebGL API observation';
            await waitLog(/TD_TRACE action=destroy /);
            await app.waitForFunction(() => {
                try {
                    const value = JSON.parse(Module.FS.readFile('/jce-lifecycle.json', {encoding: 'utf8'}));
                    return value.jceTrace && value.jceTrace.capture_complete === true;
                } catch (_) { return false; }
            }, null, {timeout, polling: 100});
            const traceText = await app.evaluate(() => Module.FS.readFile('/jce-lifecycle.json', {encoding: 'utf8'}));
            const trace = JSON.parse(traceText), metadata = trace.jceTrace;
            result.trace_metadata = metadata;
            result.trace_sha256 = await sha256(traceText);
            result.trace_path = joinOutput(names.trace);
            // Retain the producer's exact JSON bytes. The existing independent
            // lifecycle parser owns SERVICE pairing and required phase checks.
            await downloadText(names.trace, traceText);
            assert(metadata.mode === 'continuous' && metadata.consumer_mode === 'cooperative' &&
                metadata.worker_available === false && metadata.write_ok === true &&
                metadata.capture_generation_stable === true && metadata.capture_complete === true &&
                metadata.recorded === metadata.captured && metadata.captured === trace.traceEvents.length &&
                ['overwritten_before_capture', 'cursor_lost', 'lost', 'unexported',
                    'active_threads', 'active_work_items', 'active_waits'].every(key => metadata[key] === 0),
                'Trace footer is incomplete, lost, active or unavailable');
            const frameEvents = trace.traceEvents.filter(event => event.name === 'frame' && event.cat === 'frame');
            result.frame_observation = {requested: frames, observed: frameEvents.length,
                first: frameEvents[0] && frameEvents[0].args.id,
                last: frameEvents.slice(-1)[0] && frameEvents.slice(-1)[0].args.id};
            assert(frameEvents.length === frames && result.frame_observation.first === 0 &&
                result.frame_observation.last === frames - 1, 'Actual full requested frame run was not observed');
            result.contexts = await app.evaluate(() => {
                for (const context of window.jceWebObservation.contexts) {
                    for (const row of Object.values(context.resources)) if (row.inspect) row.inspect();
                    if (context.inspect_error) context.gl_error_after_shutdown = context.inspect_error();
                }
                return JSON.parse(JSON.stringify(window.jceWebObservation.contexts));
            });
            const resources = {}, programs = {created: 0, deleted: 0, live: 0};
            assert(result.contexts.length > 0, 'No actual WebGL context was observed');
            for (const context of result.contexts) {
                assert(!context.context_lost && context.gl_error_after_shutdown === 0,
                    'Actual context lost or GL error at shutdown');
                for (const [kind, row] of Object.entries(context.resources)) {
                    const totals = kind === 'Program' ? programs :
                        resources[kind] || (resources[kind] = {created: 0, deleted: 0, live: 0, valid_survivors: 0});
                    for (const key of ['created', 'deleted', 'live']) totals[key] += row[key];
                    if (kind !== 'Program') totals.valid_survivors += row.valid_survivors || 0;
                }
            }
            result.shutdown = {programsCreated: programs.created, programsDeleted: programs.deleted,
                live: programs.live, resources};
            result.shutdownWaitError = null;
            assert(result.contexts.every(context => Object.values(context.resources).every(row =>
                row.live === 0 && (row.valid_survivors === null || row.valid_survivors === 0))),
                'Actual WebGL objects survive engine shutdown');
            result.scope = 'Actual create/delete ownership and post-shutdown object validity before browser close; not GPU heap, fence completion or driver reclamation';
            result.independent_required_cpu_phase_validation = 'PENDING lifecycle_trace_report.py';
            result.wasm_linear_memory_capacity_bytes = await app.evaluate(() =>
                typeof HEAPU8 !== 'undefined' ? HEAPU8.byteLength : null);
        }
        result.input_events = await app.evaluate(() => window.jceWebInputEvents);
        result.input_events_lost = await app.evaluate(() => window.jceWebInputEventsLost);
        result.world_draw_calls = await app.evaluate(() => window.jceWebObservation.world_draw_calls);
        assert(unexpectedNetwork.length === 0, 'Unexpected external HTTP dependencies observed');
        assert(errors.length === 0, 'Actual browser errors observed');
        result.status = 'OBSERVED_CHECKS_PASSED';
    } catch (error) {
        errors.push({kind: 'acceptance', message: String(error)});
        result.status = 'FAILED';
        if (mode === 'lifecycle') {
            result.shutdownWaitError = String(error);
            // An incomplete producer footer remains useful negative evidence.
            // Preserve any actual file without synthesizing events or outcomes.
            if (app && !app.isClosed() && !result.trace_sha256) {
                try {
                    const text = await app.evaluate(() => Module.FS.readFile('/jce-lifecycle.json', {encoding: 'utf8'}));
                    result.trace_sha256 = await sha256(text);
                    result.trace_path = joinOutput(names.trace);
                    try { result.trace_metadata = JSON.parse(text).jceTrace; }
                    catch (parseError) { result.trace_parse_error = String(parseError); }
                    await downloadText(names.trace, text);
                } catch (readError) {
                    result.trace_unavailable = String(readError);
                }
            }
        }
    } finally {
        try {
            if (app && !app.isClosed()) {
                if (envelope) await downloadText(names.receipt, envelope);
                await downloadText(names.result, result);
            }
        } finally {
            await browser.close();
        }
    }
    return {status: result.status, errors, downloads, screenshots, network: result.network,
        artifact_sha256: result.artifact_sha256, request_sha256: result.request_sha256};
}
