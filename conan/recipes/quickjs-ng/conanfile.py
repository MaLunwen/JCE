"""quickjs-ng — a LOCAL conan recipe, because conan-center's cannot be used.

WHY THIS EXISTS AT ALL.  conan-center ships `quickjs/<date>`, which is
Bellard's fork, and its recipe refuses this project outright:

    def validate(self):
        if is_msvc(self):
            raise ConanInvalidConfiguration(
                f"{self.ref} can not be built on Visual Studio and msvc.")

That is not a packaging quirk a conan hook could patch — Bellard's tree is
Makefile-only with no MSVC support at all, so the refusal is honest.  The
quickjs-ng fork ships CMake, handles `_MSC_VER` explicitly (it disables
computed-goto dispatch there) and builds under MSVC unchanged; conan-center has
no recipe for it.

WHY A RECIPE AND NOT A VENDORED TREE.  Both were tried.  Vendoring works and it
costs two things this route does not:
  * tools/audit/check_dependency_licenses.py resolves the CONAN graph, so a
    vendored dependency is invisible to the licence gate.  MIT would have
    passed anyway — but the next vendored dependency would get no check.
  * lua, the other embedded runtime this engine carries, comes from conan
    (`lua/5.4.8`).  Two embedded language runtimes acquired two different ways
    is a difference every later reader has to learn.

This is the first local recipe in this repository.  scripts/jce.py exports
conan/recipes/*/ before every `conan install`, the same way it syncs
conan/hooks/.
"""

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get
from conan.tools.scm import Version

import os

required_conan_version = ">=2.0"


class QuickJSNgConan(ConanFile):
    name = "quickjs-ng"
    description = "A fork of QuickJS, a small and embeddable JavaScript engine"
    license = "MIT"
    url = "https://github.com/quickjs-ng/quickjs"
    homepage = "https://github.com/quickjs-ng/quickjs"
    topics = ("javascript", "interpreter", "embedded")

    package_type = "static-library"
    settings = "os", "arch", "compiler", "build_type"
    options = {"fPIC": [True, False]}
    default_options = {"fPIC": True}

    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

    def layout(self):
        cmake_layout(self)

    def source(self):
        get(self, **self.conan_data["sources"][self.version], strip_root=True)

    def generate(self):
        tc = CMakeToolchain(self)
        # The library only.  qjs/qjsc are a REPL and an offline compiler, and
        # the test suite needs a checkout this package does not ship.
        tc.cache_variables["BUILD_QJS_LIBC"] = False
        tc.cache_variables["BUILD_EXAMPLES"] = False
        tc.cache_variables["BUILD_STATIC_QJS_LIBC"] = False
        tc.cache_variables["QJS_BUILD_LIBC"] = False
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure(build_script_folder=self.source_folder)
        cmake.build(target="qjs")

    def package(self):
        copy(self, "LICENSE", self.source_folder,
             os.path.join(self.package_folder, "licenses"))
        # Headers a consumer needs: quickjs.h includes cutils.h and list.h.
        for h in ("quickjs.h", "quickjs-atom.h", "quickjs-opcode.h",
                  "cutils.h", "list.h", "libunicode.h", "libregexp.h",
                  "libregexp-opcode.h", "quickjs-c-atomics.h", "dtoa.h"):
            copy(self, h, self.source_folder,
                 os.path.join(self.package_folder, "include"))
        for pat in ("*.lib", "*.a"):
            copy(self, pat, self.build_folder,
                 os.path.join(self.package_folder, "lib"), keep_path=False)

    def package_info(self):
        self.cpp_info.libs = ["qjs"]
        self.cpp_info.set_property("cmake_file_name", "quickjs-ng")
        self.cpp_info.set_property("cmake_target_name", "quickjs-ng::quickjs-ng")
        if self.settings.os in ("Linux", "FreeBSD"):
            self.cpp_info.system_libs = ["m", "dl", "pthread", "atomic"]
