/* lifecycle.java — the SAME script as scripts/lifecycle.lua, in Java.
 *
 * Read them side by side.  Every report(code, a, b) here has a report(code, a,
 * b) there with the same code, in the same order, from the same hook.
 *
 * The file is named for the SCRIPT, not for the class, and the class is
 * package-private: the VM compiles from source text and never from disk, so
 * javac's "public class must live in a file of the same name" rule does not
 * apply — and requiring `public class Foo` in `Foo.java` would be this backend
 * inventing a constraint the scripting model does not have.  The differential
 * needs the two files to share a stem (`lifecycle.lua` / `lifecycle.java`) so
 * that the host's read_file trace is comparable with the extension stripped
 * and nothing else normalised away.
 */
import com.jce.script.vm.JceEntityScript;

class Lifecycle extends JceEntityScript {

    /* A named handler is `public static` — the Java spelling of a Lua global
     * function — and a static method has no instance to reach the engine
     * through.  Lua's globals close over the chunk's locals; a Java script
     * stashes itself.  The VM does not do this for you on purpose: a magic
     * injected field would be a second definition of instance state. */
    static Lifecycle SELF;

    void report(int code, float a, float b) {
        setPosition(code, a, b, 0.0f);
    }

    public void onStart() {
        SELF = this;
        set("count", 0.0);
        set("boom", 0.0);
        log("start");
        report(1, entity(), 0.0f);

        float[] p = new float[3];
        boolean ok = getPosition(entity(), p);
        report(2, ok ? p[0] : -1.0f, ok ? p[1] : -1.0f);

        /* The same call that fails.  Java sees `false` with `p` untouched
         * where Lua sees nil; reporting the discrimination as 1/0 is how two
         * spellings of absence answer one question. */
        boolean missing = getPosition(4242L, p);
        report(3, missing ? 1.0f : 0.0f, 0.0f);

        /* Lua's start_coroutine runs the body up to its first wait_seconds
         * immediately, so the first step happens HERE and the second is
         * scheduled.  after() cannot suspend a method, so the same timeline is
         * written as "do the first part, schedule the rest". */
        report(10, 0.0f, 0.0f);
        after(0.5f, new Runnable() {
            @Override
            public void run() {
                report(11, 0.0f, 0.0f);
            }
        });
    }

    public void onUpdate(float dt) {
        double count = number("count", 0.0) + 1.0;
        set("count", count);
        report(20, (float) count, dt);
        if (number("boom", 0.0) != 0.0) {
            set("boom", 0.0);
            throw new IllegalStateException("update boom");
        }
    }

    /* Code 21, beside onUpdate's 20 -- see the Lua side for why the dt is
       reported rather than a bare call. */
    public void onFixedUpdate(float dt) {
        report(21, 1.0f, dt);
    }

    public void onCollision(long other) {
        report(30, other, 0.0f);
    }

    /* Three states of the string argument, three answers. */
    public void ping(double n, String s) {
        int tag = (s == null) ? 0 : (s.isEmpty() ? 1 : 2);
        report(40, (float) n, tag);
    }

    /* Declared with NO parameters although the dispatcher passes two — the
     * resolver takes the longest prefix a candidate accepts, which is what Lua
     * does by dropping extra arguments. */
    public void arm_boom() {
        set("boom", 1.0);
        report(41, 1.0f, 0.0f);
    }

    public void onAnimEvent(int id, String name, float f0, float f1, int i0) {
        int tag = (name == null) ? 0 : (name.isEmpty() ? 1 : 2);
        report(50, id, tag);
        report(51, f0, f1);
        report(52, i0, 0.0f);
    }

    public void onDestroy() {
        report(60, (float) number("count", 0.0), 0.0f);
    }

    /* Global handlers: what UIButton / UISlider / UIInputField dispatch into. */

    public static void on_ping(long e) {
        SELF.report(70, e, 0.0f);
    }

    public static void on_value(long e, double v) {
        SELF.report(71, e, (float) v);
    }

    public static void on_text(long e, String s) {
        SELF.report(72, e, s == null ? -1.0f : (float) s.length());
    }

    public static void on_boom(long e) {
        throw new IllegalStateException("boom from on_boom");
    }
}
