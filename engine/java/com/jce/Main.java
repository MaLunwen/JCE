package com.jce;

public final class Main {
    private Main() {
    }

    public static void main(String[] args) {
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
}
