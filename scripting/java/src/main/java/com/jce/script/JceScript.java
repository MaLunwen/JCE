/* JceScript.java -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * The scripting surface for Java, bound through JNI to the C ABI shared
 * library (scripting/c_abi).  Sources of truth:
 *   struct JceScriptHost   engine/include/jce/middleware/script/jce_script.h
 *   the decisions          engine/src/middleware/script/script_exposure.json
 *
 * THE SURFACE, NOT THE TRANSPORT.  The C ABI this class calls is deliberately
 * a different thing from the scripting surface: it passes 0-based indices,
 * takes an out-capacity as a parameter, copies owned strings into a caller
 * buffer, and reports a miss as `false` with zeroed outputs.  Every one of
 * those is re-applied here from the manifest, so that a Java script and a Lua
 * script see the SAME contract.  That equivalence is what
 * tests/scripting/java/ compares, entry by entry, against the Lua bindings.
 *
 * ABSENCE.  A host callback may be NULL and a shorter host leaves later
 * members NULL, so every entry has an absent answer:
 *   fallible_out / owned_string_release  -> null
 *   value_return of a C string           -> "" (never null: the Lua binding
 *                                          pushes the empty string, and
 *                                          `tr` echoes its own key)
 *   value_return of a number / boolean   -> 0 / false, or the manifest's
 *                                          absent_value where it has one
 *   void_out_array                       -> zeros
 *
 * UNSIGNED.  Java has no unsigned integer type.  uint32_t crosses as `int`
 * and uint64_t / JceScriptEntity as `long`, bit for bit; Integer.toUnsignedLong
 * and Long.toUnsignedString recover the value.  Widening them instead would
 * make this class disagree with the C ABI about a parameter's type.
 *
 * NOT ON THIS SURFACE.
 *
 * Seven entries are hand-written in the engine and excluded from the C
 * ABI as a class -- three are Lua-VM machinery and two carry sandbox
 * policy that lives in static functions in jce_script.c. Copying that
 * policy here would be a second implementation of a security decision.
 * The manifest's own reasons:
 *
 *   log
 *       no-host fallback: the LOG_INFO else-branch at jce_script.c:62 is the
 *       only one among the 78
 *   asset_read_text
 *       policy, not glue: script_virtual_asset_path_valid (:66, called :102),
 *       the 1 MiB JCE_SCRIPT_TEXT_ASSET_MAX_BYTES cap (jce_script.h:86),
 *       jce_free on every exit path. A generated read_file template is a
 *       sandbox escape (P0-2)
 *   asset_read_json
 *       all of asset_read_text (the same script_virtual_asset_path_valid at
 *       :219) plus a depth- and node-capped JSON walk, non-finite rejection,
 *       the json_null sentinel and distinct string error codes (P0-2)
 *   play_sound
 *       arity dispatch across two members: lua_gettop at :263 routes to
 *       play_sound_spatial (:286) or play_sound (:289); its own comment at
 *       :280 concedes top == 3 is ambiguous
 *   start_coroutine
 *       Lua VM machinery: lua_newthread / lua_xmove / luaL_ref / lua_resume;
 *       owns s->coros[]
 *   wait_seconds
 *       lua_yield into the same scheduler
 *   stop_coroutine
 *       scans s->coros[]
 *
 *   json_null (lightuserdata_sentinel) has no Java form: a lightuserdata
 *   sentinel is a Lua VM object compared by identity, and its only producer
 *   is asset_read_json, which is one of the seven above. A Java constant here
 *   could never be compared against anything. */
package com.jce.script;


/** The scripting surface: 71 entries over the C ABI. */
public final class JceScript implements AutoCloseable {

    /** The manifest's script_api_version this class was generated from. */
    public static final int SCRIPT_API_MIN = 1;

    /** Shared empty result, so an empty query allocates nothing. */
    private static final long[] EMPTY_ENTITIES = new long[0];

    /* The native library carrying the JNI shim. Set -Djce.script.library
     * to an absolute path to load a specific build; otherwise the platform
     * library path is searched for "jce_script_java".
     *
     * THE VERSION HANDSHAKE IS MANDATORY, the same rule engine/java's
     * JceRuntime follows -- and it is the SCRIPTING surface's number
     * (script_api_version), not the engine C ABI's jce_api_version(), which
     * this class never calls. A newer binding on an older library cannot
     * degrade: it would call entries that do not exist. Refusing at class
     * load names both numbers; open() refuses again in C, and the two compare
     * the same pair so they cannot disagree. */
    static {
        String explicit = System.getProperty("jce.script.library");
        if (explicit != null && !explicit.isEmpty()) {
            System.load(explicit);
        } else {
            System.loadLibrary("jce_script_java");
        }
        int have = nativeApiVersion();
        if (have < SCRIPT_API_MIN) {
            throw new IllegalStateException(
                "jce_script_java implements script_api_version " + have
                + " but " + JceScript.class.getName()
                + " was generated from " + SCRIPT_API_MIN
                + " -- a newer binding cannot run against an older library");
        }
    }

    private long handle;

    private JceScript(long handle) {
        this.handle = handle;
    }

    /** The script_api_version the loaded library implements. */
    public static int libraryApiVersion() {
        return nativeApiVersion();
    }

    /**
     * Binds to a host the engine already built.
     *
     * @param hostPointer address of a JceScriptHost the caller keeps alive
     *                    for the call (the library copies it)
     * @param hostSize    the CALLER's sizeof(JceScriptHost), always from
     *                    sizeof and never summed
     * @return a handle, or null when the library is older than
     *         SCRIPT_API_MIN or the arguments are unusable
     */
    public static JceScript open(long hostPointer, long hostSize) {
        long h = nativeOpen(hostPointer, hostSize, SCRIPT_API_MIN);
        return h == 0L ? null : new JceScript(h);
    }

    /** Releases the handle. Idempotent. */
    @Override
    public void close() {
        long h = handle;
        handle = 0L;
        if (h != 0L) {
            nativeClose(h);
        }
    }

    /** The raw JceScriptApi* -- for a native embedder, not for scripts. */
    public long nativeHandle() {
        return handle;
    }

    private static native int nativeApiVersion();
    private static native long nativeOpen(long hostPointer, long hostSize,
                                          int scriptApiMin);
    private static native void nativeClose(long api);

    /* ---- Result types, one per entry whose answer has more than
     * one component. An entry with a single component yields that
     * component directly. ---- */

    /** raycast: the host's answer when it has one. */
    public static final class RaycastResult {
        /** out parameter entity. */
        public final long entity;
        /** out parameter point. */
        public final float[] point;
        /** out parameter normal. */
        public final float[] normal;
        /** out parameter distance. */
        public final float distance;

        RaycastResult(long entity, float[] point, float[] normal, float distance) {
            this.entity = entity;
            this.point = point;
            this.normal = normal;
            this.distance = distance;
        }
    }

    /** get_touch: the host's answer when it has one. */
    public static final class GetTouchResult {
        /** out parameter id. */
        public final long id;
        /** out parameter x. */
        public final float x;
        /** out parameter y. */
        public final float y;
        /** out parameter pressure. */
        public final float pressure;

        GetTouchResult(long id, float x, float y, float pressure) {
            this.id = id;
            this.x = x;
            this.y = y;
            this.pressure = pressure;
        }
    }

    /** find_by_name: the host's answer when it has one. */
    public static final class FindByNameResult {
        /** the first match, or null when count is 0. */
        public final Long first;
        /** how many the host found. */
        public final int count;

        FindByNameResult(Long first, int count) {
            this.first = first;
            this.count = count;
        }
    }

    /**
     * World-space position of `entity`. Absent when the entity has no
     * transform.
     * <p>Shape fallible_out, since 1; host member get_position.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     */
    public float[] getPosition(long e) {
        float[] outFloat = new float[3];
        if (!nGetPosition(handle, e, outFloat))
            return null;
        return outFloat;
    }

    private static native boolean nGetPosition(long api, long e, float[] outFloat);

    /**
     * jce.set_position
     * <p>Shape void_call, since 1; host member set_position.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param x float
     * @param y float
     * @param z float
     */
    public void setPosition(long e, float x, float y, float z) {
        nSetPosition(handle, e, x, y, z);
    }

    private static native void nSetPosition(long api, long e, float x, float y, float z);

    /**
     * jce.get_rotation
     * <p>Shape fallible_out, since 1; host member get_rotation.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     */
    public float[] getRotation(long e) {
        float[] outFloat = new float[3];
        if (!nGetRotation(handle, e, outFloat))
            return null;
        return outFloat;
    }

    private static native boolean nGetRotation(long api, long e, float[] outFloat);

    /**
     * jce.set_rotation
     * <p>Shape void_call, since 1; host member set_rotation.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param x float
     * @param y float
     * @param z float
     */
    public void setRotation(long e, float x, float y, float z) {
        nSetRotation(handle, e, x, y, z);
    }

    private static native void nSetRotation(long api, long e, float x, float y, float z);

    /**
     * jce.get_scale
     * <p>Shape fallible_out, since 1; host member get_scale.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     */
    public float[] getScale(long e) {
        float[] outFloat = new float[3];
        if (!nGetScale(handle, e, outFloat))
            return null;
        return outFloat;
    }

    private static native boolean nGetScale(long api, long e, float[] outFloat);

    /**
     * jce.set_scale
     * <p>Shape void_call, since 1; host member set_scale.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param x float
     * @param y float
     * @param z float
     */
    public void setScale(long e, float x, float y, float z) {
        nSetScale(handle, e, x, y, z);
    }

    private static native void nSetScale(long api, long e, float x, float y, float z);

    /**
     * The ONLY boolean argument on this surface that is type-checked; the
     * other five accept any truthy value. The luaL_checktype is emitted from
     * this entry's `strict` modifier -- no line citation, because the
     * hand-written body that carried it is gone.
     * test_strict_emits_a_type_check_only_for_the_named_parameter is what
     * fails if the emitter drops it.
     * <p>Shape value_return, since 1; host member set_parent.
     * The Lua binding type-checks preserve_world at run time; here the Java
     * compiler does it, which is strictly stronger and leaves nothing to
     * emit.
     * @param child JceScriptEntity, unsigned; the bits round-trip exactly
     * @param parent JceScriptEntity, unsigned; the bits round-trip exactly
     * @param preserveWorld bool
     */
    public boolean setParent(long child, long parent, boolean preserveWorld) {
        return nSetParent(handle, child, parent, preserveWorld);
    }

    private static native boolean nSetParent(long api, long child, long parent, boolean preserveWorld);

    /**
     * jce.get_parent
     * <p>Shape value_return, since 1; host member get_parent.
     * @param child JceScriptEntity, unsigned; the bits round-trip exactly
     */
    public long getParent(long child) {
        return nGetParent(handle, child);
    }

    private static native long nGetParent(long api, long child);

    /**
     * jce.is_key_down
     * <p>Shape value_return, since 1; host member is_key_down.
     * @param keycode int
     */
    public boolean isKeyDown(int keycode) {
        return nIsKeyDown(handle, keycode);
    }

    private static native boolean nIsKeyDown(long api, int keycode);

    /**
     * jce.find_with_tag
     * <p>Shape value_return, since 1; host member find_with_tag.
     * @param tag const char *
     */
    public long findWithTag(String tag) {
        return nFindWithTag(handle, tag);
    }

    private static native long nFindWithTag(long api, String tag);

    /**
     * jce.destroy
     * <p>Shape void_call, since 1; host member destroy_entity.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     */
    public void destroy(long e) {
        nDestroy(handle, e);
    }

    private static native void nDestroy(long api, long e);

    /**
     * jce.spawn
     * <p>Shape value_return, since 1; host member spawn.
     * @param prefabPath const char *
     * @param x float
     * @param y float
     * @param z float
     */
    public long spawn(String prefabPath, float x, float y, float z) {
        return nSpawn(handle, prefabPath, x, y, z);
    }

    /** spawn with the manifest defaults (z=0.0). */
    public long spawn(String prefabPath, float x, float y) {
        return spawn(prefabPath, x, y, 0.0f);
    }

    /** spawn with the manifest defaults (y=0.0, z=0.0). */
    public long spawn(String prefabPath, float x) {
        return spawn(prefabPath, x, 0.0f, 0.0f);
    }

    /** spawn with the manifest defaults (x=0.0, y=0.0, z=0.0). */
    public long spawn(String prefabPath) {
        return spawn(prefabPath, 0.0f, 0.0f, 0.0f);
    }

    private static native long nSpawn(long api, String prefabPath, float x, float y, float z);

    /**
     * jce.move_axis
     * <p>Shape void_out_array, since 1; host member move_axis.
     */
    public float[] moveAxis() {
        float[] out = new float[2];
        nMoveAxis(handle, out);
        return out;
    }

    private static native void nMoveAxis(long api, float[] outFloat);

    /**
     * jce.jump_pressed
     * <p>Shape value_return, since 1; host member input_button.
     * The binding supplies button=0 (manifest bind_args), so it is not a
     * parameter here.
     */
    public boolean jumpPressed() {
        return nJumpPressed(handle);
    }

    private static native boolean nJumpPressed(long api);

    /**
     * jce.sprint
     * <p>Shape value_return, since 1; host member input_button.
     * The binding supplies button=1 (manifest bind_args), so it is not a
     * parameter here.
     */
    public boolean sprint() {
        return nSprint(handle);
    }

    private static native boolean nSprint(long api);

    /**
     * jce.attack_pressed
     * <p>Shape value_return, since 1; host member input_button.
     * The binding supplies button=2 (manifest bind_args), so it is not a
     * parameter here.
     */
    public boolean attackPressed() {
        return nAttackPressed(handle);
    }

    private static native boolean nAttackPressed(long api);

    /**
     * jce.set_time_scale
     * <p>Shape void_call, since 1; host member set_time_scale.
     * @param scale float
     */
    public void setTimeScale(float scale) {
        nSetTimeScale(handle, scale);
    }

    private static native void nSetTimeScale(long api, float scale);

    /**
     * jce.pause() with no argument pauses; jce.pause(false) resumes.
     * <p>Shape void_call, since 1; host member set_paused.
     * @param paused bool
     */
    public void pause(boolean paused) {
        nPause(handle, paused);
    }

    /** pause with the manifest defaults (paused=True). */
    public void pause() {
        pause(true);
    }

    private static native void nPause(long api, boolean paused);

    /**
     * jce.shake_camera
     * <p>Shape void_call, since 1; host member shake_camera.
     * @param amount float
     */
    public void shakeCamera(float amount) {
        nShakeCamera(handle, amount);
    }

    /** shakeCamera with the manifest defaults (amount=0.5). */
    public void shakeCamera() {
        shakeCamera(0.5f);
    }

    private static native void nShakeCamera(long api, float amount);

    /**
     * jce.music_set_intensity
     * <p>Shape void_call, since 1; host member music_set_intensity.
     * @param intensity float
     */
    public void musicSetIntensity(float intensity) {
        nMusicSetIntensity(handle, intensity);
    }

    private static native void nMusicSetIntensity(long api, float intensity);

    /**
     * jce.music_get_intensity
     * <p>Shape value_return, since 1; host member music_get_intensity.
     */
    public float musicGetIntensity() {
        return nMusicGetIntensity(handle);
    }

    private static native float nMusicGetIntensity(long api);

    /**
     * Absolute playhead time of the quantized switch; negative on miss or no
     * track, which is why the no-host value is -1 and not 0.
     * <p>Shape value_return, since 1; host member music_request_transition.
     * With no host this returns -1.0, not 0.
     * @param toSegment int
     */
    public float musicRequestTransition(int toSegment) {
        return nMusicRequestTransition(handle, toSegment);
    }

    private static native float nMusicRequestTransition(long api, int toSegment);

    /**
     * jce.gas_activate
     * <p>Shape value_return, since 1; host member gas_activate.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param abilityId uint32_t, unsigned; the bits round-trip exactly
     */
    public boolean gasActivate(long e, int abilityId) {
        return nGasActivate(handle, e, abilityId);
    }

    private static native boolean nGasActivate(long api, long e, int abilityId);

    /**
     * jce.gas_get
     * <p>Shape fallible_out, since 1; host member gas_get.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param attrName const char *
     */
    public Float gasGet(long e, String attrName) {
        float[] outFloat = new float[1];
        if (!nGasGet(handle, e, attrName, outFloat))
            return null;
        return Float.valueOf(outFloat[0]);
    }

    private static native boolean nGasGet(long api, long e, String attrName, float[] outFloat);

    /**
     * jce.gas_apply
     * <p>Shape value_return, since 1; host member gas_apply.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param attrName const char *
     * @param op int
     * @param magnitude float
     * @param durationSeconds float
     */
    public boolean gasApply(long e, String attrName, int op, float magnitude, float durationSeconds) {
        return nGasApply(handle, e, attrName, op, magnitude, durationSeconds);
    }

    /** gasApply with the manifest defaults (durationSeconds=0.0). */
    public boolean gasApply(long e, String attrName, int op, float magnitude) {
        return gasApply(e, attrName, op, magnitude, 0.0f);
    }

    private static native boolean nGasApply(long api, long e, String attrName, int op, float magnitude, float durationSeconds);

    /**
     * 8 values on a hit; a MISS pushes integer 0, not nil -- scripts branch
     * on `e == 0`.
     * <p>Shape fallible_out, since 1; host member raycast.
     * A miss is null. (The Lua binding renders the same miss as the number 0;
     * absence is uniformly null here.)
     * @param origin const float
     * @param dir const float
     * @param maxDist float
     */
    public RaycastResult raycast(float[] origin, float[] dir, float maxDist) {
        long[] outLong = new long[1];
        float[] outFloat = new float[7];
        if (!nRaycast(handle, origin, dir, maxDist, outLong, outFloat))
            return null;
        return new RaycastResult(outLong[0], java.util.Arrays.copyOfRange(outFloat, 0, 3), java.util.Arrays.copyOfRange(outFloat, 3, 6), outFloat[6]);
    }

    private static native boolean nRaycast(long api, float[] origin, float[] dir, float maxDist, long[] outLong, float[] outFloat);

    /**
     * jce.apply_impulse
     * <p>Shape void_call, since 1; host member apply_impulse.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param x float
     * @param y float
     * @param z float
     */
    public void applyImpulse(long e, float x, float y, float z) {
        nApplyImpulse(handle, e, x, y, z);
    }

    private static native void nApplyImpulse(long api, long e, float x, float y, float z);

    /**
     * jce.set_velocity
     * <p>Shape void_call, since 1; host member set_velocity.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param x float
     * @param y float
     * @param z float
     */
    public void setVelocity(long e, float x, float y, float z) {
        nSetVelocity(handle, e, x, y, z);
    }

    private static native void nSetVelocity(long api, long e, float x, float y, float z);

    /**
     * jce.anim_set_float
     * <p>Shape void_call, since 1; host member anim_set_float.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param name const char *
     * @param v float
     */
    public void animSetFloat(long e, String name, float v) {
        nAnimSetFloat(handle, e, name, v);
    }

    private static native void nAnimSetFloat(long api, long e, String name, float v);

    /**
     * jce.anim_set_int
     * <p>Shape void_call, since 1; host member anim_set_int.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param name const char *
     * @param v int
     */
    public void animSetInt(long e, String name, int v) {
        nAnimSetInt(handle, e, name, v);
    }

    private static native void nAnimSetInt(long api, long e, String name, int v);

    /**
     * jce.anim_set_bool
     * <p>Shape void_call, since 1; host member anim_set_bool.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param name const char *
     * @param v bool
     */
    public void animSetBool(long e, String name, boolean v) {
        nAnimSetBool(handle, e, name, v);
    }

    private static native void nAnimSetBool(long api, long e, String name, boolean v);

    /**
     * jce.anim_set_trigger
     * <p>Shape void_call, since 1; host member anim_set_trigger.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param name const char *
     */
    public void animSetTrigger(long e, String name) {
        nAnimSetTrigger(handle, e, name);
    }

    private static native void nAnimSetTrigger(long api, long e, String name);

    /**
     * jce.is_action_down
     * <p>Shape value_return, since 1; host member action_down.
     * @param name const char *
     */
    public boolean isActionDown(String name) {
        return nIsActionDown(handle, name);
    }

    private static native boolean nIsActionDown(long api, String name);

    /**
     * jce.is_action_pressed
     * <p>Shape value_return, since 1; host member action_pressed.
     * @param name const char *
     */
    public boolean isActionPressed(String name) {
        return nIsActionPressed(handle, name);
    }

    private static native boolean nIsActionPressed(long api, String name);

    /**
     * jce.get_axis
     * <p>Shape value_return, since 1; host member action_axis.
     * @param name const char *
     */
    public float getAxis(String name) {
        return nGetAxis(handle, name);
    }

    private static native float nGetAxis(long api, String name);

    /**
     * jce.get_pointer_delta
     * <p>Shape void_out_array, since 1; host member pointer_delta.
     */
    public float[] getPointerDelta() {
        float[] out = new float[2];
        nGetPointerDelta(handle, out);
        return out;
    }

    private static native void nGetPointerDelta(long api, float[] outFloat);

    /**
     * jce.get_pointer_wheel
     * <p>Shape value_return, since 1; host member pointer_wheel.
     */
    public float getPointerWheel() {
        return nGetPointerWheel(handle);
    }

    private static native float nGetPointerWheel(long api);

    /**
     * jce.is_pointer_down
     * <p>Shape value_return, since 1; host member pointer_button.
     * @param button int
     */
    public boolean isPointerDown(int button) {
        return nIsPointerDown(handle, button);
    }

    private static native boolean nIsPointerDown(long api, int button);

    /**
     * A host returning a negative count is clamped to 0 so `for i = 1,
     * jce.get_touch_count()` cannot underflow.
     * <p>Shape value_return, since 1; host member touch_count.
     * Clamped to a minimum of 0.
     */
    public int getTouchCount() {
        return nGetTouchCount(handle);
    }

    private static native int nGetTouchCount(long api);

    /**
     * 1-based Lua index mapped to 0-based C; an index below 1 returns nil
     * without calling the host.
     * <p>Shape fallible_out, since 1; host member touch_get.
     * Indices are 1-based, as on the Lua surface; anything below 1 answers
     * absent without calling the host.
     * @param index int
     */
    public GetTouchResult getTouch(int index) {
        if (index < 1)
            return null;
        long[] outLong = new long[1];
        float[] outFloat = new float[3];
        if (!nGetTouch(handle, index - 1, outLong, outFloat))
            return null;
        return new GetTouchResult(outLong[0], outFloat[0], outFloat[1], outFloat[2]);
    }

    private static native boolean nGetTouch(long api, int index, long[] outLong, float[] outFloat);

    /**
     * Passthrough is the contract, not a fallback: an unlocalized build shows
     * readable keys instead of blank UI.
     * <p>Shape value_return, since 1; host member loc_translate.
     * With no host this returns key itself.
     * @param key const char *
     */
    public String tr(String key) {
        String v = nTr(handle, key);
        return v == null ? "" : v;
    }

    private static native String nTr(long api, String key);

    /**
     * jce.get_locale
     * <p>Shape value_return, since 1; host member loc_get_locale.
     */
    public String getLocale() {
        String v = nGetLocale(handle);
        return v == null ? "" : v;
    }

    private static native String nGetLocale(long api);

    /**
     * jce.set_locale
     * <p>Shape void_call, since 1; host member loc_set_locale.
     * @param locale const char *
     */
    public void setLocale(String locale) {
        nSetLocale(handle, locale);
    }

    private static native void nSetLocale(long api, String locale);

    /**
     * jce.get_velocity
     * <p>Shape fallible_out, since 1; host member get_velocity.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     */
    public float[] getVelocity(long e) {
        float[] outFloat = new float[3];
        if (!nGetVelocity(handle, e, outFloat))
            return null;
        return outFloat;
    }

    private static native boolean nGetVelocity(long api, long e, float[] outFloat);

    /**
     * jce.vehicle_set_input
     * <p>Shape void_call, since 1; host member vehicle_set_input.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param throttle float
     * @param brake float
     * @param steer float
     */
    public void vehicleSetInput(long e, float throttle, float brake, float steer) {
        nVehicleSetInput(handle, e, throttle, brake, steer);
    }

    private static native void nVehicleSetInput(long api, long e, float throttle, float brake, float steer);

    /**
     * jce.vehicle_get_speed
     * <p>Shape value_return, since 1; host member vehicle_get_speed.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     */
    public float vehicleGetSpeed(long e) {
        return nVehicleGetSpeed(handle, e);
    }

    private static native float nVehicleGetSpeed(long api, long e);

    /**
     * jce.get_move
     * <p>Shape void_out_array, since 1; host member get_move.
     */
    public float[] getMove() {
        float[] out = new float[3];
        nGetMove(handle, out);
        return out;
    }

    private static native void nGetMove(long api, float[] outFloat);

    /**
     * jce.ui_get_slider
     * <p>Shape fallible_out, since 1; host member ui_get_slider.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     */
    public Float uiGetSlider(long e) {
        float[] outFloat = new float[1];
        if (!nUiGetSlider(handle, e, outFloat))
            return null;
        return Float.valueOf(outFloat[0]);
    }

    private static native boolean nUiGetSlider(long api, long e, float[] outFloat);

    /**
     * jce.ui_set_slider
     * <p>Shape void_call, since 1; host member ui_set_slider.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param v float
     */
    public void uiSetSlider(long e, float v) {
        nUiSetSlider(handle, e, v);
    }

    private static native void nUiSetSlider(long api, long e, float v);

    /**
     * jce.ui_get_toggle
     * <p>Shape fallible_out, since 1; host member ui_get_toggle.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     */
    public Boolean uiGetToggle(long e) {
        boolean[] outBoolean = new boolean[1];
        if (!nUiGetToggle(handle, e, outBoolean))
            return null;
        return Boolean.valueOf(outBoolean[0]);
    }

    private static native boolean nUiGetToggle(long api, long e, boolean[] outBoolean);

    /**
     * jce.ui_set_toggle
     * <p>Shape void_call, since 1; host member ui_set_toggle.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param v bool
     */
    public void uiSetToggle(long e, boolean v) {
        nUiSetToggle(handle, e, v);
    }

    private static native void nUiSetToggle(long api, long e, boolean v);

    /**
     * jce.ui_set_text
     * <p>Shape void_call, since 1; host member ui_set_text.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param txt const char *
     */
    public void uiSetText(long e, String txt) {
        nUiSetText(handle, e, txt);
    }

    private static native void nUiSetText(long api, long e, String txt);

    /**
     * jce.send_message
     * <p>Shape void_call, since 1; host member send_message.
     * @param target JceScriptEntity, unsigned; the bits round-trip exactly
     * @param msg const char *
     * @param numberArg double
     * @param strArg const char *
     */
    public void sendMessage(long target, String msg, double numberArg, String strArg) {
        nSendMessage(handle, target, msg, numberArg, strArg);
    }

    /** sendMessage with the manifest defaults (strArg=None). */
    public void sendMessage(long target, String msg, double numberArg) {
        sendMessage(target, msg, numberArg, null);
    }

    /** sendMessage with the manifest defaults (numberArg=0.0, strArg=None). */
    public void sendMessage(long target, String msg) {
        sendMessage(target, msg, 0.0, null);
    }

    private static native void nSendMessage(long api, long target, String msg, double numberArg, String strArg);

    /**
     * jce.broadcast
     * <p>Shape void_call, since 1; host member broadcast.
     * @param msg const char *
     * @param numberArg double
     * @param strArg const char *
     */
    public void broadcast(String msg, double numberArg, String strArg) {
        nBroadcast(handle, msg, numberArg, strArg);
    }

    /** broadcast with the manifest defaults (strArg=None). */
    public void broadcast(String msg, double numberArg) {
        broadcast(msg, numberArg, null);
    }

    /** broadcast with the manifest defaults (numberArg=0.0, strArg=None). */
    public void broadcast(String msg) {
        broadcast(msg, 0.0, null);
    }

    private static native void nBroadcast(long api, String msg, double numberArg, String strArg);

    /**
     * jce.has_component
     * <p>Shape value_return, since 1; host member has_component.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param compName const char *
     */
    public boolean hasComponent(long e, String compName) {
        return nHasComponent(handle, e, compName);
    }

    private static native boolean nHasComponent(long api, long e, String compName);

    /**
     * jce.is_component_enabled
     * <p>Shape value_return, since 1; host member is_component_enabled.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param compName const char *
     */
    public boolean isComponentEnabled(long e, String compName) {
        return nIsComponentEnabled(handle, e, compName);
    }

    private static native boolean nIsComponentEnabled(long api, long e, String compName);

    /**
     * jce.set_component_enabled
     * <p>Shape void_call, since 1; host member set_component_enabled.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param compName const char *
     * @param on bool
     */
    public void setComponentEnabled(long e, String compName, boolean on) {
        nSetComponentEnabled(handle, e, compName, on);
    }

    private static native void nSetComponentEnabled(long api, long e, String compName, boolean on);

    /**
     * jce.net_is_server
     * <p>Shape value_return, since 1; host member net_is_server.
     */
    public boolean netIsServer() {
        return nNetIsServer(handle);
    }

    private static native boolean nNetIsServer(long api);

    /**
     * jce.net_is_client
     * <p>Shape value_return, since 1; host member net_is_client.
     */
    public boolean netIsClient() {
        return nNetIsClient(handle);
    }

    private static native boolean nNetIsClient(long api);

    /**
     * jce.net_spawn
     * <p>Shape value_return, since 1; host member net_spawn.
     * @param prefabPath const char *
     * @param x float
     * @param y float
     * @param z float
     */
    public long netSpawn(String prefabPath, float x, float y, float z) {
        return nNetSpawn(handle, prefabPath, x, y, z);
    }

    private static native long nNetSpawn(long api, String prefabPath, float x, float y, float z);

    /**
     * jce.rpc_send
     * <p>Shape value_return, since 1; host member rpc_send.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param event const char *
     * @param target int
     * @param payload const char *
     */
    public boolean rpcSend(long e, String event, int target, String payload) {
        return nRpcSend(handle, e, event, target, payload);
    }

    /** rpcSend with the manifest defaults (payload=None). */
    public boolean rpcSend(long e, String event, int target) {
        return rpcSend(e, event, target, null);
    }

    /** rpcSend with the manifest defaults (target=0, payload=None). */
    public boolean rpcSend(long e, String event) {
        return rpcSend(e, event, 0, null);
    }

    private static native boolean nRpcSend(long api, long e, String event, int target, String payload);

    /**
     * jce.particle_burst
     * <p>Shape void_call, since 1; host member particle_burst.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param count int
     */
    public void particleBurst(long e, int count) {
        nParticleBurst(handle, e, count);
    }

    private static native void nParticleBurst(long api, long e, int count);

    /**
     * jce.particle_set_emitting
     * <p>Shape void_call, since 1; host member particle_set_emitting.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param on bool
     */
    public void particleSetEmitting(long e, boolean on) {
        nParticleSetEmitting(handle, e, on);
    }

    private static native void nParticleSetEmitting(long api, long e, boolean on);

    /**
     * jce.particle_set_color
     * <p>Shape void_call, since 1; host member particle_set_color.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param r float
     * @param g float
     * @param b float
     */
    public void particleSetColor(long e, float r, float g, float b) {
        nParticleSetColor(handle, e, r, g, b);
    }

    private static native void nParticleSetColor(long api, long e, float r, float g, float b);

    /**
     * first-or-nil AND a match count, so a strict scene director can reject
     * duplicate authored names. IDENTICAL C signature to find_by_prefix and a
     * DIFFERENT contract.
     * <p>Shape first_and_count, since 1; host member find_by_name.
     * At most 2 results (manifest out_capacity).
     * @param name const char *
     */
    public FindByNameResult findByName(String name) {
        long[] buf = new long[2];
        int n = nFindByName(handle, name, buf);
        return new FindByNameResult(n > 0 ? Long.valueOf(buf[0]) : null, n);
    }

    private static native int nFindByName(long api, String name, long[] out);

    /**
     * One Lua array. IDENTICAL C signature to find_by_name and a DIFFERENT
     * contract.
     * <p>Shape entity_table, since 1; host member find_by_prefix.
     * At most 1024 results (manifest out_capacity).
     * @param prefix const char *
     */
    public long[] findByPrefix(String prefix) {
        long[] buf = new long[1024];
        int n = nFindByPrefix(handle, prefix, buf);
        if (n <= 0) return EMPTY_ENTITIES;
        if (n > 1024) n = 1024;
        return java.util.Arrays.copyOf(buf, n);
    }

    private static native int nFindByPrefix(long api, String prefix, long[] out);

    /**
     * jce.comp_get
     * <p>Shape owned_string_release, since 1; host member comp_get_json.
     * The host's string is released by the C ABI before it returns, so
     * nothing here owns native memory.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param type const char *
     */
    public String compGet(long e, String type) {
        return nCompGet(handle, e, type);
    }

    private static native String nCompGet(long api, long e, String type);

    /**
     * jce.comp_set
     * <p>Shape value_return, since 1; host member comp_set_json.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param type const char *
     * @param json const char *
     */
    public boolean compSet(long e, String type, String json) {
        return nCompSet(handle, e, type, json);
    }

    private static native boolean nCompSet(long api, long e, String type, String json);

    /**
     * jce.render_get
     * <p>Shape owned_string_release, since 1; host member render_get_json.
     * The host's string is released by the C ABI before it returns, so
     * nothing here owns native memory.
     */
    public String renderGet() {
        return nRenderGet(handle);
    }

    private static native String nRenderGet(long api);

    /**
     * jce.render_set
     * <p>Shape value_return, since 1; host member render_set_json.
     * @param json const char *
     */
    public boolean renderSet(String json) {
        return nRenderSet(handle, json);
    }

    private static native boolean nRenderSet(long api, String json);

    /**
     * jce.audio_set_volume
     * <p>Shape void_call, since 1; host member audio_set_volume.
     * @param e JceScriptEntity, unsigned; the bits round-trip exactly
     * @param volume float
     */
    public void audioSetVolume(long e, float volume) {
        nAudioSetVolume(handle, e, volume);
    }

    private static native void nAudioSetVolume(long api, long e, float volume);

}
