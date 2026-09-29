import com.jce.script.JceScript;
import com.jce.script.vm.JceEntityScript;
import com.jce.script.vm.JceScriptSurface;

/**
 * GameBoard.java -- MODULE 4 of 7: THE BOARD, in Java.
 *
 * Owns one rule: the head must stay inside 30 x 20 (spec 11). It writes a
 * single verdict slot and decides nothing else -- not what happens next,
 * not the score, not whether the game ends. The state machine in C reads
 * this verdict and decides.
 *
 * <p>A NOTE ON WHAT JAVA CAN DO HERE, because I had it wrong. An earlier
 * comment in this project claimed the Java surface was nine bindings and
 * could not look an entity up by name, so this module was written to read
 * only its own transform. That was false: {@code JceScript} exposes 103
 * methods including {@code findByName}, {@code uiSetText} and
 * {@code saveGame}. The authoritative count is {@code declared_totals} in
 * contracts/script-api.json -- 99 generated plus 8 hand-written -- and not
 * any one language's file. The cost of believing the wrong number was a
 * module shaped around a restriction that did not exist.
 *
 * <p>What IS true, and is a different fact: {@code log} is one of the 8
 * hand-written bindings, so each backend writes it by hand and coverage is
 * uneven. Java has it (on this base class). C# does not have it at all.
 * That is why no module in this game proves it ran by printing.
 */
public class GameBoard extends JceEntityScript {

    private static final int COLS = 30;   // spec 11
    private static final int ROWS = 20;

    private static final float HX = (COLS - 1) / 2.0f;
    private static final float HZ = (ROWS - 1) / 2.0f;

    private JceScript api;
    private long stateE, headE;
    private float lastRun = -1.0f;

    private static int toCellX(float x) { return Math.round(x + HX); }
    private static int toCellZ(float z) { return Math.round(z + HZ); }

    @Override
    public void onStart() {
        api = JceScriptSurface.of(this);
        stateE = first("GameState");
        headE = first("SnakeHead");
        log("SNAKE java: board referee up; bounds are 0.." + (COLS - 1)
            + " x 0.." + (ROWS - 1));
    }

    private long first(String name) {
        JceScript.FindByNameResult r = api.findByName(name);
        return (r == null || r.first == null) ? 0L : r.first;
    }

    @Override
    public void onUpdate(float dt) {
        if (api == null || headE == 0L || stateE == 0L) return;

        float[] st = api.getPosition(stateE);
        if (st == null) return;

        // A new run clears this module's verdict. Nothing else clears it,
        // because nothing else knows whether this module had written one.
        float[] meta = api.getPosition(first("GameMeta"));
        if (meta != null && meta[2] != lastRun) {
            lastRun = meta[2];
            float[] v = api.getPosition(entity());
            if (v != null) api.setPosition(entity(), 0.0f, v[1], v[2]);
            return;
        }

        if (Math.round(st[0]) != 1) return;       // only PLAYING is judged

        float[] hp = api.getPosition(headE);
        if (hp == null) return;

        int cx = toCellX(hp[0]);
        int cz = toCellZ(hp[2]);
        boolean out = cx < 0 || cx >= COLS || cz < 0 || cz >= ROWS;

        float[] v = api.getPosition(entity());
        if (v == null) return;
        float want = out ? 1.0f : 0.0f;
        if (v[0] != want) {
            // Read-modify-write: the other two slots belong to C++ and to
            // Python, and setPosition writes all three.
            api.setPosition(entity(), want, v[1], v[2]);
            if (out) {
                log("SNAKE java: head left the board at (" + cx + "," + cz
                    + ")");
            }
        }
    }
}
