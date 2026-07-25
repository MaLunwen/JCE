"""
# conan install . --build=missing -c tools.cmake.cmaketoolchain:generator=Ninja
# conan remove "*" -c
# conan remove "sdl/*"
#
# conan install . --build=assimp* -c tools.cmake.cmaketoolchain:generator=Ninja
#
# rmdir /s /q build
# vs 2022
# cmake -S . -B build/Release -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
"""

from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain, cmake_layout


class JCEConan(ConanFile):
    name = "jce"
    settings = "os", "compiler", "build_type", "arch"

    # ── Options ──────────────────────────────────────────────────────
    # NOTE: legal-/packaging-sensitive switches (JCE_BUILD_JNI,
    # JCE_ENABLE_PATENTED_CODECS) are intentionally NOT exposed here.
    # They are CMake-only and must be set with -D at configure time:
    #   cmake -DJCE_ENABLE_PATENTED_CODECS=OFF  (default ON; forced OFF
    #                                            for the dist variant and
    #                                            for Web — see the root
    #                                            CMakeLists derivation)
    #   cmake -DJCE_BUILD_JNI=ON                (default OFF)
    #
    # The line above used to say the codec option defaults OFF.  It does
    # not: engine/CMakeLists.txt declares it ON, deliberately, because the
    # editor needs AAC/H.264/H.265 to import legacy assets.  A comment that
    # understates what a build produces is worse than none on a
    # licensing-sensitive switch, so it now states the real default and
    # where it is overridden.
    # Keeping them out of Conan options still means `conan install .`
    # itself pulls no patent-encumbered dependency — the vendored codec
    # sources are not Conan packages.  It does NOT mean a subsequent
    # cmake configure leaves them off; that is the option default above.
    default_options = {
        "bgfx/*:tools": True,
        # Build Bullet thread-safe (BULLET2_MULTITHREADING -> the Mt solver
        # classes' internal locks are real, not no-ops). This is what lets the
        # OPT-IN multithreaded physics path (JCE_PHYSICS_MT, default OFF at
        # runtime) step the world in parallel. The default single-threaded,
        # deterministic path is unchanged — it just links a lib that *can* be
        # driven multithreaded. The recipe does not propagate -DBT_THREADSAFE to
        # consumers, so engine/CMakeLists.txt defines it for jce_physics to match.
        "bullet3/*:bt2_thread_locks": True,
        # Tracy in on-demand mode: it only collects/buffers profiling data
        # while a Tracy server is actually connected. Without this, a build with
        # JCE_ENABLE_PROFILING=ON buffers EVERY zone + alloc event in RAM forever
        # when no profiler is attached — an unbounded leak (~20 MB/s in Play,
        # reaching multiple GB over a session).
        "tracy/*:on_demand": True,
        # ── Keep the dependency closure free of copyleft ────────────────
        # The SDK merges every static dependency into one redistributable fat
        # lib (engine/cmake/JCESDKInstall.cmake), so anything that lands in the
        # graph lands in what consumers ship. These three defaults each pulled
        # in a subtree that NO first-party code uses:
        #
        #   harfbuzz:with_glib   -> glib + libiconv + gettext(intl) + pcre2 +
        #                           libffi. glib/iconv/intl are LGPL-2.1+, and
        #                           statically merging them puts a relinking
        #                           obligation on every SDK consumer. Nothing
        #                           here calls a glib API or hb_glib_*; with it
        #                           off, HarfBuzz uses its built-in UCDN
        #                           Unicode functions and JCE shapes through
        #                           FreeType exactly as before.
        #   enable_groot_interface -> cppzmq + ZeroMQ, for the Groot2 debugger.
        #                           No BT publisher exists in this tree. (ZeroMQ
        #                           is MPL-2.0, not LGPL as the audit first
        #                           recorded — dropped for closure size and to
        #                           stop shipping a network stack nothing uses.)
        #   enable_sqlite_logging  -> sqlite3, for BT SQLite logging. No
        #                           SqliteLogger anywhere in the tree.
        #
        # Turning any of these back on is a licensing decision, not just a
        # feature toggle — see docs/audits/dependency-and-language-audit.md.
        "harfbuzz/*:with_glib": False,
        "behaviortree.cpp/*:enable_groot_interface": False,
        "behaviortree.cpp/*:enable_sqlite_logging": False,
        # libtiff's LZMA codec is the ONLY thing pulling xz_utils into the host
        # graph (sdl_image -> libtiff -> xz_utils). The xz package is
        # multi-licensed and its recipe declares GPL-2.0+/GPL-3.0+/LGPL-2.1+
        # alongside Unlicense; rather than argue over which sub-license covers
        # the part we would link, drop it. TIFF itself still loads — only the
        # rare COMPRESSION_LZMA (tag 34925) variant is unsupported.
        #
        # NOTE: strawberryperl (Artistic-1.0/GPL-1.0) is still in the graph for
        # libaom-av1 and nasm, but it is a BUILD-CONTEXT tool that runs on the
        # build machine and is never linked or redistributed, so it carries no
        # distribution obligation.
        "libtiff/*:lzma": False,
    }

    def configure(self):
        if self.settings.os == "Windows":
            # libcurl: native TLS (schannel) keeps the dependency closure
            # small — no OpenSSL build for the ai_dispatch transport.
            self.options["libcurl/*"].with_ssl = "schannel"

        if self.settings.os in ("Emscripten", "Android", "iOS"):
            # bgfx tools (shaderc) are host-only build tools; they can't run on
            # WASM/Android/iOS targets. The host shaderc.exe is passed via
            # -DJCE_SHADERC_EXECUTABLE in the cross-compile build scripts.
            self.options["bgfx/*"].tools = False

        if self.settings.os == "Macos":
            # SDL HIDAPI/haptics on recent SDKs can introduce IOKit symbols that
            # are unavailable on older macOS runtimes (for example, macOS 11).
            # Disable these optional subsystems for desktop runtime compatibility.
            self.options["sdl/*"].hidapi = False
            self.options["sdl/*"].haptic = False

        if self.settings.os in ("iOS", "tvOS", "visionOS", "watchOS"):
            # Desktop OpenGL is not available on Apple mobile platforms;
            # the SDL recipe's package_info() incorrectly references opengl::
            # when the option is True, even though it isn't added as a requirement.
            self.options["sdl/*"].opengl = False

    def requirements(self):
        # ── Core (all platforms) ─────────────────────────────────────
        self.requires("sdl/3.4.0")
        self.requires("sdl_image/3.4.0")

        self.requires("bgfx/1.146.9306-550")

        self.requires("harfbuzz/12.3.0")
        self.requires("freetype/2.14.3", force=True)

        self.requires("miniaudio/0.11.22")
        self.requires("opus/1.5.2")
        self.requires("ogg/1.3.5")
        self.requires("dav1d/1.5.3")
        self.requires("libvpx/1.16.0")
        self.requires("libwebm/1.0.0.31")

        self.requires("imgui/1.92.6-docking")
        self.requires("flecs/4.1.5")
        self.requires("cjson/1.7.19")

        self.requires("assimp/6.0.2")
        self.requires("cgltf/1.15")
        self.requires("meshoptimizer/1.0")
        self.requires("v-hacd/4.1.0")

        self.requires("ozz-animation/0.14.1")
        self.requires("behaviortree.cpp/4.9.0")
        self.requires("recastnavigation/1.6.0")

        self.requires("box2d/3.1.1")
        self.requires("bullet3/3.25")

        self.requires("lua/5.4.8")
        self.requires("rmlui/6.2")

        self.requires("physfs/3.2.0")
        self.requires("zstd/1.5.7")
        self.requires("xxhash/0.8.3")

        self.requires("mimalloc/3.3.2")
        self.requires("enkits/1.11")

        self.requires("libcurl/8.21.0")
        self.requires("enet/1.3.18")
        self.requires("protobuf/7.35.0")

        self.requires("tracy/0.13.1")

        # ── Transitive overrides (resolve version conflicts) ─────────
        if self.settings.os == "Linux":
            # bgfx pins wayland/1.23.92 while SDL & others pull 1.24.0
            self.requires("wayland/1.24.0", override=True)

    def build_requirements(self):
        # protobuf_generate() executes protoc on the build machine.  A target
        # protoc from a WASM/Android/iOS package cannot run there, while an
        # unrelated system protoc may generate code incompatible with the
        # target headers.  Conan's <host_version> token keeps both graph
        # contexts on the exact same protobuf recipe revision and version.
        self.tool_requires("protobuf/<host_version>")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        # Transitive dependency fix for Apple profiles: bgfx aggressively adds
        # macOS-only frameworks on iOS/tvOS. Strip them out before generating.
        if self.settings.os in ("iOS", "tvOS", "visionOS", "watchOS"):
            for dep in self.dependencies.values():
                if dep.ref.name == "bgfx":
                    for fw in ("Cocoa", "IOKit"):
                        if fw in dep.cpp_info.frameworks:
                            dep.cpp_info.frameworks.remove(fw)
                        for comp in dep.cpp_info.components.values():
                            if fw in comp.frameworks:
                                comp.frameworks.remove(fw)

        deps = CMakeDeps(self)
        # Emit a separately named CMake package for the native protoc supplied
        # by build_requirements().  The target-context protobuf package remains
        # authoritative for headers and libraries.
        deps.build_context_activated = ["protobuf"]
        deps.build_context_suffix = {"protobuf": "_BUILD"}
        # The protobuf recipe assigns absolute CMake names to its components.
        # CMakeDeps cannot suffix those automatically, so make the build graph
        # collision-free explicitly.  We only execute protoc from this context;
        # none of these host libraries may enter a target link interface.
        for component in (
            "utf8_range",
            "utf8_validity",
            "libprotobuf",
            "libprotoc",
        ):
            deps.set_property(
                f"protobuf::{component}",
                "cmake_target_name",
                f"protobuf_BUILD::{component}_BUILD",
                build_context=True,
            )
        deps.generate()
        tc = CMakeToolchain(self)
        # JCE_BUILD_JNI and JCE_ENABLE_PATENTED_CODECS are intentionally
        # NOT forwarded from Conan options — they are CMake-only switches.
        # Set them with -D at cmake configure time.
        tc.generate()
