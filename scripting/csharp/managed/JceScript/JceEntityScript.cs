namespace JceScript;

/// <summary>
/// What a C# gameplay script derives from.
///
/// <para>The class name must match the file name, because a Script component
/// stores a PATH and "Assets/Turret.cs" is resolved to the type
/// <c>Turret</c>.  That is Unity's convention and it is the whole reason .cs
/// is registered as a REFERENCE form: the engine never reads the bytes at
/// that path, it reads the name.</para>
///
/// <para>Every override may throw.  The dispatcher catches it, reports it to
/// the host log, and keeps the instance alive — a throwing OnUpdate does not
/// take down the frame, the scene, or the other scripts, and it does not
/// disable itself either, so the message repeats until it is fixed.</para>
///
/// <para>Reach the engine through <see cref="Jce"/>, the generated surface.
/// It is static because the C ABI handle belongs to the VM rather than to any
/// one instance, and a script that held its own copy would be one more thing
/// to get wrong at reload.</para>
/// </summary>
public abstract class JceEntityScript
{
    private Vm? _vm;

    /// <summary>The entity this instance is attached to.  Set once, when the
    /// VM instantiates it, and again on a reload — a rebind preserves the
    /// entity and nothing else.</summary>
    public uint Entity { get; private set; }

    internal void Bind(Vm vm, uint entity)
    {
        _vm = vm;
        Entity = entity;
        Jce.Use(vm.Api);
    }

    /// <summary>Called once, after the component is attached and before the
    /// first OnUpdate.</summary>
    public virtual void OnStart() { }

    /// <summary>Called every frame with the delta time in seconds.</summary>
    public virtual void OnUpdate(float dt) { }

    /// <summary>Called once per PHYSICS step with the FIXED delta time, just
    /// before that step runs — Unity's FixedUpdate.  Zero or many times per
    /// rendered frame, and always the same dt, so a force applied here
    /// produces the same motion at 30 Hz and at 144 Hz.  Use
    /// <see cref="OnUpdate"/> for anything that should happen once per drawn
    /// frame.</summary>
    public virtual void OnFixedUpdate(float dt) { }

    /// <summary>Called when the owning entity's collider touches
    /// <paramref name="other"/>.</summary>
    public virtual void OnCollision(uint other) { }

    /// <summary>Every message reaches this one method, whatever its name.
    /// The four backends route messages through one thunk because the name
    /// comes from the CALLER and has no identity they share.</summary>
    public virtual void OnMessage(string name, double number, string? text) { }

    /// <summary>An animation frame event: the five scalars, not the engine's
    /// struct, so a script sees the same shape in every language.</summary>
    public virtual void OnAnimEvent(uint id, string? name, float f0, float f1,
                                    int i0)
    { }

    /// <summary>The VM this instance belongs to — for the generated surface,
    /// not for scripts.</summary>
    internal Vm? Owner => _vm;
}
