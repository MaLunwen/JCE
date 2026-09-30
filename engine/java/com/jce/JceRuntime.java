package com.jce;

import java.io.IOException;
import java.io.InputStream;
import java.net.URISyntaxException;
import java.nio.file.FileSystems;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardCopyOption;
import java.nio.file.attribute.PosixFilePermission;
import java.util.EnumSet;
import java.util.Set;

public final class JceRuntime implements AutoCloseable {
    private static final String OS_NAME_PROP = "os.name";
    private static final String OS_ARCH_PROP = "os.arch";
    private static final String LIB_NAME = "jce";
    private static final String CLASSIFIER;
    private static final String LIB_FILE;
    private static final String RESOURCE_PATH;

    /**
     * Packed JCE C-ABI version this binding was written against, in the
     * jce_version.h layout 0xMMmmpp00 (0.11.2 -> 0x000B0200). Bump in
     * lockstep with project(JCE VERSION ...) in the root CMakeLists.txt
     * whenever the native surface used below changes shape.
     */
    private static final int EXPECTED_API_VERSION = 0x000C0200;

    /**
     * Mirror of JceAppResult in engine/include/jce/application/jce_engine.h.
     * Ordinals ARE the wire values — a boolean cannot distinguish a clean
     * quit (SUCCESS) from an error quit (FAILURE), which is exactly what
     * callers need in order to pick a process exit code.
     */
    public enum Result {
        CONTINUE,
        SUCCESS,
        FAILURE;

        static Result fromNative(int value) {
            switch (value) {
                case 0:  return CONTINUE;
                case 1:  return SUCCESS;
                default: return FAILURE;  /* unknown code == not safe to keep running */
            }
        }
    }

    static {
        CLASSIFIER = detectClassifier();
        LIB_FILE = detectLibFileName();
        RESOURCE_PATH = "/natives/" + CLASSIFIER + "/" + LIB_FILE;
        loadNativeLibrary();
        verifyApiVersion();
    }

    private static String detectClassifier() {
        String osName = System.getProperty(OS_NAME_PROP, "").toLowerCase();
        String osArch = System.getProperty(OS_ARCH_PROP, "").toLowerCase();

        String osId;
        if (osName.contains("win")) {
            osId = "win32";
        } else if (osName.contains("linux")) {
            osId = "linux";
        } else if (osName.contains("mac") || osName.contains("darwin")) {
            osId = "darwin";
        } else {
            throw new UnsupportedOperationException(
                "Unsupported OS: " + System.getProperty(OS_NAME_PROP)
                + ". Supported: Windows, Linux, macOS");
        }

        String archId;
        if (osArch.equals("amd64") || osArch.equals("x86_64")) {
            archId = "x86_64";
        } else if (osArch.equals("aarch64") || osArch.equals("arm64")) {
            archId = "aarch64";
        } else {
            throw new UnsupportedOperationException(
                "Unsupported architecture: " + System.getProperty(OS_ARCH_PROP)
                + ". Supported: x86_64/amd64, aarch64/arm64");
        }

        return osId + "-" + archId;
    }

    private static String detectLibFileName() {
        String osName = System.getProperty(OS_NAME_PROP, "").toLowerCase();
        if (osName.contains("win")) {
            return "jce.dll";
        } else if (osName.contains("mac") || osName.contains("darwin")) {
            return "libjce.dylib";
        } else {
            return "libjce.so";
        }
    }

    private long nativeHandle;

    private static native int nativeApiVersion();
    private static native String nativeApiVersionString();
    private static native long nativeCreate();
    private static native int nativeIterate(long handle);
    private static native int nativeLastResult(long handle);
    private static native void nativeDestroy(long handle);

    public void init() {
        if (nativeHandle != 0L) {
            return;
        }

        System.setProperty("jce.config.path", resolveConfigPathForNative());

        /* Extract and set the shared PAK path. Native side reads this via
         * jce_engine_set_pak_path() — one copy shared across all platforms. */
        String pakPath = extractPakIfNeeded();
        if (pakPath != null) {
            System.setProperty("jce.pak.path", pakPath);
        }

        nativeHandle = nativeCreate();
        if (nativeHandle == 0L) {
            throw new IllegalStateException("Failed to create native JCE engine");
        }
    }

    /** Runs one frame. Returns the engine's own JceAppResult, not a boolean. */
    public Result iterate() {
        ensureInitialized();
        return Result.fromNative(nativeIterate(nativeHandle));
    }

    /** Result of the last {@link #iterate()} — survives the loop exit. */
    public Result lastResult() {
        ensureInitialized();
        return Result.fromNative(nativeLastResult(nativeHandle));
    }

    public boolean shouldQuit() {
        return lastResult() != Result.CONTINUE;
    }

    @Override
    public void close() {
        if (nativeHandle != 0L) {
            nativeDestroy(nativeHandle);
            nativeHandle = 0L;
        }
    }

    private void ensureInitialized() {
        if (nativeHandle == 0L) {
            throw new IllegalStateException("JceRuntime is not initialized");
        }
    }

    private static void loadNativeLibrary() {
        try {
            System.loadLibrary(LIB_NAME);
            return;
        } catch (UnsatisfiedLinkError ignored) {
            // Fall through to bundled-resource loading.
        }

        try (InputStream input = JceRuntime.class.getResourceAsStream(RESOURCE_PATH)) {
            if (input == null) {
                throw new IllegalStateException("Native library not found in JAR: " + RESOURCE_PATH);
            }

            Path tempDir = createSecureExtractionDirectory();
            tempDir.toFile().deleteOnExit();

            Path libPath = tempDir.resolve(LIB_FILE);
            Files.copy(input, libPath, StandardCopyOption.REPLACE_EXISTING);
            applyOwnerOnlyPermissions(libPath);
            libPath.toFile().deleteOnExit();

            System.load(libPath.toAbsolutePath().toString());
        } catch (IOException e) {
            throw new IllegalStateException("Failed to unpack bundled native library", e);
        }
    }

    /**
     * ABI handshake — the JAR and the native library ship separately (the
     * library can also come from java.library.path), so a stale one of
     * either must fail loudly here rather than corrupt memory later.
     *
     * Rule follows jce_version.h: reject a differing major. Pre-1.0 the
     * minor IS the breaking axis, so while major == 0 a differing minor is
     * rejected too. The patch field is deliberately ignored.
     */
    private static void verifyApiVersion() {
        int actual = nativeApiVersion();
        int actualMajor = (actual >>> 24) & 0xFF;
        int actualMinor = (actual >>> 16) & 0xFF;
        int expectedMajor = (EXPECTED_API_VERSION >>> 24) & 0xFF;
        int expectedMinor = (EXPECTED_API_VERSION >>> 16) & 0xFF;

        boolean compatible = actualMajor == expectedMajor
            && (expectedMajor != 0 || actualMinor == expectedMinor);
        if (compatible) {
            return;
        }

        throw new IllegalStateException(
            "JCE native/Java ABI mismatch: native library reports "
            + nativeApiVersionString() + " (0x" + Integer.toHexString(actual)
            + "), but this binding was built for " + expectedMajor + "." + expectedMinor
            + ".x (0x" + Integer.toHexString(EXPECTED_API_VERSION) + "). "
            + "Rebuild the JAR and the native library together "
            + "(scripts/package-jni-jar.bat).");
    }

    private static Path createSecureExtractionDirectory() throws IOException {
        Path baseDir = Paths.get(System.getProperty("java.io.tmpdir"));
        String userName = sanitizeUserName(System.getProperty("user.name", "user"));
        Path dir = baseDir.resolve("jce-native-" + userName);
        Files.createDirectories(dir);
        applyOwnerOnlyPermissions(dir);
        return dir;
    }

    private static String sanitizeUserName(String value) {
        String sanitized = value.replaceAll("[^a-zA-Z0-9._-]", "_");
        return sanitized.isEmpty() ? "user" : sanitized;
    }

    private static void applyOwnerOnlyPermissions(Path path) {
        if (!FileSystems.getDefault().supportedFileAttributeViews().contains("posix")) {
            return;
        }

        Set<PosixFilePermission> perms = EnumSet.of(
            PosixFilePermission.OWNER_READ,
            PosixFilePermission.OWNER_WRITE,
            PosixFilePermission.OWNER_EXECUTE
        );

        try {
            Files.setPosixFilePermissions(path, perms);
        } catch (IOException ignored) {
            // Best-effort hardening on POSIX systems.
        }
    }

    /**
     * Extracts game_assets.pak from the JAR to the application directory
     * if it doesn't already exist there.  Returns the absolute path.
     */
    private static String extractPakIfNeeded() {
        Path appDir = resolveApplicationDirectory();
        Path pakDest = appDir.resolve("game_assets.pak");

        /* If PAK is already on disk (dev override or previous extract), use it. */
        if (Files.exists(pakDest)) {
            return pakDest.toAbsolutePath().toString();
        }

        /* Try to extract from JAR resource. */
        try (InputStream in = JceRuntime.class.getResourceAsStream("/game_assets.pak")) {
            if (in == null) {
                return null;  /* No PAK bundled — will fall back to native search. */
            }
            Files.copy(in, pakDest, StandardCopyOption.REPLACE_EXISTING);
            return pakDest.toAbsolutePath().toString();
        } catch (IOException e) {
            System.err.println("JCE: failed to extract game_assets.pak: " + e.getMessage());
            return null;
        }
    }

    static String resolveConfigPathForNative() {
        Path appDir = resolveApplicationDirectory();
        return appDir.resolve(".config").resolve("jce.ini").toAbsolutePath().normalize().toString();
    }

    private static Path resolveApplicationDirectory() {
        try {
            Path codePath = Paths.get(JceRuntime.class.getProtectionDomain().getCodeSource().getLocation().toURI())
                .toAbsolutePath()
                .normalize();
            if (Files.isRegularFile(codePath)) {
                Path parent = codePath.getParent();
                if (parent != null) {
                    return parent;
                }
            }
            return codePath;
        } catch (URISyntaxException e) {
            return Paths.get(".").toAbsolutePath().normalize();
        }
    }
}
