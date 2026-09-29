using JceScript;

/* Hud.cs -- MODULE 7 of 7: THE HUD, in C#.
 *
 * Owns every word on screen: SCORE and BEST in all four states (spec 6),
 * and the overlay pair that says which state the game is in (spec 5, 7).
 *
 * THIS MODULE CANNOT PRINT.  `log` is one of the eight hand-written
 * bindings in contracts/script-api.json, so each backend writes it by hand
 * and C# has none -- Jce.g.cs is 101 bindings and not one of them is Log.
 * So there is no line this file could emit to prove it ran, and its only
 * evidence is the text itself.  verify.py therefore checks the HUD by
 * reading the UIText back, not by grepping the log.  That is the right
 * shape anyway: a log line proves a script executed, while reading the
 * text back proves the thing the player sees is correct.
 *
 * The class name must match the file name: a Script component stores a
 * PATH, "scripts/Hud.cs" resolves to the TYPE Hud, and the bytes at that
 * path are never read -- .cs is a REFERENCE form.  This file is compiled
 * into SnakeScripts.dll and the game loads that assembly at startup.
 */
public class Hud : JceEntityScript
{
    private const int StMenu = 0;
    private const int StPlaying = 1;
    private const int StPaused = 2;
    private const int StGameOver = 3;

    private uint _state, _meta, _score, _best, _title, _hint;

    private int _shownState = -1;
    private int _shownScore = -1;
    private int _shownBest = -1;

    public override void OnStart()
    {
        _state = Find("GameState");
        _meta = Find("GameMeta");
        _score = Find("ScoreText");
        _best = Find("BestText");
        _title = Find("StateText");
        _hint = Find("HintText");
    }

    private static uint Find(string name)
    {
        var f = Jce.FindByName(name);
        return (f.Count > 0 && f.First.HasValue) ? f.First.Value : 0u;
    }

    public override void OnUpdate(float dt)
    {
        if (_state == 0u || _meta == 0u) return;
        if (!Jce.TryGetPosition(_state, out var s)) return;
        if (!Jce.TryGetPosition(_meta, out var m)) return;

        int state = (int)System.MathF.Round(s.Item1);
        int score = (int)System.MathF.Round(s.Item2);
        int best = (int)System.MathF.Round(m.Item1);

        // A run's best is not written back to GameMeta until the game ends,
        // so show the live maximum -- a player who has just beaten their
        // record should see it while they are still playing, not after they
        // die.
        if (score > best) best = score;

        // Only write when something changed.  UiSetText crosses the managed
        // boundary and this runs every frame for four labels.
        if (score != _shownScore)
        {
            _shownScore = score;
            if (_score != 0u) Jce.UiSetText(_score, "SCORE " + score);
        }
        if (best != _shownBest)
        {
            _shownBest = best;
            if (_best != 0u) Jce.UiSetText(_best, "BEST " + best);
        }
        if (state == _shownState && state != StGameOver) return;
        _shownState = state;

        string title, hint;
        switch (state)
        {
            case StPlaying:
                title = "";
                hint = "";
                break;
            case StPaused:
                title = "PAUSED";
                hint = "P / ESC  RESUME      ENTER  RESTART      Q  QUIT TO MENU";
                break;
            case StGameOver:
                title = "GAME OVER";
                hint = "SCORE " + score + "      BEST " + best
                     + "      ENTER  PLAY AGAIN";
                break;
            default:
                title = "SNAKE";
                hint = "PRESS ENTER TO START      ARROWS / WASD TO STEER";
                break;
        }
        if (_title != 0u) Jce.UiSetText(_title, title);
        if (_hint != 0u) Jce.UiSetText(_hint, hint);
    }
}
