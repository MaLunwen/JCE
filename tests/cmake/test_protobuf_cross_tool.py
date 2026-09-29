import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
CONANFILE = ROOT / "conanfile.py"
CMAKE = ROOT / "CMakeLists.txt"


class ProtobufCrossToolContractTests(unittest.TestCase):
    def test_recipe_provides_version_matched_host_protoc(self):
        recipe = CONANFILE.read_text(encoding="utf-8")

        self.assertIn('self.tool_requires("protobuf/<host_version>")', recipe)
        self.assertIn('deps.build_context_activated = ["protobuf"]', recipe)
        self.assertIn('deps.build_context_suffix = {"protobuf": "_BUILD"}', recipe)
        self.assertIn('f"protobuf_BUILD::{component}_BUILD"', recipe)
        self.assertIn("build_context=True", recipe)

    def test_cmake_uses_only_the_conan_build_context_tool(self):
        cmake = CMAKE.read_text(encoding="utf-8")

        self.assertIn("find_package(protobuf_BUILD REQUIRED CONFIG)", cmake)
        self.assertIn("protobuf_BUILD_PACKAGE_FOLDER_", cmake)
        self.assertIn("protobuf_BUILD_VERSION_STRING", cmake)
        self.assertNotIn("file(GLOB _protoc_list", cmake)
        self.assertNotIn("Searching Conan2 cache for native protoc", cmake)


if __name__ == "__main__":
    unittest.main()
