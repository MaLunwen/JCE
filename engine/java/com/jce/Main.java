package com.jce;

import java.net.URISyntaxException;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;

public final class Main {
    // Sentinel property set on the child process to prevent infinite restart loops.
    private static final String FIRST_THREAD_PROP = "jce.macos.firstthread";

    private Main() {
    }

    public static void main(String[] args) throws Exception {
        // SDL/Cocoa on macOS must be initialised on the OS main thread.
        // The JVM main() thread is not the OS main thread by default, which
        // causes SDL_Init to fail with "No available video device".
        // Detect this case and relaunch with -XstartOnFirstThread.
        if (isMacOS() && System.getProperty(FIRST_THREAD_PROP) == null) {
            restartWithFirstThread(args);
            return;
        }

        int maxFrames = 0;
        if (args.length > 0) {
            maxFrames = Integer.parseInt(args[0]);
        }

        int frameCount = 0;
        try (JceRuntime runtime = new JceRuntime()) {
            runtime.init();
            boolean running = true;
            while (running && !runtime.shouldQuit()) {
                boolean frameOk = runtime.iterate();
                boolean reachedMaxFrames = maxFrames > 0 && ++frameCount >= maxFrames;
                running = frameOk && !reachedMaxFrames;
            }
        }
    }

    private static boolean isMacOS() {
        String os = System.getProperty("os.name", "").toLowerCase();
        return os.contains("mac") || os.contains("darwin");
    }

    private static void restartWithFirstThread(String[] args) throws Exception {
        String jarPath;
        try {
            jarPath = Paths.get(
                Main.class.getProtectionDomain().getCodeSource().getLocation().toURI()
            ).toAbsolutePath().toString();
        } catch (URISyntaxException e) {
            System.err.println("[JCE] macOS: cannot determine JAR path — run with -XstartOnFirstThread manually.");
            return;
        }

        String javaExe = System.getProperty("java.home") + "/bin/java";

        List<String> cmd = new ArrayList<>();
        cmd.add(javaExe);
        cmd.add("-XstartOnFirstThread");
        cmd.add("-D" + FIRST_THREAD_PROP + "=1");
        cmd.add("-jar");
        cmd.add(jarPath);
        for (String arg : args) {
            cmd.add(arg);
        }

        ProcessBuilder pb = new ProcessBuilder(cmd);
        pb.inheritIO();
        System.exit(pb.start().waitFor());
    }
}
