/* lifecycle_v2.java — the hot-reload target for scripts/lifecycle.java, and
 * the Java expression of scripts/lifecycle_v2.lua.
 *
 * A Java object's class cannot change, so rebind swaps the BEHAVIOUR OBJECT
 * and carries the state map across.  That is why per-instance state lives in
 * set()/number() and not in fields: a field would be re-initialised by the
 * swap, which is the one thing rebind exists not to do.  on_update reports 21
 * (not 20) to prove the new methods are live, and keeps counting from where
 * the old ones left off to prove the state survived.
 */
import com.jce.script.vm.JceEntityScript;

class LifecycleV2 extends JceEntityScript {

    void report(int code, float a, float b) {
        setPosition(code, a, b, 0.0f);
    }

    public void onUpdate(float dt) {
        double count = number("count", 0.0) + 1.0;
        set("count", count);
        report(21, (float) count, dt);
    }

    public void onDestroy() {
        report(61, (float) number("count", 0.0), 0.0f);
    }
}
