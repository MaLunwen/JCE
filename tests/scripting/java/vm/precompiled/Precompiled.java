/*
 * Precompiled.java — the fixture for the SHIPPED-JAVA shape.
 *
 * The test never reads this file.  The test build compiles it to bytecode and
 * the test loads the resulting .class, because a shipped Java game carries
 * bytecode and not sources: source form needs a JDK on the player's machine,
 * bytecode runs on a plain JRE.  jce_script_vm_java_register() claims BOTH
 * ".java" and ".class" for exactly that reason, and
 * JceScriptRuntime.instantiate() picks the form from the class-file magic
 * rather than from the extension.
 *
 * DELIBERATELY BORING.  Everything it does is already proven for the SOURCE
 * path by the other tests in this suite; the only thing under test here is
 * that a script delivered as BYTECODE through the path slot is recognised,
 * instantiated, and dispatched to.  If this file grows a second idea, a
 * failure stops telling you which half broke.
 *
 * The class name deliberately does NOT match the file name the test loads it
 * under — see test_a_precompiled_class_runs_through_the_path_slot.
 */
import com.jce.script.vm.JceEntityScript;

public class Precompiled extends JceEntityScript {

    @Override
    public void onStart() {
        log("precompiled onStart " + entity());
        /* Observable through the host's set_position recorder, so the test has
         * a typed observation and not only a log line: the C side checks the
         * entity id and the payload it was called with. */
        setPosition(entity(), 11.0f, 22.0f, 33.0f);
    }

    @Override
    public void onUpdate(float dt) {
        log("precompiled onUpdate");
    }
}
