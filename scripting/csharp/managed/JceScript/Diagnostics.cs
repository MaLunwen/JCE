namespace JceScript.Diagnostics;

/// <summary>
/// A script that ships INSIDE JceScript.dll.
///
/// <para>It exists so the backend can be exercised end to end with no second
/// .NET project: a test — and the SDK smoke — instantiates
/// "JceScript.Diagnostics.SelfTestScript" and gets a real instantiate, a real
/// dispatch and a real barrier, over the same code path a game's own type
/// takes.  Without it, verifying this backend would require building a
/// managed project from inside the native test, and a test that needs the
/// .NET SDK is a test that does not run on most machines.</para>
///
/// <para>It is deliberately NOT a mock: nothing here is special-cased by the
/// runtime, and the only reason it is reachable is that Vm's constructor adds
/// its own assembly to the search list.</para>
/// </summary>
public sealed class SelfTestScript : JceEntityScript
{
    /// <summary>How many times each callback ran, across every instance.
    /// Static because the native side has no handle to a managed object and
    /// a test needs a value it can read back through a named handler.</summary>
    public static int Starts;
    public static int Updates;
    public static float LastDt;
    public static int Collisions;
    public static uint LastOther;
    public static int Messages;
    public static string LastMessage = string.Empty;
    public static double LastNumber;
    public static string? LastText;
    public static int AnimEvents;
    public static uint LastEntitySeen;

    /// <summary>Zeroes the counters.  A public static method of a script
    /// type, so it is ALSO reachable as a named handler — which is what makes
    /// it callable from the native side at all.</summary>
    public static void ResetSelfTest(uint _)
    {
        Starts = Updates = Collisions = Messages = AnimEvents = 0;
        LastDt = 0f;
        LastOther = 0u;
        LastMessage = string.Empty;
        LastNumber = 0.0;
        LastText = null;
        LastEntitySeen = 0u;
    }

    /// <summary>Records the entity it was called with, so a test can prove
    /// call_named passed the argument through rather than merely finding a
    /// method of the right name.</summary>
    public static void RecordEntity(uint e) => LastEntitySeen = e;

    public static void RecordNumber(uint e, double v)
    {
        LastEntitySeen = e;
        LastNumber = v;
    }

    public static void RecordText(uint e, string? s)
    {
        LastEntitySeen = e;
        LastText = s;
    }

    public override void OnStart()
    {
        ++Starts;
        LastEntitySeen = Entity;
        // The counters above are invisible to a native test: nothing can read
        // a managed static across the boundary.  So this ALSO drives the
        // engine, which puts the result somewhere the test's host recorded it
        // -- and does it in both directions from one callback: a fallible_out
        // read, then a void_call write derived from what it read.
        if (Jce.TryGetPosition(Entity, out var p))
            Jce.SetPosition(Entity, p.Item1 + 1f, p.Item2 + 2f, p.Item3 + 3f);
        else
            Jce.SetPosition(Entity, -1f, -1f, -1f);
    }

    public override void OnUpdate(float dt)
    {
        ++Updates;
        LastDt = dt;
        Jce.SetTimeScale(dt);
    }

    public override void OnCollision(uint other)
    {
        ++Collisions;
        LastOther = other;
        Jce.SetPosition(other, 9f, 9f, 9f);
    }

    public override void OnMessage(string name, double number, string? text)
    {
        ++Messages;
        LastMessage = name;
        LastNumber = number;
        LastText = text;
        // A string ARGUMENT across the boundary, which is the one marshalling
        // direction the other callbacks do not exercise.
        Jce.UiSetText(Entity, name);
    }

    public override void OnAnimEvent(uint id, string? name, float f0, float f1,
                                     int i0)
    {
        ++AnimEvents;
    }
}

/// <summary>A script whose OnStart always throws.
///
/// <para>The barrier's whole claim is that this cannot reach the engine — an
/// exception escaping an [UnmanagedCallersOnly] frame would fail fast and
/// kill the process, so "the test still runs" IS the assertion, and the host
/// log line is what proves the throw was reported rather than swallowed.</para>
/// </summary>
public sealed class ThrowingScript : JceEntityScript
{
    public override void OnStart()
        => throw new InvalidOperationException("deliberate: barrier test");

    public override void OnUpdate(float dt)
        => throw new InvalidOperationException("deliberate: barrier test");
}

/// <summary>A type that does NOT derive from JceEntityScript, so
/// instantiating it must be refused with a reason rather than crash.</summary>
public sealed class NotAScript
{
}
