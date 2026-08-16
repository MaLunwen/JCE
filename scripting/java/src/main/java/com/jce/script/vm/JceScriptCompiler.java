/* JceScriptCompiler.java — javac, in memory, for one script chunk.
 *
 * WHY A COMPILER IS PART OF THE VM AT ALL.  Two of the eighteen slots take
 * SOURCE TEXT: jce_script_instantiate_source() and jce_script_compile_module()
 * (the hot-reload entry point).  A backend that could only load pre-built
 * artefacts would have to fill both with a no-op, and a no-op there is not a
 * degraded feature — it is hot-reload and in-memory scripts gone.  Lua's
 * luaL_loadbuffer accepts source OR a pre-compiled chunk; this class plus
 * JceScriptClassLoader give Java the same pair, dispatched on the class-file
 * magic rather than on a file extension, for the same reason Lua dispatches on
 * the chunk's first byte.
 *
 * A JRE HAS NO COMPILER, AND THAT IS REPORTED, NEVER ABSORBED.
 * ToolProvider.getSystemJavaCompiler() returns null outside a JDK.  That
 * becomes a thrown JceScriptCompileError naming the cause, which travels out
 * through JNI to the native slot, which logs it through JceScriptHost::log and
 * returns 0 — the same shape as Lua's "compile error (chunk): ..." followed by
 * a 0 instance.  There is deliberately no fallback: a game that ships .class
 * bytes never reaches this code and needs no JDK, and a game that wants live
 * source needs one.  "There is an older artefact we could load instead" is not
 * a reason to answer a question that was not asked.
 *
 * NOT A SANDBOX, AND SAYING SO IS THE POINT.  jce_script.c opens Lua through
 * open_sandboxed_libs() and replaces load().  Nothing equivalent exists for
 * Java: the SecurityManager is deprecated for removal and never defended
 * against native access anyway.  Compiling attacker-supplied Java source is
 * arbitrary code execution BY DESIGN, and so is defining attacker-supplied
 * .class bytes.  A Java script is first-party code, at exactly the trust level
 * of a DLL.  engine/include/jce/resource/jce_mod_loader.h states the matching
 * rule from the other side: mounted content never names code.
 */
package com.jce.script.vm;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.net.URI;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import javax.tools.Diagnostic;
import javax.tools.DiagnosticCollector;
import javax.tools.ForwardingJavaFileManager;
import javax.tools.JavaCompiler;
import javax.tools.JavaFileManager;
import javax.tools.JavaFileObject;
import javax.tools.SimpleJavaFileObject;
import javax.tools.StandardJavaFileManager;
import javax.tools.ToolProvider;

final class JceScriptCompiler {

    private JceScriptCompiler() { }

    /** Compilation failed.  Unchecked so it travels the same path a script's
     *  own exception travels: out through JNI, to the native slot's check. */
    static final class JceScriptCompileError extends RuntimeException {
        private static final long serialVersionUID = 1L;
        JceScriptCompileError(String message) { super(message); }
    }

    /* "package a.b;" — at most one, and the first match is it. */
    private static final Pattern PACKAGE = Pattern.compile(
        "(?m)^\\s*package\\s+([A-Za-z_$][\\w$]*(?:\\.[A-Za-z_$][\\w$]*)*)\\s*;");

    /* The first top-level type declaration.  Deliberately narrow: a chunk that
     * does not begin with one is a chunk this VM cannot NAME, and guessing a
     * name javac then disagrees with surfaces as a NoClassDefFoundError a long
     * way from the cause. */
    private static final Pattern TYPE = Pattern.compile(
        "(?m)^\\s*(?:public\\s+)?(?:final\\s+|abstract\\s+|strictfp\\s+)*"
        + "(?:class|interface|enum)\\s+([A-Za-z_$][\\w$]*)");

    /** The binary name the chunk declares, or null when it declares none. */
    static String binaryNameOf(String source) {
        if (source == null) return null;
        Matcher t = TYPE.matcher(source);
        if (!t.find()) return null;
        Matcher p = PACKAGE.matcher(source);
        return p.find() ? p.group(1) + "." + t.group(1) : t.group(1);
    }

    /** Compile one chunk.  The returned map is ordered with the chunk's own
     *  top-level type FIRST, because that is the class the caller instantiates;
     *  nested and anonymous classes follow. */
    static Map<String, byte[]> compile(String chunkName, String source) {
        if (source == null) {
            throw new JceScriptCompileError(
                "compile error (" + chunkName + "): no source");
        }
        String binary = binaryNameOf(source);
        if (binary == null) {
            throw new JceScriptCompileError(
                "compile error (" + chunkName + "): the chunk declares no "
                + "top-level class, interface or enum");
        }

        JavaCompiler javac = ToolProvider.getSystemJavaCompiler();
        if (javac == null) {
            throw new JceScriptCompileError(
                "compile error (" + chunkName + "): this JVM has no Java "
                + "compiler (ToolProvider.getSystemJavaCompiler() == null). "
                + "Compiling script SOURCE needs a JDK; pre-compiled .class "
                + "bytes load on a JRE.");
        }

        DiagnosticCollector<JavaFileObject> diags =
            new DiagnosticCollector<JavaFileObject>();
        StandardJavaFileManager std =
            javac.getStandardFileManager(diags, null, null);
        Memory memory = new Memory(std);

        List<String> options = new ArrayList<String>();
        /* The chunk must see JceEntityScript and everything the application
         * put on its own classpath.  Read from the live JVM rather than
         * reconstructed, so it cannot disagree with what the loader will
         * actually resolve against. */
        options.add("-classpath");
        options.add(System.getProperty("java.class.path", ""));

        boolean ok;
        try {
            ok = javac.getTask(null, memory, diags, options, null,
                               Collections.<JavaFileObject>singletonList(
                                   new Source(binary, source))).call()
                 .booleanValue();
        } finally {
            /* Harvests the emitted bytes; also closes the delegate. */
            close(memory);
        }

        if (!ok || memory.classes.isEmpty()) {
            throw new JceScriptCompileError(
                "compile error (" + chunkName + "): " + render(diags));
        }

        /* Main class first.  javac emits nested types in its own order and the
         * caller must not have to guess which entry is the chunk. */
        Map<String, byte[]> ordered = new LinkedHashMap<String, byte[]>();
        byte[] main = memory.classes.remove(binary);
        if (main == null) {
            throw new JceScriptCompileError(
                "compile error (" + chunkName + "): javac produced no class "
                + "named '" + binary + "'");
        }
        ordered.put(binary, main);
        ordered.putAll(memory.classes);
        return ordered;
    }

    private static String render(DiagnosticCollector<JavaFileObject> diags) {
        StringBuilder sb = new StringBuilder();
        List<Diagnostic<? extends JavaFileObject>> all = diags.getDiagnostics();
        for (int i = 0; i < all.size(); ++i) {
            Diagnostic<? extends JavaFileObject> d = all.get(i);
            if (d.getKind() != Diagnostic.Kind.ERROR) continue;
            if (sb.length() > 0) sb.append("; ");
            sb.append("line ").append(d.getLineNumber()).append(": ")
              .append(d.getMessage(Locale.ROOT));
        }
        return sb.length() == 0 ? "javac reported failure with no error"
                                : sb.toString();
    }

    private static void close(JavaFileManager fm) {
        try {
            fm.close();
        } catch (IOException ignored) {
            /* Closing an in-memory file manager has nothing to fail at, and a
             * throw here would replace a real compile diagnostic with an I/O
             * one.  Nothing is dropped: the bytes are already in the map. */
        }
    }

    /** The chunk, as javac's input. */
    private static final class Source extends SimpleJavaFileObject {
        private final String text;

        Source(String binaryName, String text) {
            super(URI.create("jce:///" + binaryName.replace('.', '/') + ".java"),
                  Kind.SOURCE);
            this.text = text;
        }

        @Override
        public CharSequence getCharContent(boolean ignoreEncodingErrors) {
            return text;
        }
    }

    /** javac's output, kept in memory: nothing this VM compiles touches disk. */
    private static final class Output extends SimpleJavaFileObject {
        final ByteArrayOutputStream bytes = new ByteArrayOutputStream();

        Output(String binaryName) {
            super(URI.create("jce:///" + binaryName.replace('.', '/') + ".class"),
                  Kind.CLASS);
        }

        @Override
        public ByteArrayOutputStream openOutputStream() {
            return bytes;
        }
    }

    private static final class Memory
            extends ForwardingJavaFileManager<StandardJavaFileManager> {
        final Map<String, byte[]> classes = new LinkedHashMap<String, byte[]>();
        private final List<Output> pending = new ArrayList<Output>();
        private final List<String> names = new ArrayList<String>();

        Memory(StandardJavaFileManager delegate) { super(delegate); }

        @Override
        public JavaFileObject getJavaFileForOutput(Location location,
                                                   String className,
                                                   JavaFileObject.Kind kind,
                                                   javax.tools.FileObject sibling) {
            Output out = new Output(className);
            pending.add(out);
            names.add(className);
            return out;
        }

        /* Harvest here rather than in getJavaFileForOutput: the stream is not
         * written until javac finishes the class, so reading it earlier yields
         * an empty array — a class that "loaded" with zero bytes fails at
         * defineClass with a message about the constant pool, a long way from
         * this method. */
        @Override
        public void close() throws IOException {
            for (int i = 0; i < pending.size(); ++i) {
                classes.put(names.get(i), pending.get(i).bytes.toByteArray());
            }
            pending.clear();
            names.clear();
            super.close();
        }
    }
}
