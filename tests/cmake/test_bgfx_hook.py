import importlib.util
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
HOOK_PATH = ROOT / "conan" / "hooks" / "hook_bgfx_wasm_fix.py"


def load_hook():
    spec = importlib.util.spec_from_file_location("jce_bgfx_hook", HOOK_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class D3D12ResourceGuardPatchTests(unittest.TestCase):
    def test_patches_resource_failure_diagnostics_and_null_guards(self):
        hook = load_hook()
        source = hook._D3D12_RESOURCE_TEST_SOURCE

        patched, applied = hook._patch_d3d12_resource_guards_text(source)

        self.assertEqual(3, applied)
        self.assertIn("ID3D12Resource* resource = NULL;", patched)
        self.assertIn("GetDeviceRemovedReason", patched)
        self.assertIn("JCE D3D12 CreateCommittedResource failed", patched)
        self.assertIn("if (NULL == m_ptr)", patched)
        self.assertIn("if (NULL == staging)", patched)

    def test_patch_is_idempotent(self):
        hook = load_hook()
        once, applied_once = hook._patch_d3d12_resource_guards_text(
            hook._D3D12_RESOURCE_TEST_SOURCE
        )
        twice, applied_twice = hook._patch_d3d12_resource_guards_text(once)

        self.assertEqual(3, applied_once)
        self.assertEqual(0, applied_twice)
        self.assertEqual(once, twice)

    def test_patches_current_bgfx_heap_flags_signature(self):
        hook = load_hook()
        source = hook._D3D12_RESOURCE_TEST_SOURCE.replace(
            "D3D12_HEAP_FLAG_NONE", "_heapFlags", 1
        )

        patched, applied = hook._patch_d3d12_resource_guards_text(source)

        self.assertEqual(3, applied)
        self.assertIn("ID3D12Resource* resource = NULL;", patched)
        self.assertIn("_heapFlags", patched)
        self.assertIn("GetDeviceRemovedReason", patched)


class D3D12PsoGuardPatchTests(unittest.TestCase):
    CURRENT_BGFX_SOURCE = """
			if (NULL == pso)
			{
				DX_CHECK(m_device->CreateComputePipelineState(
					  &desc
					, IID_ID3D12PipelineState
					, (void**)&pso
					) );
			}
			BGFX_FATAL(NULL != pso, Fatal::InvalidShader, "Failed to create PSO!");
			ID3D12PipelineState* pso = getPipelineState(state
				, 0
				, packStencil(BGFX_STENCIL_DEFAULT, BGFX_STENCIL_DEFAULT)
				, 1
				, layouts
				, _blitter.m_program
				, 0
				);
			m_commandList->SetPipelineState(pso);
			m_commandList->SetGraphicsRootSignature(m_rootSignature);
			_commandList->SetPipelineState(getPipelineState(prog) );
					ID3D12PipelineState* pso = getPipelineState(key.m_program);
					if (pso != currentPso)
					ID3D12PipelineState* pso = getPipelineState(
						  state
						, draw.m_rgba
						, draw.m_stencil
						, numStreams
						, layouts
						, key.m_program
						, uint8_t(draw.m_instanceDataStride/16)
						);
					if (pso != currentPso)
"""

    def test_patches_every_current_bgfx_pso_creation_and_use_site(self):
        hook = load_hook()

        patched, applied = hook._patch_d3d12_pso_guards_text(
            self.CURRENT_BGFX_SOURCE
        )

        self.assertEqual(6, applied)
        self.assertIn("JCE: compute PSO create failed", patched)
        self.assertIn("JCE: graphics PSO create failed", patched)
        self.assertIn("JCE: skip debug blit on PSO fail", patched)
        self.assertIn("JCE: skip mip generation on PSO fail", patched)
        self.assertIn("JCE: skip compute on PSO fail", patched)
        self.assertIn("JCE: skip draw on PSO fail", patched)
        self.assertNotIn("SetPipelineState(getPipelineState", patched)

    def test_patch_is_idempotent_for_partially_patched_sources(self):
        hook = load_hook()
        once, applied_once = hook._patch_d3d12_pso_guards_text(
            self.CURRENT_BGFX_SOURCE
        )
        twice, applied_twice = hook._patch_d3d12_pso_guards_text(once)

        self.assertEqual(6, applied_once)
        self.assertEqual(0, applied_twice)
        self.assertEqual(once, twice)


class WasmSimdPatchTests(unittest.TestCase):
    def test_replaces_desktop_sse_with_wasm_simd128_conditionally(self):
        hook = load_hook()
        source = "before\n" + hook._BX_SIMD_OLD + "\nafter\n"

        patched, applied = hook._patch_wasm_simd_flags_text(source)

        self.assertEqual(1, applied)
        self.assertIn("JCE: Emscripten SIMD128 baseline", patched)
        self.assertIn('CMAKE_SYSTEM_NAME STREQUAL "Emscripten"', patched)
        self.assertIn("target_compile_options(bx PUBLIC -msimd128)", patched)
        self.assertIn("-msse4.2", patched)

    def test_patch_is_idempotent(self):
        hook = load_hook()
        source = "before\n" + hook._BX_SIMD_OLD + "\nafter\n"

        once, applied_once = hook._patch_wasm_simd_flags_text(source)
        twice, applied_twice = hook._patch_wasm_simd_flags_text(once)

        self.assertEqual(1, applied_once)
        self.assertEqual(0, applied_twice)
        self.assertEqual(once, twice)


class GraphicsApiTierTests(unittest.TestCase):
    def test_selected_tiers_are_modern_to_stable_without_gl21(self):
        hook = load_hook()

        self.assertEqual(
            {"value": 0, "opengl": 33, "opengles": 30},
            hook._GRAPHICS_TIERS["stable"],
        )
        self.assertEqual(
            {"value": 1, "opengl": 43, "opengles": 31},
            hook._GRAPHICS_TIERS["modern"],
        )
        self.assertEqual(
            {"value": 2, "opengl": 46, "opengles": 32},
            hook._GRAPHICS_TIERS["current"],
        )

    def test_toolchain_patch_is_idempotent_and_switchable(self):
        hook = load_hook()
        source = "set(UNCHANGED ON)\n"

        modern = hook._patch_graphics_toolchain_text(source, "modern")
        modern_twice = hook._patch_graphics_toolchain_text(modern, "modern")
        stable = hook._patch_graphics_toolchain_text(modern, "stable")

        self.assertEqual(modern, modern_twice)
        self.assertEqual(1, modern.count(hook._GRAPHICS_TOOLCHAIN_BEGIN))
        self.assertIn("BGFX_OPENGL_VERSION 43", modern)
        self.assertIn("BGFX_OPENGLES_VERSION 31", modern)
        self.assertNotIn("BGFX_OPENGL_VERSION 43", stable)
        self.assertIn("BGFX_OPENGL_VERSION 33", stable)
        self.assertIn("set(UNCHANGED ON)", stable)

    def test_runtime_version_trace_patch_is_complete_and_idempotent(self):
        hook = load_hook()
        gl_source = "before\n" + hook._GL_API_VERSION_ANCHOR + "after\n"
        vk_source = "before\n" + hook._VK_API_VERSION_ANCHOR + "after\n"

        gl_once, vk_once, applied_once = hook._patch_api_version_traces_text(
            gl_source, vk_source
        )
        gl_twice, vk_twice, applied_twice = hook._patch_api_version_traces_text(
            gl_once, vk_once
        )

        self.assertEqual(2, applied_once)
        self.assertEqual(0, applied_twice)
        self.assertEqual(gl_once, gl_twice)
        self.assertEqual(vk_once, vk_twice)
        self.assertIn("JCE runtime API version:", gl_once)
        self.assertIn("JCE runtime shader version:", gl_once)
        self.assertIn("JCE runtime API version:", vk_once)


if __name__ == "__main__":
    unittest.main()
