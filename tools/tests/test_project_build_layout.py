"""Actual-source positive/removal controls for the layout wiring gate.

No native compilation, application, GUI or filesystem publication occurs here.
"""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.lint.check_project_build_layout import inspect_sources, load_sources


class ProjectBuildLayoutControls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sources = load_sources()

    def changed(self, owner, old, new, count=1):
        sources = self.sources.copy()
        self.assertEqual(sources[owner].count(old), count, old)
        sources[owner] = sources[owner].replace(old, new)
        return sources

    def rejects(self, owner, old, new, reason, count=1):
        failures = inspect_sources(self.changed(owner, old, new, count))
        self.assertTrue(any(reason in failure for failure in failures), failures)

    def test_actual_owners_and_call_sites_pass(self):
        self.assertEqual(inspect_sources(self.sources), [])

    def test_formatting_and_real_comments_do_not_change_wiring(self):
        sources = self.changed('manager',
            'jce_project_build_output_dir(build_dir, variant);',
            'jce_project_build_output_dir ( /* shared owner */\n build_dir , variant );')
        self.assertEqual(inspect_sources(sources), [])

    def test_build_authority_cannot_collapse_cache_into_publication(self):
        self.rejects('target',
            'return root + "build/" + jce_project_build_platform_tag(platform) + "-" + arch;',
            'return root + "dist/apps/" + jce_project_build_platform_tag(platform) + "-" + arch;',
            'build root')
        self.rejects('target', 'return build_root + "/" + variant;',
            'return build_root + "/release";', 'variant output')
        self.rejects('package', 'root + "/dist/apps/"', 'root + "/build/"',
            'final package')
        self.rejects('header', 'return dist ? "dist" : "release";',
            'return "release";', 'Dist selection')

    def test_manager_call_removal_is_not_rescued_by_comment_or_string_decoys(self):
        call = 'output_dir = jce_project_build_output_dir(build_dir, variant);'
        for decoy in ('/* ' + call + ' */',
                      'const char *unused = "output_dir = jce_project_build_output_dir(build_dir, variant);";'):
            with self.subTest(decoy=decoy):
                self.rejects('manager', call, 'output_dir = build_dir; ' + decoy,
                             'manager must call the shared variant output owner')
        self.rejects('manager',
            'build_dir = jce_project_build_root(project, build_platform, arch);',
            'build_dir = package_out_dir;', 'manager must call the shared build root owner')

    def test_manager_selected_request_and_publication_fields_are_independent(self):
        for old, new, reason in (
            ('exe_name = jce_project_build_exe_name(target, cfg->exe_name);',
             'exe_name = target;', 'edited artifact naming'),
            ('output_dir = jce_project_build_output_dir(build_dir, variant);',
             'output_dir = jce_project_build_output_dir(build_dir, "release");', 'shared variant output'),
            ('p.output_dir        = output_dir;', 'p.output_dir = package_out_dir;', 'variant output must reach'),
            ('g_finish.out_dir     = p.package_out_dir;', 'g_finish.out_dir = p.output_dir;', 'publication destination'),
            ('g_finish.artifact_a    = p.output_dir + PATH_SEP_CHR_LOCAL + p.exe_name;',
             'g_finish.artifact_a = p.build_dir + PATH_SEP_CHR_LOCAL + p.exe_name;', 'artifact verification'),
        ):
            with self.subTest(old=old):
                self.rejects('manager', old, new, reason)

    def test_cmake_runtime_and_archive_remain_in_build_output(self):
        for flag, field in (('RUNTIME', 'output_dir'), ('LIBRARY', 'output_dir'), ('ARCHIVE', 'archive_dir')):
            with self.subTest(flag=flag):
                self.rejects('manager',
                    f'a += " -DCMAKE_{flag}_OUTPUT_DIRECTORY=" + qtok(p.{field});',
                    f'a += " -DCMAKE_{flag}_OUTPUT_DIRECTORY=" + qtok(p.package_out_dir);',
                    {'RUNTIME': 'runtime output', 'LIBRARY': 'library output', 'ARCHIVE': 'archive output'}[flag])

    def test_preview_must_follow_edited_arch_variant_and_executable(self):
        for old, new, reason in (
            ('? s_bp.arch_idx : 0]', '? 0 : 0]', 'index the selected architecture'),
            ('arch = arch_sel ? arch_sel : host_arch_triplet();', 'arch = host_arch_triplet();', 'selected architecture with host fallback'),
            ('jce_project_build_variant(s_bp.use_dist)) + "/" +', '"release") + "/" +', 'same root/output/variant/executable'),
            ('jce_project_build_exe_name(target, exe);', 'jce_project_build_exe_name(target, nullptr);', 'same root/output/variant/executable'),
            ('std::string predicted = guess_output_exe(p.preset);', 'std::string predicted = "stale.exe";', 'visible build preview'),
        ):
            with self.subTest(old=old):
                self.rejects('panel', old, new, reason)

    def test_build_and_package_request_removal_is_rejected_independently(self):
        self.rejects('panel', 'pcfg.arch          = arch;',
            'pcfg.arch = nullptr;', 'native build: selected architecture')
        self.rejects('panel', 'pcfg.exe_name      = exe;',
            'pcfg.exe_name = nullptr;', 'edited request', count=2)
        self.rejects('panel',
            'pcfg.variant       = jce_project_build_variant(s_bp.use_dist);',
            'pcfg.variant = "release";', 'edited request', count=2)
        self.rejects('panel', 'pcfg.package_out_dir = out_dir.c_str();',
            'pcfg.package_out_dir = nullptr;', 'default publication output')
        self.rejects('panel',
            'root, name, plat, arch_cli,\n                        jce_project_build_variant(s_bp.use_dist), &out_dir, &path_error)',
            'root, name, plat, host_arch_triplet(),\n                        "release", &out_dir, &path_error)',
            'selected app/arch/variant')

    def test_package_preview_and_destination_cannot_ignore_architecture_edits(self):
        self.rejects('panel', '?\n                s_arch_cli[s_bp.arch_idx] : host_arch_triplet();',
            '?\n                host_arch_triplet() : host_arch_triplet();',
            'visible package preview must consume the selected architecture')
        self.rejects('panel',
            '? s_arch_cli[s_bp.arch_idx] : host_arch_triplet();',
            '? host_arch_triplet() : host_arch_triplet();',
            'destination architecture must use the selected architecture')

    def test_shared_default_package_call_and_publish_call_cannot_disappear(self):
        self.rejects('panel',
            'jce_build_package_default_path(s_bp.project_root,',
            'legacy_package_path(s_bp.project_root,', 'visible package preview')
        self.rejects('manager',
            'jce_build_package_publish_dist(exe, g_finish.package_reports_dir,',
            'copy_without_audit(exe, g_finish.package_reports_dir,', 'verified publisher')


if __name__ == '__main__':
    unittest.main()
