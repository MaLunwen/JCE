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
    options = {
        "jce_jni": [True, False],
        # Patent-encumbered codecs (AAC / H.264 / H.265).
        # The vendored sources at engine/src/{audio,video}/third_party/
        # are always compiled (the editor needs them to import legacy
        # assets), so this option is currently informational — see
        # plan E5 follow-up to fully gate them out of the runtime.
        # When you ship binaries, consider:
        #   - jurisdictions where these patents are enforced
        #   - whether you have the required licenses
        # Default OFF for safety; the royalty-free path is Opus + AV1.
        "enable_patented_codecs": [True, False],
    }
    default_options = {
        "jce_jni": False,
        "enable_patented_codecs": False,
        "bgfx/*:tools": True,
    }

    def configure(self):
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
        self.requires("harfbuzz/12.3.0")

        self.requires("bgfx/1.129.8930-495")
        self.requires("cglm/0.9.1")

        self.requires("miniaudio/0.11.22")
        self.requires("opus/1.5.2")
        self.requires("ogg/1.3.5")

        self.requires("dav1d/1.5.3")
        self.requires("libvpx/1.16.0")
        self.requires("libwebm/1.0.0.31")
        
        self.requires("imgui/1.92.6-docking")
        self.requires("flecs/4.1.1")
        self.requires("cjson/1.7.19")
        self.requires("assimp/6.0.2")
        self.requires("cgltf/1.15")
        self.requires("ozz-animation/0.14.1")

        self.requires("physfs/3.2.0")
        self.requires("zstd/1.5.7")
        self.requires("xxhash/0.8.3")

        self.requires("mimalloc/2.2.4")
        self.requires("enkits/1.11")

        self.requires("box2d/3.1.1")
        self.requires("bullet3/3.25")
        self.requires("behaviortree.cpp/4.9.0")

        self.requires("rmlui/4.4")
        self.requires("freetype/2.13.2", force=True)

        self.requires("enet/1.3.18")
        self.requires("protobuf/6.33.5")

        self.requires("tracy/0.13.1")

        # ── Transitive overrides (resolve version conflicts) ─────────
        if self.settings.os == "Linux":
            # bgfx pins wayland/1.23.92 while SDL & others pull 1.24.0
            self.requires("wayland/1.24.0", override=True)

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
        deps.generate()
        tc = CMakeToolchain(self)
        # Forward Conan options to CMake so the build system can react.
        # Always set both True/False to override any stale CMake cache values.
        tc.variables["JCE_BUILD_JNI"] = bool(self.options.jce_jni)
        tc.variables["JCE_ENABLE_PATENTED_CODECS"] = bool(self.options.enable_patented_codecs)
        tc.generate()
