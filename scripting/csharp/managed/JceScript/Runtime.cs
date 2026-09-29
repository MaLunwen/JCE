using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

namespace JceScript;

/// <summary>
/// The managed half of the "csharp" JceScriptVM.
///
/// <para>THE EXCEPTION BARRIER IS HERE AND NOWHERE ELSE.  A .NET exception
/// that escapes an <c>[UnmanagedCallersOnly]</c> method does not unwind into
/// the C frame below it — the runtime FAILS FAST and kills the process.  So
/// every entry point in this file is a try/catch with no rethrow, and the
/// native side never sees an error it would have to interpret.  This is the
/// same placement scripting/cpp uses for its noexcept + catch(...) thunks:
/// on the module side of the boundary.</para>
///
/// <para>A caught exception is REPORTED, not swallowed: it goes to the host
/// log through the C ABI, which is where a designer looks.  A barrier that
/// only prevented the crash would leave a handler that stopped running with
/// no reason anywhere.</para>
/// </summary>
public static unsafe class Runtime
{
    /// <summary>Mirrors JceCsBridge in jce_script_vm_csharp.c, field for
    /// field.  APPEND ONLY: the native side checks <see cref="Size"/> against
    /// its own sizeof and refuses a short table, so a mismatch is a loud
    /// startup failure rather than a garbage function pointer.</summary>
    [StructLayout(LayoutKind.Sequential)]
    private struct Bridge
    {
        public int Size;
        public int Abi;

        public delegate* unmanaged<void*, long, void*, void*, int> VmOpen;
        public delegate* unmanaged<int, void> VmClose;

        public delegate* unmanaged<int, byte*, uint, uint> Instantiate;
        public delegate* unmanaged<int, uint, void> Release;

        public delegate* unmanaged<int, uint, void> CallStart;
        public delegate* unmanaged<int, uint, float, void> CallUpdate;
        public delegate* unmanaged<int, uint, uint, void> CallCollision;
        public delegate* unmanaged<int, uint, byte*, double, byte*, void> CallMessage;
        public delegate* unmanaged<int, uint, uint, byte*, float, float, int, void> CallAnimEvent;

        public delegate* unmanaged<int, byte*, uint, int> CallNamed;
        public delegate* unmanaged<int, byte*, uint, double, int> CallNamedNum;
        public delegate* unmanaged<int, byte*, uint, byte*, int> CallNamedStr;

        public delegate* unmanaged<int, int> InstanceCount;

        public delegate* unmanaged<int, byte*, uint> CompileModule;
        public delegate* unmanaged<int, uint, uint, void> RebindInstance;
        public delegate* unmanaged<int, uint, void> ReleaseModule;

        public delegate* unmanaged<int, byte*, int> LoadAssembly;

        // APPENDED, and it stays last: this bridge is a LOCKSTEP contract.
        // The native loader rejects a table that filled fewer bytes than it
        // expects, so a stale assembly fails to load with a message saying
        // to rebuild it -- it does not quietly run without on_fixed_update.
        public delegate* unmanaged<int, uint, float, void> CallFixedUpdate;
    }

    private const int BridgeAbi = 2;

    private static readonly List<Vm?> Vms = new();
    private static readonly object Gate = new();

    /// <summary>The one entry point the native host resolves by name.  It
    /// fills <paramref name="table"/> and answers how many bytes it wrote;
    /// 0 means "this assembly does not speak your bridge".</summary>
    [UnmanagedCallersOnly]
    public static int Bootstrap(void* table, int size)
    {
        try
        {
            if (table == null || size < sizeof(Bridge)) return 0;
            var b = (Bridge*)table;
            b->Size = sizeof(Bridge);
            b->Abi = BridgeAbi;

            b->VmOpen = &VmOpenEntry;
            b->VmClose = &VmCloseEntry;
            b->Instantiate = &InstantiateEntry;
            b->Release = &ReleaseEntry;
            b->CallStart = &CallStartEntry;
            b->CallUpdate = &CallUpdateEntry;
            b->CallCollision = &CallCollisionEntry;
            b->CallMessage = &CallMessageEntry;
            b->CallAnimEvent = &CallAnimEventEntry;
            b->CallNamed = &CallNamedEntry;
            b->CallNamedNum = &CallNamedNumEntry;
            b->CallNamedStr = &CallNamedStrEntry;
            b->InstanceCount = &InstanceCountEntry;
            b->CompileModule = &CompileModuleEntry;
            b->RebindInstance = &RebindInstanceEntry;
            b->ReleaseModule = &ReleaseModuleEntry;
            b->CallFixedUpdate = &CallFixedUpdateEntry;
            b->LoadAssembly = &LoadAssemblyEntry;
            return sizeof(Bridge);
        }
        catch
        {
            // Nothing to report through: the host has no VM yet, so there is
            // no host.log to reach.  0 is the answer the native side turns
            // into a named startup failure.
            return 0;
        }
    }

    // ── The entry points.  Every one of them is a barrier. ────────────────
    //
    // The shape is identical on purpose: resolve the VM, do the work, and
    // swallow-and-report anything thrown.  A single Guard helper would be
    // nicer to read and could not be used — a lambda capture inside an
    // [UnmanagedCallersOnly] method is a managed allocation on a path that
    // may run every frame for every scripted entity.

    [UnmanagedCallersOnly]
    private static int VmOpenEntry(void* hostPtr, long hostSize, void* logFn,
                                   void* logUser)
    {
        try
        {
            var vm = new Vm((IntPtr)hostPtr, hostSize, (IntPtr)logFn,
                            (IntPtr)logUser);
            lock (Gate)
            {
                for (int i = 0; i < Vms.Count; ++i)
                {
                    if (Vms[i] is null) { Vms[i] = vm; return i + 1; }
                }
                Vms.Add(vm);
                return Vms.Count;
            }
        }
        catch (Exception e)
        {
            Vm.ReportWithoutVm(e, "vm_open");
            return 0;
        }
    }

    private static Vm? Get(int id)
    {
        lock (Gate)
        {
            if (id <= 0 || id > Vms.Count) return null;
            return Vms[id - 1];
        }
    }

    [UnmanagedCallersOnly]
    private static void VmCloseEntry(int id)
    {
        try
        {
            Vm? vm;
            lock (Gate)
            {
                if (id <= 0 || id > Vms.Count) return;
                vm = Vms[id - 1];
                Vms[id - 1] = null;
            }
            vm?.Dispose();
        }
        catch (Exception e) { Vm.ReportWithoutVm(e, "vm_close"); }
    }

    [UnmanagedCallersOnly]
    private static uint InstantiateEntry(int id, byte* name, uint owner)
    {
        var vm = Get(id);
        if (vm is null) return 0u;
        try { return vm.Instantiate(Str(name), owner); }
        catch (Exception e) { vm.Report(e, "instantiate"); return 0u; }
    }

    [UnmanagedCallersOnly]
    private static void ReleaseEntry(int id, uint inst)
    {
        var vm = Get(id);
        if (vm is null) return;
        try { vm.Release(inst); }
        catch (Exception e) { vm.Report(e, "release"); }
    }

    [UnmanagedCallersOnly]
    private static void CallStartEntry(int id, uint inst)
    {
        var vm = Get(id);
        if (vm is null) return;
        try { vm.CallStart(inst); }
        catch (Exception e) { vm.Report(e, "on_start"); }
    }

    [UnmanagedCallersOnly]
    private static void CallUpdateEntry(int id, uint inst, float dt)
    {
        var vm = Get(id);
        if (vm is null) return;
        try { vm.CallUpdate(inst, dt); }
        catch (Exception e) { vm.Report(e, "on_update"); }
    }

    [UnmanagedCallersOnly]
    private static void CallFixedUpdateEntry(int id, uint inst, float dt)
    {
        var vm = Get(id);
        if (vm is null) return;
        // Reported under its OWN label, not on_update's: a handler that throws
        // every physics step must not read as the render-frame callback
        // failing, and the disable rule is keyed on the label.
        try { vm.CallFixedUpdate(inst, dt); }
        catch (Exception e) { vm.Report(e, "on_fixed_update"); }
    }

    [UnmanagedCallersOnly]
    private static void CallCollisionEntry(int id, uint inst, uint other)
    {
        var vm = Get(id);
        if (vm is null) return;
        try { vm.CallCollision(inst, other); }
        catch (Exception e) { vm.Report(e, "on_collision"); }
    }

    [UnmanagedCallersOnly]
    private static void CallMessageEntry(int id, uint inst, byte* msg,
                                         double num, byte* str)
    {
        var vm = Get(id);
        if (vm is null) return;
        try { vm.CallMessage(inst, Str(msg), num, StrOrNull(str)); }
        catch (Exception e) { vm.Report(e, "on_message"); }
    }

    [UnmanagedCallersOnly]
    private static void CallAnimEventEntry(int id, uint inst, uint animId,
                                           byte* name, float f0, float f1,
                                           int i0)
    {
        var vm = Get(id);
        if (vm is null) return;
        try { vm.CallAnimEvent(inst, animId, StrOrNull(name), f0, f1, i0); }
        catch (Exception e) { vm.Report(e, "on_anim_event"); }
    }

    [UnmanagedCallersOnly]
    private static int CallNamedEntry(int id, byte* fn, uint e)
    {
        var vm = Get(id);
        if (vm is null) return 0;
        try { return vm.CallNamed(Str(fn), new object[] { e }) ? 1 : 0; }
        catch (Exception ex) { vm.Report(ex, "call_named"); return 1; }
    }

    [UnmanagedCallersOnly]
    private static int CallNamedNumEntry(int id, byte* fn, uint e, double v)
    {
        var vm = Get(id);
        if (vm is null) return 0;
        try { return vm.CallNamed(Str(fn), new object[] { e, v }) ? 1 : 0; }
        catch (Exception ex) { vm.Report(ex, "call_named_num"); return 1; }
    }

    [UnmanagedCallersOnly]
    private static int CallNamedStrEntry(int id, byte* fn, uint e, byte* v)
    {
        var vm = Get(id);
        if (vm is null) return 0;
        try
        {
            return vm.CallNamed(Str(fn), new object?[] { e, StrOrNull(v) })
                   ? 1 : 0;
        }
        catch (Exception ex) { vm.Report(ex, "call_named_str"); return 1; }
    }

    [UnmanagedCallersOnly]
    private static int InstanceCountEntry(int id)
    {
        var vm = Get(id);
        if (vm is null) return 0;
        try { return vm.InstanceCount; }
        catch (Exception e) { vm.Report(e, "instance_count"); return 0; }
    }

    [UnmanagedCallersOnly]
    private static uint CompileModuleEntry(int id, byte* name)
    {
        var vm = Get(id);
        if (vm is null) return 0u;
        try { return vm.CompileModule(Str(name)); }
        catch (Exception e) { vm.Report(e, "compile_module"); return 0u; }
    }

    [UnmanagedCallersOnly]
    private static void RebindInstanceEntry(int id, uint inst, uint mod)
    {
        var vm = Get(id);
        if (vm is null) return;
        try { vm.RebindInstance(inst, mod); }
        catch (Exception e) { vm.Report(e, "rebind_instance"); }
    }

    [UnmanagedCallersOnly]
    private static void ReleaseModuleEntry(int id, uint mod)
    {
        var vm = Get(id);
        if (vm is null) return;
        try { vm.ReleaseModule(mod); }
        catch (Exception e) { vm.Report(e, "release_module"); }
    }

    [UnmanagedCallersOnly]
    private static int LoadAssemblyEntry(int id, byte* path)
    {
        var vm = Get(id);
        if (vm is null) return 0;
        try { return vm.LoadAssembly(Str(path)) ? 1 : 0; }
        catch (Exception e) { vm.Report(e, "load_assembly"); return 0; }
    }

    // ── UTF-8 in ──────────────────────────────────────────────────────────
    //
    // Copied, never retained: the native side owns these bytes for the
    // duration of the call only.  A string handed straight to a script field
    // would be a dangling pointer the moment the C frame returned.
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static string Str(byte* p) => p == null
        ? string.Empty : Marshal.PtrToStringUTF8((IntPtr)p) ?? string.Empty;

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static string? StrOrNull(byte* p) => p == null
        ? null : Marshal.PtrToStringUTF8((IntPtr)p);
}

/// <summary>One JceScript handle's managed state.</summary>
internal sealed unsafe class Vm : IDisposable
{
    private readonly List<object?> _instances = new();
    private readonly List<Type?> _modules = new();
    // PROCESS-WIDE, not per-Vm.  Loading an assembly is an act on the
    // AssemblyLoadContext, which the whole process shares -- so "which
    // assemblies can be resolved" is a property of the process, and a
    // per-Vm list only records which Vm happened to be told.
    //
    // It mattered: a game loads its script assembly through
    // jce_script_vm_csharp_load_assembly() at startup, and the RUNTIME then
    // creates a Vm of its own to instantiate scripts on.  With a per-Vm
    // list that second Vm holds only this assembly, so every script
    // resolved to nothing and the failure read
    //   "no type named 'scripts/Foo.cs' in 1 loaded assembly ...
    //    load the game assembly first"
    // -- advice the caller had already followed, and which no caller could
    // have followed, because the Vm that needed it did not exist yet.
    private static readonly List<Assembly> _assemblies = new();
    private readonly Dictionary<string, List<MethodInfo>> _named = new();
    private int _live;

    internal readonly IntPtr Api;
    private readonly IntPtr _logFn;
    private readonly IntPtr _logUser;

    internal Vm(IntPtr hostPtr, long hostSize, IntPtr logFn, IntPtr logUser)
    {
        _logFn = logFn;
        _logUser = logUser;
        // The C ABI, opened over the SAME JceScriptHost the native VM copied.
        // Managed code reaches the engine through this and nothing else -
        // P/Invoke is C#'s FFI, so unlike Java this backend needs no shim of
        // its own.  A zero handle is survivable: every binding then answers
        // its absent value, exactly as a NULL host member does.
        Api = hostPtr == IntPtr.Zero
            ? IntPtr.Zero
            : Interop.jce_script_api_open(hostPtr, (nuint)hostSize,
                                          Interop.ScriptApiMin);

        // The assembly this runtime lives in is always searchable, so the
        // diagnostic types below are reachable with no project of their own.
        if (!_assemblies.Contains(typeof(Vm).Assembly))
            _assemblies.Add(typeof(Vm).Assembly);
        IndexNamedHandlers(typeof(Vm).Assembly);
    }

    public void Dispose()
    {
        for (int i = 0; i < _instances.Count; ++i)
        {
            (_instances[i] as IDisposable)?.Dispose();
            _instances[i] = null;
        }
        _live = 0;
        if (Api != IntPtr.Zero) Interop.jce_script_api_close(Api);
    }

    internal int InstanceCount => _live;

    // ── Reporting ─────────────────────────────────────────────────────────

    internal void Report(Exception e, string what)
    {
        string line = what + ": " + e.GetType().Name + ": " + e.Message;
        if (e.StackTrace is { Length: > 0 } st) line += "\n  " + st;
        if (_logFn != IntPtr.Zero)
        {
            // Through the host's own `log`, which is where the editor console
            // reads from.  Not Console.Error: a shipped game is a WIN32
            // subsystem process with no console at all, so that channel is
            // /dev/null exactly where it matters most.
            // NUL-terminated by construction: the host takes a C string
            // and GetBytes does not add a terminator.  A zero-filled
            // array one byte longer is the terminator, with no escape
            // in the source to get wrong.
            int n = System.Text.Encoding.UTF8.GetByteCount(line);
            var bytes = new byte[n + 1];
            System.Text.Encoding.UTF8.GetBytes(line, 0, line.Length, bytes, 0);
            fixed (byte* p = bytes)
            {
                ((delegate* unmanaged[Cdecl]<void*, byte*, void>)_logFn)(
                    (void*)_logUser, p);
            }
            return;
        }
        Console.Error.WriteLine("[script.csharp] " + line);
    }

    internal static void ReportWithoutVm(Exception e, string what)
        => Console.Error.WriteLine("[script.csharp] " + what + ": " + e);

    // ── Types ─────────────────────────────────────────────────────────────

    internal bool LoadAssembly(string path)
    {
        if (string.IsNullOrEmpty(path) || !File.Exists(path)) return false;
        // Not a collectible context.  A collectible ALC would allow
        // unload-and-reload, and this backend cannot use it: the engine's
        // reload path (jce_runtime_reload_script) calls compile_module and
        // rebind_instance and never asks anyone to unload, so a collectible
        // context would only add a way for a still-live instance to keep a
        // dead ALC alive.
        //
        // And not AssemblyLoadContext.Default either, which is what this
        // used to say.  hostfxr loads THIS assembly into an isolated context
        // of its own, so a game assembly placed in Default cannot bind the
        // JceScript reference every one of them carries -- it resolves to a
        // different JceScript, or to none.  The symptom is a
        // ReflectionTypeLoadException the moment IndexNamedHandlers walks
        // the types, i.e. the game's scripts are simply absent, which is
        // indistinguishable from "the user wrote no scripts".
        //
        // Load beside ourselves instead: whichever context the host put the
        // bridge in is the one where JceScript already resolves.
        var host = AssemblyLoadContext.GetLoadContext(typeof(Vm).Assembly)
                   ?? AssemblyLoadContext.Default;
        var asm = host.LoadFromAssemblyPath(Path.GetFullPath(path));
        if (_assemblies.Contains(asm)) return true;
        _assemblies.Add(asm);
        IndexNamedHandlers(asm);
        return true;
    }

    /// <summary>Resolve a Script component's path to a type.</summary>
    /// <remarks>
    /// "Assets/Turret.cs" -> the type named Turret.  That is Unity's
    /// convention (the class matches the file name) and it is the whole
    /// reason .cs is registered as JCEASSET_SCRIPT_FORM_REFERENCE: the bytes
    /// at the path are never read, the NAME is the input.
    ///
    /// An assembly-qualified name ("MyGame.Turret, MyGame") is accepted too,
    /// because that is what compile_module hands back for a reload and a
    /// project with two Turret types has no other way to say which.
    /// </remarks>
    internal Type? Resolve(string nameOrPath)
    {
        if (string.IsNullOrWhiteSpace(nameOrPath)) return null;

        string name = nameOrPath;
        if (name.EndsWith(".cs", StringComparison.OrdinalIgnoreCase))
            name = Path.GetFileNameWithoutExtension(name);

        var direct = Type.GetType(name, throwOnError: false);
        if (direct is not null) return direct;

        foreach (var asm in _assemblies)
        {
            var t = asm.GetType(name, throwOnError: false);
            if (t is not null) return t;
        }
        // By short name, last: a full name is unambiguous and a short one is
        // not, so trying the ambiguous form first could pick a different type
        // than the one the caller spelled out.
        foreach (var asm in _assemblies)
        {
            foreach (var t in asm.GetTypes())
            {
                if (t.Name == name) return t;
            }
        }
        return null;
    }

    private void IndexNamedHandlers(Assembly asm)
    {
        // Every PUBLIC STATIC method of a script type becomes a named
        // handler, which is what the Java backend does and what Lua's global
        // function table amounts to.
        foreach (var t in asm.GetTypes())
        {
            if (!typeof(JceEntityScript).IsAssignableFrom(t)) continue;
            foreach (var m in t.GetMethods(BindingFlags.Public
                                           | BindingFlags.Static
                                           | BindingFlags.DeclaredOnly))
            {
                if (!_named.TryGetValue(m.Name, out var list))
                    _named[m.Name] = list = new List<MethodInfo>();
                list.Add(m);
            }
        }
    }

    // ── Instances ─────────────────────────────────────────────────────────

    internal uint Instantiate(string nameOrPath)
        => Instantiate(nameOrPath, 0u);

    internal uint Instantiate(string nameOrPath, uint owner)
    {
        var t = Resolve(nameOrPath);
        if (t is null)
        {
            Report(new TypeLoadException(
                "no type named '" + nameOrPath + "' in "
                + _assemblies.Count + " loaded assembl"
                + (_assemblies.Count == 1 ? "y" : "ies")
                + ". A .cs path resolves to the type matching its FILE NAME; "
                + "load the game assembly first."), "instantiate");
            return 0u;
        }
        if (!typeof(JceEntityScript).IsAssignableFrom(t))
        {
            Report(new InvalidOperationException(
                "'" + t.FullName + "' does not derive from JceEntityScript, "
                + "so it has no lifecycle to call."), "instantiate");
            return 0u;
        }
        var obj = (JceEntityScript?)Activator.CreateInstance(t);
        if (obj is null) return 0u;
        obj.Bind(this, owner);
        return Store(obj);
    }

    private uint Store(object o)
    {
        for (int i = 0; i < _instances.Count; ++i)
        {
            if (_instances[i] is null) { _instances[i] = o; ++_live; return (uint)(i + 1); }
        }
        _instances.Add(o);
        ++_live;
        return (uint)_instances.Count;
    }

    private JceEntityScript? Slot(uint h)
    {
        int i = (int)h - 1;
        if (i < 0 || i >= _instances.Count) return null;
        return _instances[i] as JceEntityScript;
    }

    internal void Release(uint h)
    {
        int i = (int)h - 1;
        if (i < 0 || i >= _instances.Count || _instances[i] is null) return;
        (_instances[i] as IDisposable)?.Dispose();
        _instances[i] = null;
        --_live;
    }

    internal void CallStart(uint h) => Slot(h)?.OnStart();
    internal void CallUpdate(uint h, float dt) => Slot(h)?.OnUpdate(dt);
    internal void CallFixedUpdate(uint h, float dt) => Slot(h)?.OnFixedUpdate(dt);
    internal void CallCollision(uint h, uint other) => Slot(h)?.OnCollision(other);
    internal void CallMessage(uint h, string msg, double num, string? str)
        => Slot(h)?.OnMessage(msg, num, str);
    internal void CallAnimEvent(uint h, uint id, string? name, float f0,
                                float f1, int i0)
        => Slot(h)?.OnAnimEvent(id, name, f0, f1, i0);

    /// <summary>Returns "a handler of that name existed" — which is what
    /// Lua's lua_getglobal + lua_isfunction answers, NOT "the call
    /// succeeded".  A handler that throws still counts as invoked, and the
    /// barrier above turns that into 1 for exactly this reason.</summary>
    internal bool CallNamed(string fn, object?[] args)
    {
        if (string.IsNullOrEmpty(fn)) return false;
        if (!_named.TryGetValue(fn, out var list) || list.Count == 0)
            return false;
        var m = Pick(list, args) ?? list[0];
        m.Invoke(null, Trim(args, m.GetParameters().Length));
        return true;
    }

    private static MethodInfo? Pick(List<MethodInfo> list, object?[] args)
    {
        foreach (var m in list)
        {
            var ps = m.GetParameters();
            if (ps.Length != args.Length) continue;
            bool ok = true;
            for (int i = 0; i < ps.Length && ok; ++i)
            {
                if (args[i] is null) ok = !ps[i].ParameterType.IsValueType;
                else ok = ps[i].ParameterType.IsInstanceOfType(args[i]);
            }
            if (ok) return m;
        }
        return null;
    }

    private static object?[] Trim(object?[] args, int n)
    {
        if (n >= args.Length) return args;
        var t = new object?[n];
        Array.Copy(args, t, n);
        return t;
    }

    // ── Reload ────────────────────────────────────────────────────────────

    internal uint CompileModule(string nameOrPath)
    {
        var t = Resolve(nameOrPath);
        if (t is null || !typeof(JceEntityScript).IsAssignableFrom(t))
            return 0u;
        for (int i = 0; i < _modules.Count; ++i)
        {
            if (_modules[i] is null) { _modules[i] = t; return (uint)(i + 1); }
        }
        _modules.Add(t);
        return (uint)_modules.Count;
    }

    internal void RebindInstance(uint h, uint mod)
    {
        int mi = (int)mod - 1, ii = (int)h - 1;
        if (mi < 0 || mi >= _modules.Count) return;
        if (ii < 0 || ii >= _instances.Count) return;
        var t = _modules[mi];
        var old = _instances[ii] as JceEntityScript;
        if (t is null || old is null) return;
        var fresh = (JceEntityScript?)Activator.CreateInstance(t);
        if (fresh is null) return;
        // The ENTITY carries over and the fields do not, and that is a real
        // limit rather than an oversight: the new object is an instance of a
        // possibly-different type, so there is no field of the old one this
        // could copy that the new one is guaranteed to have.
        fresh.Bind(this, old.Entity);
        (old as IDisposable)?.Dispose();
        _instances[ii] = fresh;
    }

    internal void ReleaseModule(uint mod)
    {
        int i = (int)mod - 1;
        if (i < 0 || i >= _modules.Count) return;
        _modules[i] = null;
    }
}
