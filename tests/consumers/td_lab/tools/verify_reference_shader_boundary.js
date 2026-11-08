async (page, config) => {
    // Explicit diagnostic reference policy; never a raw-source parity gate.
    // MCP exposes Buffer and browser APIs, but neither require nor dynamic import.
    let hashPage = null;
    async function sha256(value) {
        if (!hashPage) throw new Error('Secure QA hash page has not been initialized');
        const base64 = Buffer.from(value).toString('base64');
        return hashPage.evaluate(async base64 => {
            const binary = atob(base64), bytes = new Uint8Array(binary.length);
            for (let index = 0; index < binary.length; ++index) bytes[index] = binary.charCodeAt(index);
            const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
            return Array.from(digest, value => value.toString(16).padStart(2, '0')).join('');
        }, base64);
    }
    const assert = (ok, message) => { if (!ok) throw new Error(message); };
    const pin = '784129a685627a18f3d3f8fa594208000d9313e8';
    const policy = 'DIAGNOSTIC_DEFINED_POWER_REFERENCE';

    // BEGIN PURE SHADER PATCH: used by CPU-only negative controls.
    function definedPowerPatch(chunk, fragment) {
        const helperName = 'jceReferenceClassicLobePower';
        const helper = 'float ' + helperName + '(float base, float exponent) {\n' +
            '    if (exponent == 0.0) return 1.0;\n' +
            '    if (base <= 0.0) return 0.0;\n' +
            '    return pow(base, exponent);\n' +
            '}\n';
        const calls = [
            'pow( dirDotNormalHalf, shininess )',
            'pow( max( hemiDotNormalHalfSky, 0.0 ), shininess )',
            'pow( max( hemiDotNormalHalfGround, 0.0 ), shininess )'
        ];
        if (typeof chunk !== 'string' || typeof fragment !== 'string' ||
            chunk.includes(helperName) || fragment.includes(helperName)) {
            throw new Error('Missing or already modified original shader strings');
        }
        const count = (text, needle) => text.split(needle).length - 1;
        if (count(fragment, chunk) !== 1) throw new Error('Exact Phong chunk must occur once in ShaderLib');
        let modified = chunk;
        const replacements = [];
        for (const call of calls) {
            if (count(chunk, call) !== 1) throw new Error('Expected one original lobe call: ' + call);
            const replacement = helperName + call.slice(3);
            modified = modified.replace(call, replacement);
            replacements.push({before: call, after: replacement, count: 1});
        }
        let restored = modified;
        for (const row of replacements) restored = restored.replace(row.after, row.before);
        if (restored !== chunk) throw new Error('Non-lobe shader bytes changed');
        return {helperName, helper, calls, replacements, originalChunk: chunk, modifiedChunk: modified,
            originalFragment: fragment, modifiedFragment: helper + '\n' + fragment.replace(chunk, modified)};
    }
    // END PURE SHADER PATCH.

    assert(config && config.reference_commit === pin, 'The immutable reference pin is required');
    for (const key of ['source_html_sha256', 'source_application_sha256',
        'source_three_sha256', 'source_probe_sha256']) {
        assert(/^[a-f0-9]{64}$/.test(config[key] || ''), 'Missing explicit identity: ' + key);
    }
    assert(/^[A-Za-z0-9_-]{1,64}$/.test(config.run_id || ''), 'Invalid run_id');
    assert(/^(?:[A-Za-z]:[\\/]|\/)/.test(config.output_dir || ''), 'output_dir must be absolute');
    const endpoint = new URL(config.source_url);
    assert((endpoint.protocol === 'http:' || endpoint.protocol === 'https:') &&
        !endpoint.search && !endpoint.hash, 'source_url must be unqueried HTTP HTML');
    const allowedControls = ['initial', 'wheel', 'right-pan', 'middle-dolly',
        'touch-rotate', 'touch-pinch', 'touch-pan'];
    const controls = config.controls || ['initial', 'touch-rotate'];
    assert(Array.isArray(controls) && controls.length > 0 &&
        controls.every(value => allowedControls.includes(value)) &&
        new Set(controls).size === controls.length, 'Invalid or duplicate controls');
    const repeats = config.capture_repeats === undefined ? 4 : config.capture_repeats;
    assert(Number.isInteger(repeats) && repeats >= 1 && repeats <= 8, 'capture_repeats must be 1..8');
    const timeout = config.timeout_ms || 60000;
    assert(Number.isInteger(timeout) && timeout >= 5000 && timeout <= 120000, 'Invalid timeout_ms');
    const joinOutput = name => config.output_dir.replace(/[\\/]+$/, '') + '/' + name;
    const rows = [], errors = [], downloads = [];
    const report = {schema: 1, status: 'NOT_RUN', policy, raw_source_parity_passed: false,
        generated_utc: new Date().toISOString(), reference_commit: pin, source_url: endpoint.href,
        run_id: config.run_id, rows, errors, downloads,
        requested: {viewport: [1280, 720], dpr: 1, controls, capture_repeats: repeats,
            browser_channel: config.browser_channel || 'chrome', angle: 'd3d11'},
        expected_inputs: Object.fromEntries(['html', 'application', 'three', 'probe'].map(
            kind => [kind + '_sha256', config['source_' + kind + '_sha256']])),
        method: ['Each camera control has a fresh original initial fixture and original OrbitControls input producer.',
            'Raw browser PNGs precede the in-memory shader policy; no upstream network/file byte is rewritten.',
            'Only three directional/hemi lobe powers change; exponent zero returns one, nonpositive base with positive exponent returns zero, remaining pow is unchanged.',
            'ShaderChunk and its preassembled ShaderLib fragment are changed in memory; helper is global, calls remain in main.',
            'Unique scene Phong materials are disposed and marked needsUpdate to invalidate the r68 GPU shader cache.',
            'Actual linked/used fragment sources and the ground shininess uniform are required after recompilation.',
            'Scene/camera/material-value snapshots must stay exactly equal before and after the shader-only policy.',
            'Reference /evidence POSTs are intercepted and fulfilled without writing existing golden files.'],
        limits: ['This separate defined-mathematics policy never replaces raw original acceptance or its failing measurements.',
            'PNG integer pixels and shader compilation do not expose fragment floating-point NaNs or certify every GPU.',
            'No performance or commercial-engine parity conclusion follows from this diagnostic capture.']};
    const browser = await page.context().browser().browserType().launch({
        channel: config.browser_channel || 'chrome', headless: true,
        args: ['--enable-gpu', '--use-angle=d3d11']
    });
    let app = null, context = null, hashContext = null;
    async function nextFrames(count) {
        await app.evaluate(count => new Promise(resolve => {
            function step() { if (--count <= 0) resolve(); else requestAnimationFrame(step); }
            requestAnimationFrame(step);
        }), count);
    }
    async function saveJSON(filename, value) {
        const text = JSON.stringify(value, null, 2) + '\n';
        const pending = app.waitForEvent('download', {timeout});
        await app.evaluate(({filename, text}) => {
            const url = URL.createObjectURL(new Blob([text], {type: 'application/json'}));
            const anchor = document.createElement('a');
            anchor.href = url; anchor.download = filename; document.body.appendChild(anchor);
            anchor.click(); anchor.remove(); setTimeout(() => URL.revokeObjectURL(url), 1000);
        }, {filename, text});
        const path = joinOutput(filename);
        await (await pending).saveAs(path);
        downloads.push({path, bytes: Buffer.byteLength(text), sha256: await sha256(text)});
    }
    async function observeGround() {
        return app.evaluate(() => {
            const td = TowerDefense, gl = td.renderer.getContext(), observation = window.jceBoundaryGL;
            function serial(value) {
                if (value === undefined) return null;
                if (ArrayBuffer.isView(value)) return Array.from(value);
                return value;
            }
            const grounds = td.scene.children.filter(object => object.material && object.material.specularMap);
            if (grounds.length !== 1) throw new Error('Expected exactly one authored specular-map ground');
            const ground = grounds[0], material = ground.material;
            const program = material.program && material.program.program;
            if (!program || !gl.isProgram(program)) throw new Error('Ground has no actual GL program');
            const record = observation.programs.find(value => value.object === program);
            if (!record || !record.linked || record.draw_calls === 0) throw new Error('Ground program was not linked and used');
            const location = gl.getUniformLocation(program, 'shininess');
            if (location === null) throw new Error('Actual ground shininess uniform unavailable');
            const uniform = gl.getUniform(program, location);
            const families = ['hemisphereLightSkyColor', 'hemisphereLightGroundColor',
                'hemisphereLightDirection', 'directionalLightColor', 'directionalLightDirection'];
            // Actual linked ground-program uniforms; absent locations stay unavailable.
            function observeUniform(name) {
                const uniformLocation = gl.getUniformLocation(program, name);
                if (uniformLocation === null) return {name, status: 'UNAVAILABLE', value: null,
                    reason: 'No location in this actual linked ground program'};
                const value = serial(gl.getUniform(program, uniformLocation));
                if (value === null) return {name, status: 'UNAVAILABLE', value: null,
                    reason: 'Actual GL uniform query returned null'};
                return {name, status: 'AVAILABLE', value,
                    value_is_finite: Array.isArray(value) ? value.every(Number.isFinite) : Number.isFinite(value)};
            }
            const requestedUniforms = families.map(name => observeUniform(name + '[0]'));
            const activeUniforms = [], arrayElements = [];
            const activeCount = gl.getProgramParameter(program, gl.ACTIVE_UNIFORMS);
            for (let index = 0; index < activeCount; ++index) {
                const active = gl.getActiveUniform(program, index);
                if (!active) continue;
                const family = families.find(name => active.name === name || active.name === name + '[0]');
                if (!family) continue;
                activeUniforms.push({name: active.name, size: active.size, type: active.type});
                for (let element = 0; element < active.size; ++element) {
                    arrayElements.push(observeUniform(family + '[' + element + ']'));
                }
            }
            activeUniforms.sort((left, right) => left.name.localeCompare(right.name));
            arrayElements.sort((left, right) => left.name.localeCompare(right.name));
            const sceneLights = [];
            function colorObservation(value) {
                return value ? {status: 'AVAILABLE', value: [value.r, value.g, value.b]} :
                    {status: 'UNAVAILABLE', value: null, reason: 'Property absent on this scene light'};
            }
            td.scene.traverse(object => {
                if (!(object instanceof THREE.Light)) return;
                sceneLights.push({id: object.id,
                    kind: object instanceof THREE.HemisphereLight ? 'hemisphere' :
                        object instanceof THREE.DirectionalLight ? 'directional' : 'other',
                    sky_color: colorObservation(object.color), ground_color: colorObservation(object.groundColor),
                    intensity: typeof object.intensity === 'number' ?
                        {status: 'AVAILABLE', value: object.intensity} :
                        {status: 'UNAVAILABLE', value: null, reason: 'Intensity property absent'},
                    position: [object.position.x, object.position.y, object.position.z],
                    world_matrix: Array.from(object.matrixWorld.elements),
                    target_position: object.target ? [object.target.position.x, object.target.position.y,
                        object.target.position.z] : null});
            });
            sceneLights.sort((left, right) => left.id - right.id);
            const lighting = {requested_uniforms: requestedUniforms, active_uniforms: activeUniforms,
                array_elements: arrayElements, scene_lights: sceneLights,
                scope: 'Actual original scene values and linked ground-program GL uniform values; no assumed light count'};
            const errors = [];
            for (let index = 0; index < 16; ++index) {
                const error = gl.getError(); if (error === gl.NO_ERROR) break; errors.push(error);
                if (index === 15) throw new Error('GL errors did not drain within diagnostic bound');
            }
            const extension = gl.getExtension('WEBGL_debug_renderer_info');
            const shaderRows = record.shaders.map(value => ({id: value.id, type: value.type,
                source: value.source, compiled: value.compiled, info_log: value.info_log}));
            return {three_revision: THREE.REVISION, context: {
                version: gl.getParameter(gl.VERSION), glsl: gl.getParameter(gl.SHADING_LANGUAGE_VERSION),
                renderer: gl.getParameter(gl.RENDERER), vendor: gl.getParameter(gl.VENDOR),
                unmasked_renderer: extension ? gl.getParameter(extension.UNMASKED_RENDERER_WEBGL) : null,
                unmasked_vendor: extension ? gl.getParameter(extension.UNMASKED_VENDOR_WEBGL) : null,
                attributes: gl.getContextAttributes(), drawing_buffer: [gl.drawingBufferWidth, gl.drawingBufferHeight],
                canvas: [td.renderer.domElement.width, td.renderer.domElement.height],
                viewport: [innerWidth, innerHeight], dpr: devicePixelRatio},
                ground: {object_id: ground.id, material_id: material.id, material_shininess: material.shininess,
                    uniform_shininess: serial(uniform), specular: [material.specular.r, material.specular.g, material.specular.b],
                    program_id: record.id, linked: record.linked, link_log: record.info_log,
                    draw_calls: record.draw_calls, index_counts: record.index_counts, shaders: shaderRows},
                lighting, gl_errors: errors, observer_errors: observation.errors.slice()};
        });
    }
    async function sceneSnapshot() {
        return app.evaluate(() => {
            const td = TowerDefense;
            const vec = value => value ? [value.x, value.y, value.z] : null;
            const color = value => value ? [value.r, value.g, value.b] : null;
            const texture = value => value ? {id: value.id,
                src: value.image && value.image.src ? value.image.src : null} : null;
            function material(value) {
                if (!value) return null;
                if (value.materials) return value.materials.map(material);
                return {id: value.id, type: value.type, color: color(value.color), ambient: color(value.ambient),
                    emissive: color(value.emissive), specular: color(value.specular), shininess: value.shininess,
                    opacity: value.opacity, transparent: value.transparent, side: value.side, shading: value.shading,
                    depthTest: value.depthTest, depthWrite: value.depthWrite,
                    map: texture(value.map), specularMap: texture(value.specularMap)};
            }
            const objects = [];
            td.scene.traverse(object => {
                const geometry = object.geometry;
                objects.push({id: object.id, parent_id: object.parent ? object.parent.id : null, name: object.name,
                    position: vec(object.position), quaternion: [object.quaternion.x, object.quaternion.y,
                        object.quaternion.z, object.quaternion.w], scale: vec(object.scale), visible: object.visible,
                    castShadow: object.castShadow, receiveShadow: object.receiveShadow, material: material(object.material),
                    light: object instanceof THREE.Light ? {
                        kind: object instanceof THREE.HemisphereLight ? 'hemisphere' :
                            object instanceof THREE.DirectionalLight ? 'directional' : 'other',
                        color: color(object.color), ground_color: color(object.groundColor), intensity: object.intensity,
                        target_position: object.target ? vec(object.target.position) : null} : null,
                    geometry: geometry ? {id: geometry.id, vertices: (geometry.vertices || []).map(vec),
                        faces: (geometry.faces || []).map(face => ({a: face.a, b: face.b, c: face.c,
                            normal: vec(face.normal), vertex_normals: (face.vertexNormals || []).map(vec)})),
                        uvs: (geometry.faceVertexUvs || []).map(layer => layer.map(face => face.map(uv => [uv.x, uv.y])))} : null});
            });
            const camera = td.camera;
            return {camera: {position: vec(camera.position), quaternion: [camera.quaternion.x, camera.quaternion.y,
                camera.quaternion.z, camera.quaternion.w], target: vec(td.controls.target), up: vec(camera.up),
                fov: camera.fov, near: camera.near, far: camera.far, aspect: camera.aspect,
                world_matrix: Array.from(camera.matrixWorld.elements), projection_matrix: Array.from(camera.projectionMatrix.elements)},
                fog: td.scene.fog ? {color: color(td.scene.fog.color), near: td.scene.fog.near, far: td.scene.fog.far} : null,
                stats: {...td.stats}, objects};
        });
    }
    async function captures(row, phase, baseline) {
        row[phase + '_captures'] = [];
        for (let index = 0; index < repeats; ++index) {
            await app.evaluate(() => TowerDefense.renderer.render(TowerDefense.scene, TowerDefense.camera));
            await nextFrames(2);
            const snapshot = await sceneSnapshot();
            assert(JSON.stringify(snapshot) === JSON.stringify(baseline), phase + ' scene/camera values changed');
            const path = joinOutput('reference-boundary-' + config.run_id + '-' + row.control + '-' + phase + '-' + index + '.png');
            const bytes = await app.screenshot({path, type: 'png'});
            row[phase + '_captures'].push({path, bytes: bytes.length, sha256: await sha256(bytes)});
        }
    }
    async function hashShaders(observation) {
        for (const shader of observation.ground.shaders) shader.source_sha256 = await sha256(shader.source);
    }
    function fragmentSource(observation) {
        const shaders = observation.ground.shaders.filter(shader => shader.type === 35632);
        assert(shaders.length === 1 && shaders[0].compiled === true, 'Expected one actually compiled fragment shader');
        return shaders[0].source;
    }
    try {
        report.browser_version = browser.version();
        hashContext = await browser.newContext({serviceWorkers: 'block'});
        hashPage = await hashContext.newPage();
        const hashURL = new URL('/__jce_qa_sha256__/' + config.run_id, endpoint).href;
        await hashPage.route(hashURL, route => route.fulfill({status: 200,
            contentType: 'text/html', body: '<!doctype html><html><body>QA hashing</body></html>'}));
        await hashPage.goto(hashURL, {waitUntil: 'load', timeout});
        assert(await hashPage.evaluate(() => isSecureContext && !!crypto.subtle),
            'Actual QA hash page must provide secure-context WebCrypto');
        report.hash_method = {api: 'Platform WebCrypto SHA-256', url: hashURL,
            transport: 'Exact route.fulfill; no server request, app bootstrap or GL context',
            encoding: 'Buffer base64 payload decoded to Uint8Array before digest'};
        for (const control of controls) {
            if (app) { await app.close(); app = null; }
            if (context) { await context.close(); context = null; }
            context = await browser.newContext({viewport: {width: 1280, height: 720},
                deviceScaleFactor: 1, acceptDownloads: true, serviceWorkers: 'block'});
            const row = {control, status: 'NOT_RUN', errors: [], logs: [], inputs: {}, probe_posts: []};
            rows.push(row);
            app = await context.newPage();
            app.on('pageerror', error => row.errors.push(String(error)));
            app.on('console', message => {
                if (row.logs.length < 1000) row.logs.push({type: message.type(), text: message.text()});
                if (message.type() === 'error') row.errors.push('Console: ' + message.text());
            });
            try {
                const paths = {html: endpoint.href, application: new URL('js/application.js', endpoint).href,
                    three: new URL('js/lib/three.min.js', endpoint).href, probe: new URL('reference_probe.js', endpoint).href};
                for (const [kind, url] of Object.entries(paths)) {
                    const response = await context.request.get(url, {maxRedirects: 0, timeout});
                    assert(response.ok(), 'Source ' + kind + ' HTTP status ' + response.status());
                    const body = await response.body();
                    row.inputs[kind + '_sha256'] = await sha256(body);
                    assert(row.inputs[kind + '_sha256'] === config['source_' + kind + '_sha256'],
                        'Actual served source identity changed: ' + kind);
                    const headers = {...response.headers()};
                    delete headers['content-encoding']; delete headers['transfer-encoding'];
                    headers['content-length'] = String(body.length);
                    // Serve the exact verified bytes, including the unmodified owned probe.
                    await context.route(kind === 'html' ? requestURL => {
                        const actual = new URL(requestURL);
                        return actual.origin === endpoint.origin && actual.pathname === endpoint.pathname;
                    } : url, route => route.fulfill({response, headers, body}));
                }
                await context.route(new URL('evidence', endpoint).href, async route => {
                    try {
                        assert(route.request().method() === 'POST', 'Unexpected /evidence method');
                        const data = route.request().postDataJSON();
                        assert(data.state && data.state.reference_commit === pin, 'Probe reference pin mismatch');
                        assert(typeof data.image === 'string' && data.image.startsWith('data:image/png;base64,'),
                            'Probe did not produce an actual PNG');
                        const bytes = Buffer.from(data.image.slice(data.image.indexOf(',') + 1), 'base64');
                        assert(bytes.subarray(0, 8).equals(Buffer.from([137,80,78,71,13,10,26,10])), 'Invalid probe PNG signature');
                        row.probe_posts.push({name: data.name, state: data.state,
                            image_sha256: await sha256(bytes), image_bytes: bytes.length});
                        await route.fulfill({status: 200, contentType: 'text/plain', body: 'DIAGNOSTIC OBSERVED; GOLDENS UNTOUCHED'});
                    } catch (error) {
                        row.errors.push(String(error));
                        await route.fulfill({status: 400, contentType: 'text/plain', body: 'DIAGNOSTIC FAILED'});
                    }
                });
                await context.addInitScript(() => {
                    const observation = window.jceBoundaryGL = {programs: [], shaders: [], errors: []};
                    const original = HTMLCanvasElement.prototype.getContext;
                    const patched = new WeakSet();
                    HTMLCanvasElement.prototype.getContext = function (...args) {
                        const gl = original.apply(this, args);
                        if (!gl || !/^webgl$|^experimental-webgl$/.test(String(args[0])) || patched.has(gl)) return gl;
                        patched.add(gl);
                        const shaders = new WeakMap(), programs = new WeakMap();
                        function wrap(name, observer) {
                            const native = gl[name];
                            gl[name] = function (...values) {
                                const result = native.apply(this, values);
                                try { observer.call(this, values, result); } catch (error) { observation.errors.push(String(error)); }
                                return result;
                            };
                        }
                        wrap('createShader', (values, object) => {
                            if (!object) return;
                            const row = {object, id: observation.shaders.length + 1, type: values[0], source: null,
                                compiled: null, info_log: null};
                            shaders.set(object, row); observation.shaders.push(row);
                        });
                        wrap('shaderSource', values => { const row = shaders.get(values[0]); if (row) row.source = values[1]; });
                        wrap('compileShader', values => {
                            const row = shaders.get(values[0]);
                            if (row) { row.compiled = gl.getShaderParameter(values[0], gl.COMPILE_STATUS);
                                row.info_log = gl.getShaderInfoLog(values[0]); }
                        });
                        wrap('createProgram', (values, object) => {
                            if (!object) return;
                            const row = {object, id: observation.programs.length + 1, shaders: [], linked: null,
                                info_log: null, draw_calls: 0, index_counts: {}};
                            programs.set(object, row); observation.programs.push(row);
                        });
                        wrap('attachShader', values => {
                            const program = programs.get(values[0]), shader = shaders.get(values[1]);
                            if (program && shader) program.shaders.push(shader);
                        });
                        wrap('linkProgram', values => {
                            const row = programs.get(values[0]);
                            if (row) { row.linked = gl.getProgramParameter(values[0], gl.LINK_STATUS);
                                row.info_log = gl.getProgramInfoLog(values[0]); }
                        });
                        wrap('drawElements', values => {
                            const row = programs.get(gl.getParameter(gl.CURRENT_PROGRAM));
                            if (row) { ++row.draw_calls; row.index_counts[String(values[1])] =
                                (row.index_counts[String(values[1])] || 0) + 1; }
                        });
                        return gl;
                    };
                });
                const fixture = new URL(endpoint);
                fixture.searchParams.set('fixture', 'initial'); fixture.searchParams.set('input-fixture', '1');
                await app.goto(fixture.href, {waitUntil: 'load', timeout});
                await app.waitForFunction(() => document.getElementById('probe-status') &&
                    document.getElementById('probe-status').textContent.trim() === 'READY initial', null, {timeout});
                assert(row.probe_posts.length === 1, 'Initial fixture must complete one actual intercepted POST');
                if (control !== 'initial') {
                    await app.getByRole('combobox', {name: 'Reference input fixture'}).selectOption(control);
                    await app.locator('#control-replay').click();
                    await app.waitForFunction(control => document.getElementById('probe-status').textContent.trim() ===
                        'INPUT READY ' + control, control, {timeout});
                    assert(row.probe_posts.length === 2, 'Control fixture must produce its actual intercepted POST');
                }
                await app.evaluate(() => {
                    for (const id of ['probe', 'control-probe']) document.getElementById(id).style.visibility = 'hidden';
                    TowerDefense.renderer.render(TowerDefense.scene, TowerDefense.camera);
                });
                await nextFrames(2);
                row.raw_observation = await observeGround(); await hashShaders(row.raw_observation);
                assert(String(row.raw_observation.three_revision) === '68', 'Actual Three.js revision must be 68');
                assert(row.raw_observation.ground.material_shininess === 0 &&
                    row.raw_observation.ground.uniform_shininess === 0, 'Actual ground shininess must be zero');
                assert(row.raw_observation.context.viewport.join(',') === '1280,720' &&
                    row.raw_observation.context.dpr === 1 &&
                    row.raw_observation.context.drawing_buffer.join(',') === '1280,720', 'Actual dimensions changed');
                row.scene_before = await sceneSnapshot();
                row.scene_before_sha256 = await sha256(JSON.stringify(row.scene_before));
                const original = await app.evaluate(() => ({chunk: THREE.ShaderChunk.lights_phong_fragment,
                    fragment: THREE.ShaderLib.phong.fragmentShader}));
                const patch = definedPowerPatch(original.chunk, original.fragment);
                row.shader_policy = {name: policy, helper: patch.helper, replacements: patch.replacements,
                    original_chunk_sha256: await sha256(patch.originalChunk), modified_chunk_sha256: await sha256(patch.modifiedChunk),
                    original_fragment_sha256: await sha256(patch.originalFragment), modified_fragment_sha256: await sha256(patch.modifiedFragment),
                    original_chunk: patch.originalChunk, modified_chunk: patch.modifiedChunk,
                    original_fragment: patch.originalFragment, modified_fragment: patch.modifiedFragment};
                assert(fragmentSource(row.raw_observation).includes(original.chunk), 'Raw actual fragment lacks original chunk');
                await captures(row, 'raw', row.scene_before);
                row.program_rebuild = await app.evaluate(patch => {
                    if (THREE.ShaderChunk.lights_phong_fragment !== patch.originalChunk ||
                        THREE.ShaderLib.phong.fragmentShader !== patch.originalFragment) {
                        throw new Error('Original shader changed before the explicit memory-only policy');
                    }
                    const materials = [];
                    function add(value) {
                        if (!value) return;
                        if (value.materials) { value.materials.forEach(add); return; }
                        if (value instanceof THREE.MeshPhongMaterial && !materials.includes(value)) materials.push(value);
                    }
                    TowerDefense.scene.traverse(object => add(object.material));
                    if (materials.length === 0) throw new Error('No scene Phong material to rebuild');
                    if (materials.some(value => !Number.isFinite(value.shininess) || value.shininess < 0)) {
                        throw new Error('The diagnostic lobe policy requires finite nonnegative shininess');
                    }
                    const previousProgramIDs = materials.map(value => value.program ? value.program.id : null);
                    THREE.ShaderChunk.lights_phong_fragment = patch.modifiedChunk;
                    THREE.ShaderLib.phong.fragmentShader = patch.modifiedFragment;
                    materials.forEach(value => { value.dispose(); value.needsUpdate = true; });
                    TowerDefense.renderer.render(TowerDefense.scene, TowerDefense.camera);
                    return {method: 'Unique scene Phong material dispose plus needsUpdate; original r68 renderer recompiles',
                        material_ids: materials.map(value => value.id), previous_three_program_ids: previousProgramIDs,
                        next_three_program_ids: materials.map(value => value.program ? value.program.id : null)};
                }, patch);
                await nextFrames(2);
                row.finite_observation = await observeGround(); await hashShaders(row.finite_observation);
                assert(row.finite_observation.ground.program_id !== row.raw_observation.ground.program_id,
                    'Old cached GPU ground program was reused');
                const actualFragment = fragmentSource(row.finite_observation);
                assert(actualFragment.includes(patch.modifiedFragment) && actualFragment.includes(patch.helper),
                    'Actual compiled fragment does not contain the defined helper and exact modified ShaderLib');
                for (const replacement of patch.replacements) {
                    assert(actualFragment.includes(replacement.after) && !actualFragment.includes(replacement.before),
                        'Actual compiled fragment did not replace exactly the requested lobe');
                }
                assert(row.finite_observation.ground.uniform_shininess === 0 &&
                    row.finite_observation.ground.material_shininess === 0, 'Finite policy changed shininess');
                assert(JSON.stringify(row.raw_observation.lighting) === JSON.stringify(row.finite_observation.lighting),
                    'Shader-only policy changed actual scene lighting or ground-program light uniforms');
                assert(row.raw_observation.gl_errors.length === 0 && row.finite_observation.gl_errors.length === 0 &&
                    row.raw_observation.observer_errors.length === 0 && row.finite_observation.observer_errors.length === 0,
                    'Actual GL/observer errors were recorded');
                row.scene_after = await sceneSnapshot();
                row.scene_after_sha256 = await sha256(JSON.stringify(row.scene_after));
                assert(row.scene_before_sha256 === row.scene_after_sha256, 'Scene/camera/material values changed');
                await captures(row, 'finite', row.scene_before);
                assert(row.errors.length === 0, 'Browser/probe errors were recorded');
                row.status = 'DEFINED_POWER_REFERENCE_CAPTURED';
            } catch (error) {
                row.errors.push(String(error)); row.status = 'FAILED'; errors.push(control + ': ' + String(error));
            }
            await saveJSON('reference-boundary-' + config.run_id + '-' + control + '.json', row);
        }
        report.status = errors.length === 0 && rows.every(row => row.status === 'DEFINED_POWER_REFERENCE_CAPTURED') ?
            'DEFINED_POWER_REFERENCE_CAPTURED' : 'FAILED';
        await saveJSON('reference-boundary-' + config.run_id + '.json', report);
        return report;
    } finally {
        if (app) await app.close().catch(error => errors.push('Page close: ' + String(error)));
        if (context) await context.close().catch(error => errors.push('Context close: ' + String(error)));
        if (hashContext) await hashContext.close().catch(error => errors.push('Hash context close: ' + String(error)));
        hashPage = null;
        await browser.close();
    }
}
