/* Test-only probe, independently authored; upstream files stay immutable. */
(function () {
    var frame = 0, fixture = new URLSearchParams(location.search).get('fixture');
    var events = [], randomCalls=[], frozen = false, timingReport=null;
    function state() {
        var td = TowerDefense, camera = td.camera, objects = [];
        (td.objects || []).forEach(function (o) {
            if (!o.object || !o.object.position) return;
            var type=o.type || (o instanceof TowerDefense.Tower?"TOWER":"");
            if (type || o.selected) objects.push({id:o.id,type:type,name:o.name,p:o.p,
                path:o.type==='BULLET'?o.path:null,grid:o.gridPosition,position:o.object.position,parentPosition:o.object.parent?o.object.parent.position:null,rotation:{x:o.object.rotation.x,y:o.object.rotation.y,z:o.object.rotation.z},stats:o.stats,selected:o.selected});
        });
        if(!td.scene || !td.scene.children || !camera || !camera.position) return {fixture:fixture,frame:frame,stats:td.stats,camera:null,objects:objects,events:events,reference_commit:'784129a685627a18f3d3f8fa594208000d9313e8'};
        var ground=td.scene.children.filter(function(o){return o.material && o.material.specularMap;})[0];
        return {fixture:fixture,frame:frame,time:Date.now(),timing:timingReport,stats:td.stats,wave:td.currentLevel.currentWave,
            render:{fog:td.scene.fog,lights:td.scene.children.filter(function(o){return o instanceof THREE.Light;}).map(function(o){return {position:o.position,intensity:o.intensity,color:o.color,target:o.target?o.target.position:null,type:o instanceof THREE.DirectionalLight?'directional':'other'};}),gammaInput:td.renderer.gammaInput,gammaOutput:td.renderer.gammaOutput,ground:ground?{geometry:{vertices:ground.geometry.vertices,faces:ground.geometry.faces.map(function(f){return {a:f.a,b:f.b,c:f.c,normal:f.normal,vertexNormals:f.vertexNormals};}),uvs:ground.geometry.faceVertexUvs[0]},material:ground.material.type,gpuShininess:ground.material.program?td.renderer.getContext().getUniform(ground.material.program.program,ground.material.program.uniforms.shininess):null,uniformShininess:ground.material.__webglShader?ground.material.__webglShader.uniforms.shininess.value:null,color:ground.material.color,emissive:ground.material.emissive,specular:ground.material.specular,shininess:ground.material.shininess,normalMap:ground.material.normalMap!=null,map:ground.material.map.image.src,specularMap:ground.material.specularMap.image.src}:null},
            camera:camera.position?{position:camera.position,quaternion:camera.quaternion,up:camera.up,fov:camera.fov,near:camera.near,far:camera.far,aspect:camera.aspect,target:td.controls.target}:null,
            objects:objects,events:events,randomCalls:randomCalls,reference_commit:'784129a685627a18f3d3f8fa594208000d9313e8'};
    }
    function save(name) {
        TowerDefense.renderer.render(TowerDefense.scene,TowerDefense.camera);
        return fetch('/evidence',{method:'POST',headers:{'Content-Type':'application/json'},
            body:JSON.stringify({name:name,image:TowerDefense.renderer.domElement.toDataURL('image/png'),state:state()})});
    }
    ['keyup','click','mousedown','mouseup','mousemove','wheel','blur','focus'].forEach(function (type) {
        document.addEventListener(type,function(e){
            if(e.target.closest && e.target.closest('#probe')) return;
            events.push({type:type,time:Date.now(),key:e.key,code:e.keyCode,x:e.clientX,y:e.clientY,button:e.button,deltaY:e.deltaY});
            if (events.length > 1000) events.shift();
        },true);
    });
    var panel = document.createElement('aside'); panel.id='probe';
    panel.style.cssText='position:fixed;left:6px;bottom:4px;z-index:10000;font:12px Arial;color:white';
    panel.innerHTML='<button id="probe-freeze">Freeze reference</button> <button id="probe-save">Save canvas</button> <button id="probe-upload">Upload browser evidence</button><span id="probe-status"></span><form id="probe-form" style="display:none"><textarea id="capture-payload" aria-label="Browser screenshot payload"></textarea><button type="submit">Save browser capture</button></form>';
    document.body.appendChild(panel);
    /* Test input producer: dispatches standard event payloads through the
     * ORIGINAL OrbitControls listeners, without modifying camera state. */
    if(new URLSearchParams(location.search).has('input-fixture')) {
        var inputPanel=document.createElement('aside');inputPanel.id='control-probe';
        inputPanel.style.cssText='position:fixed;top:6px;left:6px;z-index:10001;background:black;color:white';
        inputPanel.innerHTML='<select aria-label="Reference input fixture"><option>wheel</option><option>right-pan</option><option>middle-dolly</option><option>touch-rotate</option><option>touch-pinch</option><option>touch-pan</option></select><button id="control-replay">Replay input fixture</button>';
        document.body.appendChild(inputPanel);
        document.getElementById('control-replay').onclick=function(){
            var kind=inputPanel.querySelector('select').value;
            function mouse(type,x,y,button) {document.dispatchEvent(new MouseEvent(type,{bubbles:true,cancelable:true,clientX:x,clientY:y,button:button}));}
            function touch(type,points) {
                var e=new Event(type,{bubbles:true,cancelable:true});
                Object.defineProperty(e,'touches',{value:points.map(function(p,i){return {identifier:i+1,pageX:p[0],pageY:p[1],clientX:p[0],clientY:p[1]};})});
                document.dispatchEvent(e);
            }
            if(kind==='wheel') {
                var e=new Event('mousewheel',{bubbles:true,cancelable:true});
                Object.defineProperty(e,'wheelDelta',{value:120});document.dispatchEvent(e);
            } else if(kind==='right-pan' || kind==='middle-dolly') {
                var button=kind==='right-pan'?2:1;mouse('mousedown',640,360,button);
                mouse('mousemove',655,kind==='right-pan'?369:370,button);mouse('mouseup',655,370,button);
            } else {
                var before=[[640,360]],after=[[700,330]];
                if(kind==='touch-pinch') {before=[[540,360],[740,360]];after=[[530,360],[750,360]];}
                if(kind==='touch-pan') {before=[[640,360],[700,360],[760,360]];after=[[655,369],[715,369],[775,369]];}
                touch('touchstart',before);touch('touchmove',after);touch('touchend',[]);
            }
            requestAnimationFrame(function(){
                TowerDefense.renderer.render(TowerDefense.scene,TowerDefense.camera);
                requestAnimationFrame(function(){
                    save('control-'+kind).then(function(){document.getElementById('probe-status').textContent=' INPUT READY '+kind;});
                });
            });
        };
    }

    document.getElementById('probe-freeze').onclick=function(){
        frozen=true; cancelAnimationFrame(gameRender); document.getElementById('probe-status').textContent=' FROZEN';
    };
    document.getElementById('probe-save').onclick=function(){
        save(fixture || 'interactive').then(function(){document.getElementById('probe-status').textContent=' SAVED';});
    };
    document.getElementById('probe-upload').onclick=function(){document.getElementById('probe-form').style.display='block';};
    document.getElementById('probe-form').onsubmit=function(e){
        e.preventDefault(); var data=JSON.parse(document.getElementById('capture-payload').value);
        data.state=state(); data.browser=true;
        fetch('/evidence',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)})
            .then(function(){document.getElementById('probe-form').style.display='none';document.getElementById('probe-status').textContent=' BROWSER SAVED';});
    };
    function until(test) {return new Promise(function(resolve,reject){var tries=0,id=setInterval(function(){if(test()){clearInterval(id);resolve();}else if(++tries>300){clearInterval(id);reject(Error('fixture timeout'));}},20);});}
    async function build(x,y,kind) {
        TowerDefense.deselectAll(); TowerDefense.grid[x][y].select(); TowerDefense.Ui.buildTower(kind);
        await until(function(){return TowerDefense.grid[x][y].currentTower.id!=null;});
    }
    async function runFixture() {
        var rng=7; Math.random=function(){rng^=rng<<13;rng^=rng>>>17;rng^=rng<<5;var v=(rng>>>0)/4294967296;randomCalls.push({frame:frame,value:v,stack:new Error().stack});return v;};
        Date.now=function(){return frame*1000/60;};
        render=function(){TowerDefense.renderer.render(TowerDefense.scene,TowerDefense.camera);};
        TowerDefense.time=0; TowerDefense.Ui.playLevel();
        await until(function(){return TowerDefense.camera && TowerDefense.camera.position && TowerDefense.grid[10] && TowerDefense.grid[10][10] && TowerDefense.controls.target;});
        if(fixture==='select') TowerDefense.grid[5][5].select();
        if(fixture==='low-money') {TowerDefense.stats.resources=0;TowerDefense.grid[5][5].select();}
        if(fixture==='basic') await build(1,0,0);
        if(fixture==='advanced' || fixture==='combat') await build(1,0,1);
        if(fixture==='rocket' || fixture==='combat') await build(2,1,2);
        if(fixture==='ghost' || fixture==='ufo' || fixture==='combat') {
            spawnEnemy(fixture==='ufo'?'ufo':null);
            await until(function(){return TowerDefense.objects.some(function(o){return o.type==='ENEMY' && o.path.length>0;});});
            var total=fixture==='combat'?120:120;
            for(frame=1;frame<=total;frame++) TowerDefense.update();
            frame=total;
        }
        /* Original CSS card transitions and the 50 ms observation fallback
         * settle independently of the frozen simulation clock. */
        await new Promise(function(resolve){setTimeout(resolve,350);});
        await until(function(){return getComputedStyle(document.getElementById('loading-container')).display==='none';});
        await save(fixture);
        document.getElementById('probe-status').textContent=' READY '+fixture;
    }
    if(new URLSearchParams(location.search).has('timing-fixture')) {
        var timingButton=document.createElement('button');timingButton.textContent='Measure original timing';
        panel.appendChild(timingButton);
        timingButton.onclick=async function(){
            document.getElementById('probe-status').textContent=' TIMING RUNNING';
            TowerDefense.Ui.playLevel();
            await until(function(){return TowerDefense.grid[10] && TowerDefense.grid[10][10] && TowerDefense.camera.position;});
            spawnEnemy();
            await until(function(){return TowerDefense.objects.some(function(o){return o.type==='ENEMY' && o.path.length>0;});});
            var enemy=TowerDefense.objects.filter(function(o){return o.type==='ENEMY';})[0];
            var start=performance.now(),last=start,frames=[],updateCPU=[],renderCPU=[];
            var originalUpdate=TowerDefense.update,originalRender=TowerDefense.renderer.render;
            var initial={p:enemy.p,position:{x:enemy.object.position.x,y:enemy.object.position.y,z:enemy.object.position.z},pathLength:enemy.path.length,pSpeed:enemy.pSpeed};
            TowerDefense.update=function(){var before=performance.now();originalUpdate.apply(this,arguments);var now=performance.now();updateCPU.push(now-before);frames.push(now-last);last=now;};
            TowerDefense.renderer.render=function(){var before=performance.now();var out=originalRender.apply(this,arguments);renderCPU.push(performance.now()-before);return out;};
            await new Promise(function(resolve){setTimeout(resolve,5000);});
            TowerDefense.update=originalUpdate;TowerDefense.renderer.render=originalRender;
            var elapsed=performance.now()-start;
            timingReport={visibility:document.visibilityState,viewport:[innerWidth,innerHeight],elapsed_ms:elapsed,updates:frames.length,update_hz:frames.length*1000/elapsed,frame_ms:frames,update_cpu_ms:updateCPU,render_cpu_ms:renderCPU,initial:initial,
                final:{p:enemy.p,position:{x:enemy.object.position.x,y:enemy.object.position.y,z:enemy.object.position.z}},
                method:'Unmodified Date.now and requestAnimationFrame; wrappers measure original update/render calls. CPU submit timing is not GPU time.'};
            await save('timing-live');document.getElementById('probe-status').textContent=' TIMING SAVED';
        };
    }
    if(fixture) runFixture().catch(function(e){document.getElementById('probe-status').textContent=' FAILED '+e.message;});
})();
