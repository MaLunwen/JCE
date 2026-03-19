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
        "with_assimp": [True, False],
        "jce_jni": [True, False],
    }
    default_options = {
        "with_assimp": True,
        "jce_jni": False,
        "bgfx/*:tools": True,
    }

    def configure(self):
        if self.settings.os in ("Emscripten", "Android", "iOS"):
            # bgfx tools (shaderc) are host-only build tools; they can't run on
            # WASM/Android/iOS targets. The host shaderc.exe is passed via
            # -DJCE_SHADERC_EXECUTABLE in the cross-compile build scripts.
            self.options["bgfx/*"].tools = False

    def requirements(self):
        # ── Core (all platforms) ─────────────────────────────────────
        self.requires("sdl/3.4.0")
        self.requires("sdl_image/3.4.0")
        self.requires("sdl_ttf/3.2.2")

        self.requires("bgfx/1.129.8930-495")
        self.requires("flecs/4.1.1")

        self.requires("glm/1.0.1")
        self.requires("imgui/1.92.5")

        self.requires("zstd/1.5.7")
        self.requires("xxhash/0.8.3")

        self.requires("miniaudio/0.11.22")

        if self.options.get_safe("with_assimp"):
            self.requires("assimp/6.0.2")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        # Forward Conan options to CMake so the build system can react.
        # Always set both True/False to override any stale CMake cache values.
        tc.variables["JCE_USE_ASSIMP"] = bool(self.options.get_safe("with_assimp"))
        tc.variables["JCE_BUILD_JNI"] = bool(self.options.jce_jni)
        tc.generate()
