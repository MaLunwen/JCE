/* JceScriptClassLoader.java — one per JceScript handle.
 *
 * WHY EACH HANDLE GETS ITS OWN.  JNI_CreateJavaVM runs once per process, so
 * two JceScript handles in the Java language share one JVM — where two Lua
 * handles are two lua_States that share nothing at all.  A class is identified
 * by (binary name, defining loader), so defining a script through a
 * PER-HANDLE loader is what makes "handle A's Spinner" and "handle B's
 * Spinner" different classes with different static storage.  Without this the
 * second handle would silently reuse the first handle's classes, and a static
 * field written by one entity's script would be read by another's.
 * *Enforced by:* tests/scripting/java/vm/test_jce_script_vm_java.c ::
 * test_two_handles_do_not_share_script_statics.
 *
 * It is also what makes hot-reload possible: jce_script_compile_module()
 * recompiles the same source under the same name, and a loader that had
 * already defined that name would throw LinkageError.  Each define goes to a
 * fresh child loader for exactly that reason, so a module can be compiled
 * again and again while the instances bound to the previous one keep working.
 *
 * WHAT IS NOT ISOLATED, stated rather than implied: everything the parent
 * loader owns — this package, com.jce.script.JceScript, the JDK.  Isolation
 * here is about the classes a script DEFINES, not about the process.
 */
package com.jce.script.vm;

import java.util.Map;

final class JceScriptClassLoader {

    private final ClassLoader parent;

    JceScriptClassLoader(ClassLoader parent) {
        this.parent = parent != null ? parent
                                     : JceScriptClassLoader.class.getClassLoader();
    }

    /** Define one compilation unit's classes and return its main type — the
     *  FIRST entry, which JceScriptCompiler guarantees is the chunk's own
     *  top-level type. */
    Class<?> defineFromSource(Map<String, byte[]> classes) {
        Unit unit = new Unit(parent, classes);
        String main = classes.keySet().iterator().next();
        try {
            return unit.loadClass(main);
        } catch (ClassNotFoundException e) {
            /* Unreachable by construction — `main` is a key of the very map
             * `unit` resolves from — so if it ever fires the map and the name
             * have diverged, and that is worth naming rather than swallowing. */
            throw new IllegalStateException(
                "compiled class '" + main + "' is not in its own unit", e);
        }
    }

    /** Define pre-compiled bytes.  The name is read from the class file by the
     *  JVM: passing null is not laziness, it is the only spelling that cannot
     *  disagree with the bytes. */
    Class<?> defineFromClassFile(byte[] bytes) {
        return new Unit(parent, null).define(bytes);
    }

    /** One compilation unit.  Resolves its own classes first so a chunk's
     *  nested types find each other, then delegates. */
    private static final class Unit extends ClassLoader {
        private final Map<String, byte[]> pending;

        Unit(ClassLoader parent, Map<String, byte[]> pending) {
            super(parent);
            this.pending = pending;
        }

        Class<?> define(byte[] bytes) {
            return defineClass(null, bytes, 0, bytes.length);
        }

        /* CHILD-FIRST, for this unit's own classes ONLY.  The default
         * parent-first order would hand back an app-classpath class of the
         * same name in preference to the one just compiled — silently running
         * different code than the source the caller passed, which is the worst
         * available failure for a hot-reload path.  Everything not in this
         * unit still delegates, so the JDK and JceEntityScript resolve
         * normally and only one copy of them ever exists. */
        @Override
        protected Class<?> loadClass(String name, boolean resolve)
                throws ClassNotFoundException {
            if (pending != null && pending.containsKey(name)) {
                synchronized (getClassLoadingLock(name)) {
                    Class<?> c = findLoadedClass(name);
                    if (c == null) c = findClass(name);
                    if (resolve) resolveClass(c);
                    return c;
                }
            }
            return super.loadClass(name, resolve);
        }

        @Override
        protected Class<?> findClass(String name) throws ClassNotFoundException {
            byte[] bytes = pending != null ? pending.get(name) : null;
            if (bytes == null) throw new ClassNotFoundException(name);
            return defineClass(name, bytes, 0, bytes.length);
        }
    }
}
