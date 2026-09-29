/*
 * EsCampfire.java — the second campfire light, in Java.
 *
 * WHAT THE SCENE WAS MISSING.  gen_scene.py authors TWO campfire point lights,
 * CampfireLight and CampfireLight2.  The Lua director resolves exactly one of
 * them (`ids.fire = jce.find_by_name("CampfireLight")`) and flickers it every
 * frame; CampfireLight2 appears in no line of es_director.lua at all.  It has
 * therefore sat at its authored intensity since it was added — it does not
 * flicker, and it does NOT go out when the rain douses the fire, so in rainy
 * weather the camp is lit by a steady point light with no visible source.
 * That is the defect this script fixes, and it is why the work is real rather
 * than a demonstration: something in the scene was wrong before it existed.
 *
 * IT DOES NOT REIMPLEMENT THE DIRECTOR'S FLICKER, IT READS IT.  Every frame
 * this script does compGet("Light") on CampfireLight — the light the DIRECTOR
 * owns — and takes its live intensity as the base.  Two consequences fall out
 * for free rather than being copied:
 *
 *   - rain: the director's own `base = is_raining() and 0.0 or 10.0` makes
 *     CampfireLight's intensity collapse to ~0, so the mirror collapses too.
 *     This file contains no weather logic and no `is_raining` of its own,
 *     which is the point — a second copy of that predicate is a second thing
 *     to keep in step.
 *   - the season/day-night tween: whatever the director does to the fire, the
 *     second light follows one beat behind.
 *
 * The phase is deliberately offset and the amplitude deliberately smaller, so
 * the two lights are a fire and not one light drawn twice.
 *
 * A JAVA SCRIPT'S PATH IS A FILE PATH.  "scripts/EsCampfire.java" is read
 * through JceScriptHost::read_file exactly the way es_director.lua is, and the
 * BYTES pick the form: class-file magic loads pre-compiled bytecode, anything
 * else is compiled in memory by the JDK at instantiate.  This project ships
 * SOURCE, so a JDK — not a JRE — is required at run time; shipping the
 * compiled form would change this file's extension to .class and nothing else,
 * because jce_script_vm_java_register() claims BOTH.  The class name is taken
 * from the first top-level declaration below, NOT from the file name.
 *
 * WHY JAVA AND NOT LUA.  Honestly: because a real project has a team, and a
 * team has people who write Java.  This script is small on purpose — the case
 * being proved is that a JVM script sits in the same scene as a Lua one and
 * reads the same components through the same bridge, not that Java is faster
 * at trigonometry.
 */
import com.jce.script.JceScript;
import com.jce.script.vm.JceEntityScript;
import com.jce.script.vm.JceScriptSurface;

class EsCampfire extends JceEntityScript {

    /* The authored second-light colour and radius, copied VERBATIM from
     * gen_scene.py's CampfireLight2 (0.97, 0.50, 0.18 / radius 1.0).  The
     * whole component is written every frame because compSet REPLACES —
     * unlike render_set, which merges (rs_json_merge) — so an object carrying
     * only `intensity` would silently reset the colour to black and the
     * radius to zero.  That is the single easiest way to break a light from a
     * script, and it looks like "the light stopped working" rather than like
     * a bad write. */
    private static final String LIGHT_HEAD =
        "{\"lightType\":1,\"colorR\":0.97,\"colorG\":0.50,\"colorB\":0.18,"
      + "\"radius\":1.00,\"castsShadow\":false,\"intensity\":";

    /* Flicker: three incommensurate rates, the same shape the director uses,
     * with a phase offset so the two lights never peak together. */
    private static final float PHASE = 1.9f;
    private static final float AMPLITUDE = 0.26f;   /* director uses 0.40 */

    private JceScript api;
    private long fireA;       /* CampfireLight  — the director's, READ only  */
    private long fireB;       /* CampfireLight2 — ours, written every frame  */
    private long probe;

    private int starts;
    private int frames;
    private int found;
    private float t;

    @Override
    public void onStart() {
        api = JceScriptSurface.of(this);
        starts = starts + 1;

        fireA  = first("CampfireLight");
        fireB  = first("CampfireLight2");
        probe  = first("EsJavaProbe");

        if (fireA != 0L)  found = found + 1;
        if (fireB != 0L)  found = found + 1;

        if (probe != 0L) {
            api.setPosition(probe, (float) starts, 0.0f, (float) found);
        }
        log("es_campfire (java) online: CampfireLight="
            + (fireA != 0L ? "ok" : "MISSING")
            + " CampfireLight2=" + (fireB != 0L ? "ok" : "MISSING")
            + " probe=" + (probe != 0L ? "ok" : "MISSING"));
    }

    @Override
    public void onUpdate(float dt) {
        frames = frames + 1;
        t = t + dt;

        if (fireB != 0L) {
            /* The director's live intensity is the base.  Absent component ->
             * null, tested for null rather than for truth: an empty string is
             * not what "no component" looks like here, and writing the
             * condition that only happens to agree is how a port acquires a
             * difference nobody can see. */
            float base = 0.0f;
            String json = (fireA != 0L) ? api.compGet(fireA, "Light") : null;
            if (json != null) {
                base = readNumber(json, "\"intensity\":", 0.0f);
            }

            float f = (float) (Math.sin((t + PHASE) * 10.0) * 0.5
                             + Math.sin((t + PHASE) * 23.0) * 0.3
                             + Math.sin((t + PHASE) * 41.0) * 0.2);
            /* 0.62 of the director's light: a second, smaller fire-lit face
             * of the pit, not a duplicate of the first. */
            float lit = base * 0.62f * (1.0f + AMPLITUDE * f);
            if (lit < 0.0f) lit = 0.0f;

            api.compSet(fireB, "Light", LIGHT_HEAD + lit + "}");
        }

        if (probe != 0L) {
            api.setPosition(probe, (float) starts, (float) frames,
                            (float) found);
        }
    }

    /* Minimal number scan: find `key` in `json` and read the decimal that
     * follows.  Not a JSON parser and not trying to be — the only field this
     * script reads is a top-level number written by the engine's own
     * serializer, and pulling in a parser to read one float would be the
     * larger risk. */
    private static float readNumber(String json, String key, float absent) {
        int i = json.indexOf(key);
        if (i < 0) return absent;
        int s = i + key.length();
        int e = s;
        while (e < json.length()) {
            char c = json.charAt(e);
            if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+'
                || c == 'e' || c == 'E') {
                e++;
            } else {
                break;
            }
        }
        if (e == s) return absent;
        try {
            return Float.parseFloat(json.substring(s, e));
        } catch (NumberFormatException ex) {
            return absent;
        }
    }

    private long first(String name) {
        JceScript.FindByNameResult r = api.findByName(name);
        return r.first != null ? r.first.longValue() : 0L;
    }
}
