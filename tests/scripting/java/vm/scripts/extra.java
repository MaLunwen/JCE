/* extra.java — the Java expression of scripts/extra.lua.
 *
 * Loaded through jce_script_instantiate, i.e. through JceScriptHost::read_file,
 * so it also exercises the source-vs-class-file dispatch: these bytes are
 * SOURCE, and the VM compiles them because they do not start with the
 * class-file magic.
 */
import com.jce.script.vm.JceEntityScript;

class Extra extends JceEntityScript {

    public void onStart() {
        setPosition(80L, entity(), 0.0f, 0.0f);
    }
}
