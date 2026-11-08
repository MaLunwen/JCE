async (page, config, hashValue) => {
    // Bounded actual WebGL ground-draw observation; never run in timing windows.
    const assert = (ok, text) => { if (!ok) throw new Error(text); };
    assert(typeof hashValue === 'function', 'Shared browser hash function required');
    assert(/^[0-9a-f]{64}$/.test(config.artifact_sha256 || ''), 'Actual artifact SHA required');
    assert(/^[A-Za-z0-9_-]{1,64}$/.test(config.run_id || ''), 'ASCII run_id required');
    assert(/^(?:[A-Za-z]:[\\/]|\/)/.test(config.output_dir || ''), 'Absolute output_dir required');
    const base = new URL(config.artifact_url);
    assert(/^https?:$/.test(base.protocol) && !base.search && !base.hash, 'Unmodified HTTP(S) artifact URL required');
    const hash = value => hashValue(page, value, new URL('/__jce_qa_hash_' + config.run_id, base).href);
    const browser = await page.context().browser().browserType().launch({
        channel: config.browser_channel || 'chrome', headless: true,
        args: ['--enable-gpu', '--use-angle=d3d11']
    });
    const errors = [], downloads = [];
    const result = {schema: 1, status: 'NOT_RUN', artifact_sha256: config.artifact_sha256,
        artifact_url: base.href, run_id: config.run_id, generated_utc: new Date().toISOString(),
        scope: 'Instrumented actual WebGL ground draw and linked shader/uniform state. No timing or every-fragment claim.', errors};
    let app, frozenResult;
    const onConsole = message => { if (message.type() === 'error') errors.push({kind: 'console', message: message.text()}); };
    const onPageError = error => errors.push({kind: 'pageerror', message: String(error)});
    const join = name => config.output_dir.replace(/[\\/]+$/, '') + '/' + name;
    async function save(name, value) {
        const text = typeof value === 'string' ? value : JSON.stringify(value, null, 2) + '\n';
        const waiting = app.waitForEvent('download', {timeout: 30000, predicate: file => file.suggestedFilename() === name});
        waiting.catch(() => {});
        await app.evaluate(({name, text}) => {
            const url = URL.createObjectURL(new Blob([text], {type: 'text/plain'}));
            const a = document.createElement('a'); a.href = url; a.download = name;
            document.body.appendChild(a); a.click(); a.remove();
            setTimeout(() => URL.revokeObjectURL(url), 1000);
        }, {name, text});
        const file = await waiting, path = join(name); await file.saveAs(path);
        const row = {path, sha256: await hash(text), bytes: Buffer.byteLength(text)};
        downloads.push(row); return row;
    }
    try {
        const context = await browser.newContext({viewport: {width: 1280, height: 720}, deviceScaleFactor: 1, acceptDownloads: true});
        const response = await context.request.get(base.href);
        assert(response.ok(), 'Artifact HTTP status ' + response.status());
        result.served_artifact_sha256 = await hash(await response.body());
        assert(result.served_artifact_sha256 === config.artifact_sha256, 'Served HTML identity changed');
        await context.addInitScript(() => {
            window.jceActualQuality = {contexts: [], candidates: [], programs: [], lost: 0, draw_calls: 0, draws36: 0};
            const originalGetContext = HTMLCanvasElement.prototype.getContext, seen = new WeakSet();
            HTMLCanvasElement.prototype.getContext = function (...args) {
                const gl = originalGetContext.apply(this, args);
                if (!gl || !/^(webgl2?|experimental-webgl)$/.test(String(args[0])) || seen.has(gl)) return gl;
                seen.add(gl);
                const context = {kind: String(args[0]), version: gl.getParameter(gl.VERSION),
                    shading_language: gl.getParameter(gl.SHADING_LANGUAGE_VERSION), lost: false};
                const debug = gl.getExtension('WEBGL_debug_renderer_info');
                context.renderer = debug ? gl.getParameter(debug.UNMASKED_RENDERER_WEBGL) : null;
                context.vendor = debug ? gl.getParameter(debug.UNMASKED_VENDOR_WEBGL) : null;
                window.jceActualQuality.contexts.push(context);
                this.addEventListener('webglcontextlost', () => { context.lost = true; });
                let activeProgram = null;
                // bgfx detaches/deletes shader objects after a successful link.
                // Preserve exact observed source text and actual link metadata,
                // without retaining any shader object or altering its lifetime.
                const shaderSources = new WeakMap(), linkedPrograms = new WeakMap();
                const originalSource = gl.shaderSource, originalLink = gl.linkProgram;
                gl.shaderSource = function (shader, source) {
                    const out = originalSource.call(this, shader, source);
                    shaderSources.set(shader, String(source)); return out;
                };
                gl.linkProgram = function (program) {
                    const shaders = gl.getAttachedShaders(program) || [];
                    const out = originalLink.call(this, program);
                    const fragment = shaders.find(shader => gl.getShaderParameter(shader, gl.SHADER_TYPE) === gl.FRAGMENT_SHADER);
                    linkedPrograms.set(program, {fragment_source: fragment ? shaderSources.get(fragment) : '',
                        fragment_compiled: fragment ? gl.getShaderParameter(fragment, gl.COMPILE_STATUS) : false,
                        linked: gl.getProgramParameter(program, gl.LINK_STATUS)});
                    return out;
                };
                const originalUse = gl.useProgram;
                gl.useProgram = function (program) { const out = originalUse.call(this, program); activeProgram = program; return out; };
                const inspected = new WeakSet(), reported = new WeakSet();
                for (const method of ['drawElements', 'drawElementsInstanced']) {
                    if (typeof gl[method] !== 'function') continue;
                    const original = gl[method];
                    gl[method] = function (...values) {
                        const out = original.apply(this, values);
                        ++window.jceActualQuality.draw_calls;
                        if (values[1] === 36) ++window.jceActualQuality.draws36;
                        if (values[1] !== 36 || !activeProgram || inspected.has(activeProgram)) return out;
                        const linked = linkedPrograms.get(activeProgram);
                        const source = linked ? linked.fragment_source : '';
                        if (!source) return out;
                        const uniforms = {}, names = [];
                        const count = gl.getProgramParameter(activeProgram, gl.ACTIVE_UNIFORMS);
                        for (let i = 0; i < count; ++i) {
                            const info = gl.getActiveUniform(activeProgram, i), name = info.name.replace(/\[0\]$/, '');
                            names.push(info.name);
                            if (!['u_pbrParams', 'u_baseColorFactor', 'u_ambientColor', 'u_lookHemiGround',
                                'u_dirLights', 'u_lightCounts', 'u_csmParams', 'u_shadowQuality'].includes(name)) continue;
                            const indices = info.size > 1 ? Array.from({length: info.size}, (_, n) => name + '[' + n + ']') : [info.name];
                            const actual = [];
                            for (const index of indices) {
                                const location = gl.getUniformLocation(activeProgram, index);
                                if (!location) continue;
                                const value = gl.getUniform(activeProgram, location);
                                actual.push(...(typeof value === 'number' ? [value] : Array.from(value || [])));
                            }
                            uniforms[name] = actual;
                        }
                        if (!reported.has(activeProgram)) { reported.add(activeProgram);
                            window.jceActualQuality.programs.push({names, uniforms, source_bytes: source.length, source_start: source.slice(0, 300), contains_finite_helper: source.includes('classic_lobe_power')});
                        }
                        const pbr = uniforms.u_pbrParams;
                        if (!pbr || !Number.isFinite(pbr[0]) || !(Math.abs(pbr[0] - 1 / 15) <= 1e-6) || pbr[1] !== 1) return out;
                        inspected.add(activeProgram);
                        if (window.jceActualQuality.candidates.length >= 2) { ++window.jceActualQuality.lost; return out; }
                        window.jceActualQuality.candidates.push({method, count: values[1],
                            timestamp_ms: performance.now(), linked: gl.getProgramParameter(activeProgram, gl.LINK_STATUS),
                            fragment_compiled: linked.fragment_compiled,
                            gl_error: gl.getError(), uniforms, fragment_source: source});
                        return out;
                    };
                }
                return gl;
            };
        });
        app = await context.newPage();
        app.on('console', onConsole);
        app.on('pageerror', onPageError);
        const url = new URL(base); url.searchParams.set('fixture', 'initial'); url.searchParams.set('seed', '7');
        // Large inline HTML exceeds Chromium's inspector response cache.
        // Hash the actual navigation's HTTP bytes before forwarding them
        // unchanged; no application code or asset bytes are substituted.
        let executed = null;
        await context.route(url.href, async route => {
            const actual = await route.fetch({maxRedirects: 0});
            assert(actual.ok(), 'Executed artifact HTTP status ' + actual.status());
            const bytes = await actual.body(); executed = await hash(bytes);
            assert(executed === config.artifact_sha256, 'Executed artifact bytes differ from identity GET');
            await route.fulfill({response: actual, body: bytes});
        });
        const navigation = await app.goto(url.href, {waitUntil: 'domcontentloaded', timeout: 60000});
        assert(navigation && navigation.ok() && executed, 'Executed navigation unavailable');
        result.executed_artifact_sha256 = executed;
        result.executed_transport = 'Actual route.fetch HTTP bytes, hashed then forwarded byte-identically to bypass inspector-cache eviction';
        await app.waitForFunction(() => window.jceActualQuality && window.jceActualQuality.candidates.length > 0, null, {timeout: 20000});
        const observation = await app.evaluate(() => JSON.parse(JSON.stringify(window.jceActualQuality)));
        result.program_observations = observation.programs; result.draw_calls = observation.draw_calls; result.draws36 = observation.draws36;
        result.contexts = observation.contexts; result.lost = observation.lost; result.ground_draws = [];
        assert(observation.contexts.length > 0 && observation.contexts.every(x => !x.lost), 'Actual WebGL context unavailable/lost');
        assert(observation.candidates.length > 0 && observation.lost === 0, 'Missing/bounded quality observation failed');
        for (let i = 0; i < observation.candidates.length; ++i) {
            const draw = observation.candidates[i], source = draw.fragment_source; delete draw.fragment_source;
            draw.fragment_source = await save('web-quality-' + config.run_id + '-fragment-' + i + '.glsl', source);
            result.ground_draws.push(draw);
            assert(draw.linked === true && draw.fragment_compiled === true && draw.gl_error === 0, 'Actual compiled ground program/GL failed');
            const near = (actual, expected) => Array.isArray(actual) && actual.length === expected.length &&
                actual.every((value, n) => Number.isFinite(value) && Math.abs(value - expected[n]) <= 1e-6);
            assert(near(draw.uniforms.u_pbrParams, [1 / 15, 1, 1, 0.5]) && near(draw.uniforms.u_baseColorFactor, [1, 1, 1, 1]), 'Actual ground material fingerprint changed');
            assert(near(draw.uniforms.u_lookHemiGround, [247 / 255, 1, 243 / 255, 1]), 'Actual hemisphere ground uniform differs from pinned source request');
            assert(near(draw.uniforms.u_ambientColor, [243 / 255, 253 / 255, 1, 0.6]), 'Actual ambient sky/intensity changed');
            assert(near(draw.uniforms.u_lightCounts, [1, 0, 0, 1]), 'Actual directional/shadow light counts changed');
            assert(near(draw.uniforms.u_dirLights, [66 / Math.hypot(66, 84, 60), -84 / Math.hypot(66, 84, 60), 60 / Math.hypot(66, 84, 60), 0.7, 1, 0.96, 0.9, 0, 0, 0, 0, 0, 0, 0, 0, 0]), 'Actual directional light direction/color/intensity changed');
        }
        assert(errors.length === 0, 'Browser reported errors');
        result.status = 'ACTUAL_WEB_GROUND_QUALITY_OBSERVED';
    } catch (error) {
        result.status = 'FAILED'; errors.push({kind: 'harness', message: String(error)});
        if (app) try { result.partial_observation = await app.evaluate(() => window.jceActualQuality); } catch (_) {}
    } finally {
        result.completed_utc = new Date().toISOString();
        if (app) { app.off('console', onConsole); app.off('pageerror', onPageError); }
        if (errors.length) result.status = 'FAILED';
        frozenResult = JSON.parse(JSON.stringify(result));
        try { if (app) await save('web-render-quality-' + config.run_id + '.json', frozenResult); }
        finally { await browser.close(); }
    }
    return {status: frozenResult.status, errors: frozenResult.errors, downloads, ground_draw_count: (frozenResult.ground_draws || []).length};
}
