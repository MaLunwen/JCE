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

    static {
        CLASSIFIER = detectClassifier();
        LIB_FILE = detectLibFileName();
        RESOURCE_PATH = "/natives/" + CLASSIFIER + "/" + LIB_FILE;
        loadNativeLibrary();
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

    private static native long nativeCreate();
    private static native boolean nativeIterate(long handle);
    private static native boolean nativeShouldQuit(long handle);
    private static native void nativeDestroy(long handle);

    public void init() {
        if (nativeHandle != 0L) {
            return;
        }

        System.setProperty("jce.config.path", resolveConfigPathForNative());
        nativeHandle = nativeCreate();
        if (nativeHandle == 0L) {
            throw new IllegalStateException("Failed to create native JCE engine");
        }
    }

    public boolean iterate() {
        ensureInitialized();
        return nativeIterate(nativeHandle);
    }

    public boolean shouldQuit() {
        ensureInitialized();
        return nativeShouldQuit(nativeHandle);
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
