"""CPU-only contract controls; every generated fixture is explicitly synthetic.

The observed status below is a schema input, never a claim of browser/GPU work.
Only the archived Web09 failure is an actual observation, and remains read-only.
"""
from __future__ import annotations

import copy
import hashlib
import json
import math
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import web_render_quality_evidence as admission

from web_render_quality_evidence import validate_web_render_quality


ARTIFACT = 'a' * 64
OBSERVED = 'ACTUAL_WEB_GROUND_QUALITY_OBSERVED'
ROOT = Path(__file__).resolve().parents[1]
SCOPE = ('Instrumented actual WebGL ground draw and linked shader/uniform state. '
         'No timing or every-fragment claim.')
ARCHIVED_WEB09 = (ROOT / 'build' / 'verification' / 'perf27' / 'web09-acceptance' /
                  'web-render-quality-final09-v2.json')

# A supplementary Unicode comment makes UTF-8 bytes, Python characters and
# JavaScript UTF-16 source.length different, without invoking any compiler.
DIRECT_SOURCE = """#version 300 es
// SYNTHETIC CPU CONTRACT CONTROL ONLY. \U0001f9ea
precision highp float;
uniform vec4 u_pbrParams;
out vec4 color;
float classic_lobe_power(float base, float exponent)
{
    if (exponent == 0.0) return 1.0;
    if (base <= 0.0) return 0.0;
    return pow(base, exponent);
}
void main()
{
    vec3 N = vec3(0.0, 1.0, 0.0);
    vec3 skyH = normalize(vec3(0.0, 1.0, 1.0));
    vec3 groundH = normalize(vec3(0.0, -1.0, 1.0));
    vec3 H = normalize(vec3(1.0, 1.0, 1.0));
    float shine = max(0.0, (1.0 - u_pbrParams.y) * 128.0);
    float sky_lobe = classic_lobe_power(max(0.5 * dot(N, skyH) + 0.5, 0.0), shine);
    float ground_lobe = classic_lobe_power(max(0.5 * dot(N, groundH) + 0.5, 0.0), shine);
    float directional_lobe = classic_lobe_power(max(dot(N, H), 0.0), shine);
    color = vec4(vec3(sky_lobe + ground_lobe + directional_lobe), 1.0);
}
"""

INLINE_SOURCE = """#version 300 es
// SYNTHETIC CPU CONTRACT CONTROL ONLY: compiler-style inline witness.
precision highp float;
uniform vec4 u_pbrParams;
out vec4 color;
void main()
{
    vec3 N = vec3(0.0, 1.0, 0.0);
    vec3 skyH = normalize(vec3(0.0, 1.0, 1.0));
    vec3 groundH = normalize(vec3(0.0, -1.0, 1.0));
    vec3 H = normalize(vec3(1.0, 1.0, 1.0));
    float shine = max(0.0, (1.0 - u_pbrParams.y) * 128.0);
    float sky_base = max(0.5 * dot(N, skyH) + 0.5, 0.0);
    float sky_lobe;
    if ((shine == 0.0)) {
        sky_lobe = 1.0;
    } else {
        if ((sky_base <= 0.0)) {
            sky_lobe = 0.0;
        } else {
            sky_lobe = pow(sky_base, shine);
        };
    };
    float ground_base = max(0.5 * dot(N, groundH) + 0.5, 0.0);
    float ground_lobe;
    if ((shine == 0.0)) {
        ground_lobe = 1.0;
    } else {
        if ((ground_base <= 0.0)) {
            ground_lobe = 0.0;
        } else {
            ground_lobe = pow(ground_base, shine);
        };
    };
    float directional_base = max(dot(N, H), 0.0);
    float directional_lobe;
    if ((shine == 0.0)) {
        directional_lobe = 1.0;
    } else {
        if ((directional_base <= 0.0)) {
            directional_lobe = 0.0;
        } else {
            directional_lobe = pow(directional_base, shine);
        };
    };
    color = vec4(vec3(sky_lobe + ground_lobe + directional_lobe), 1.0);
}
"""


def js_length(source):
    return len(source.encode('utf-16-le')) // 2


def js_prefix(source):
    return source.encode('utf-16-le')[:600].decode('utf-16-le', errors='surrogatepass')


def source_receipt(path, raw):
    path.write_bytes(raw)
    return {'path': str(path.resolve()), 'bytes': len(raw),
            'sha256': hashlib.sha256(raw).hexdigest()}


class WebRenderQualityControls(unittest.TestCase):
    def setUp(self):
        # Explicit synthetic source identities are approved only inside this
        # CPU test patch. No production fixture_tag bypass or GPU claim exists.
        synthetic_sources = frozenset(hashlib.sha256(s.encode("utf-8")).hexdigest()
                                      for s in (DIRECT_SOURCE, INLINE_SOURCE))
        approved = admission.REVIEWED_LINKED_SOURCE_SHA256 | synthetic_sources
        patch = mock.patch.object(admission, "REVIEWED_LINKED_SOURCE_SHA256", approved)
        patch.start()
        self.addCleanup(patch.stop)
        directory = ROOT / 'build' / 'verification'
        directory.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix='web-quality-control-', dir=directory)
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        length = math.hypot(66, 84, 60)
        uniforms = {
            'u_pbrParams': [1 / 15, 1, 1, .5],
            'u_baseColorFactor': [1, 1, 1, 1],
            'u_lookHemiGround': [247 / 255, 1, 243 / 255, 1],
            'u_ambientColor': [243 / 255, 253 / 255, 1, .6],
            'u_dirLights': [66 / length, -84 / length, 60 / length, .7,
                            1, .96, .9, 0] + [0] * 8,
            'u_lightCounts': [1, 0, 0, 1],
            'u_csmParams': [1 / 512, 0, .012, 1.4],
            'u_shadowQuality': [0, 0, 8, 0],
        }
        self.data = {
            'schema': 1, 'status': OBSERVED,
            'fixture_tag': 'SYNTHETIC_CPU_CONTRACT_CONTROL_NO_GPU',
            'artifact_sha256': ARTIFACT, 'served_artifact_sha256': ARTIFACT,
            'executed_artifact_sha256': ARTIFACT,
            'artifact_url': 'http://127.0.0.1:8785/synthetic-control.html',
            'executed_transport': 'Synthetic CPU contract control; no browser execution occurred.',
            'run_id': 'synthetic_quality_1', 'generated_utc': '2026-10-08T12:00:00.000Z',
            'completed_utc': '2026-10-08T12:00:10.000Z', 'scope': SCOPE, 'errors': [],
            'contexts': [{'kind': 'webgl2', 'version': 'WebGL 2.0 synthetic control',
                          'shading_language': 'WebGL GLSL ES 3.00 synthetic control',
                          'vendor': 'SYNTHETIC CPU CONTROL', 'renderer': 'SYNTHETIC CPU CONTROL',
                          'lost': False}],
            'lost': 0, 'draw_calls': 30, 'draws36': 2,
            'ground_draws': [{'method': 'drawElements', 'count': 36, 'timestamp_ms': 5000,
                             'linked': True, 'fragment_compiled': True, 'gl_error': 0,
                             'uniforms': uniforms}],
        }
        self.install_source(self.data, DIRECT_SOURCE)

    def install_source(self, data, source):
        receipt = source_receipt(self.directory / 'synthetic-fragment.glsl', source.encode('utf-8'))
        for draw in data['ground_draws']:
            draw['fragment_source'] = copy.deepcopy(receipt)
        data['program_observations'] = [{
            'names': list(data['ground_draws'][0]['uniforms']),
            'uniforms': copy.deepcopy(data['ground_draws'][0]['uniforms']),
            'source_bytes': js_length(source), 'source_start': js_prefix(source),
            'contains_finite_helper': 'classic_lobe_power' in source,
        }]

    def assert_rejected(self, mutate, artifact=ARTIFACT):
        data = copy.deepcopy(self.data)
        mutate(data)
        with self.assertRaises(ValueError):
            validate_web_render_quality(data, artifact)

    def test_synthetic_direct_contract_and_raw_source_identity(self):
        result = validate_web_render_quality(self.data, ARTIFACT)
        self.assertIsInstance(result, dict)
        self.assertEqual(result['status'], OBSERVED)
        self.assertEqual(result['ground_draw_count'], 1)
        receipt = self.data['ground_draws'][0]['fragment_source']
        self.assertNotEqual(receipt['bytes'], js_length(DIRECT_SOURCE))
        self.assertNotEqual(js_length(DIRECT_SOURCE), len(DIRECT_SOURCE))

    def test_synthetic_inline_contract_does_not_require_function_name(self):
        self.install_source(self.data, INLINE_SOURCE)
        self.assertFalse(self.data['program_observations'][0]['contains_finite_helper'])
        self.assertEqual(validate_web_render_quality(self.data, ARTIFACT)['ground_draw_count'], 1)

    def test_two_ordered_draw_methods_and_observation_uniforms_are_not_authority(self):
        second = copy.deepcopy(self.data['ground_draws'][0])
        second.update(method='drawElementsInstanced', timestamp_ms=6000)
        self.data['ground_draws'].append(second)
        self.data['program_observations'][0]['uniforms']['u_lookHemiGround'] = [0] * 4
        self.assertEqual(validate_web_render_quality(self.data, ARTIFACT)['ground_draw_count'], 2)

    def test_finite_shadow_vectors_and_well_typed_extra_json_are_allowed(self):
        self.data['ground_draws'][0]['uniforms'].update(
            u_csmParams=[1 / 1024, 1, .02, 2], u_shadowQuality=[1, 2, 8, 0])
        self.data['extra_diagnostic'] = {'enabled': False, 'items': [None, 'synthetic', {'value': 1.5}]}
        validate_web_render_quality(self.data, ARTIFACT)

    def test_reject_unknown_current_artifact_identity(self):
        for identity in (None, '', 'unknown', 'g' * 64, 'A' * 64, True, 123, [ARTIFACT]):
            with self.subTest(identity=identity), self.assertRaises(ValueError):
                validate_web_render_quality(self.data, identity)
        for field in ('artifact_sha256', 'served_artifact_sha256', 'executed_artifact_sha256'):
            for invalid in ('b' * 64, '', None, True):
                with self.subTest(field=field, invalid=invalid):
                    self.assert_rejected(lambda d: d.update({field: invalid}))

    def test_reject_missing_actual_observations_and_wrong_top_level_types(self):
        required = ('schema', 'status', 'artifact_sha256', 'served_artifact_sha256',
                    'executed_artifact_sha256', 'artifact_url', 'run_id', 'generated_utc',
                    'completed_utc', 'scope', 'executed_transport', 'errors', 'contexts',
                    'lost', 'draw_calls', 'draws36', 'ground_draws', 'program_observations')
        for field in required:
            with self.subTest(field=field):
                self.assert_rejected(lambda d: d.pop(field))
        for invalid in (None, [], '{}', True):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                validate_web_render_quality(invalid, ARTIFACT)
        for field, invalid in (('schema', True), ('schema', 2), ('status', 'FAILED'),
                               ('status', 'NOT_RUN'), ('errors', [{'kind': 'console', 'message': 'failure'}]),
                               ('errors', None), ('scope', ''), ('executed_transport', [])):
            with self.subTest(field=field, invalid=invalid):
                self.assert_rejected(lambda d: d.update({field: invalid}))

    def test_reject_bad_url_run_id_and_utc_interval(self):
        invalid = {
            'artifact_url': ('', 'file:///synthetic.html', 'http:///missing-host',
                             self.data['artifact_url'] + '?qa=1', self.data['artifact_url'] + '#x', None),
            'run_id': ('', 'with space', 'x' * 65, '\u5854', 'x\n', 1),
            'generated_utc': ('2026-10-08T12:00:00', '2026-10-08T12:00:00+08:00',
                              'not-a-date', '2026-10-08T12:00:10Z', '2026-10-08T12:00:11Z'),
            'completed_utc': ('2026-10-08T12:00:00Z', '2026-10-08T11:59:59Z',
                              '2026-13-08T12:00:10Z', '2026-10-08T12:00:10+08:00', None),
        }
        for field, values in invalid.items():
            for value in values:
                with self.subTest(field=field, value=value):
                    self.assert_rejected(lambda d: d.update({field: value}))

    def test_reject_missing_multiple_lost_or_malformed_context(self):
        for contexts in ([], self.data['contexts'] * 2, {}, [None]):
            with self.subTest(contexts=contexts):
                self.assert_rejected(lambda d: d.update(contexts=contexts))
        for field, invalid in (('kind', 'webgl'), ('kind', 'experimental-webgl'),
                               ('lost', True), ('lost', 0), ('lost', None),
                               ('vendor', None), ('renderer', ''), ('version', []),
                               ('shading_language', 123)):
            with self.subTest(field=field, invalid=invalid):
                self.assert_rejected(lambda d: d['contexts'][0].update({field: invalid}))
        for field in ('kind', 'version', 'shading_language', 'vendor', 'renderer', 'lost'):
            with self.subTest(missing=field):
                self.assert_rejected(lambda d: d['contexts'][0].pop(field))

    def test_reject_loss_bad_counters_or_inconsistent_draw_count(self):
        for field, values in {
            'lost': (1, False, -1, 0.0, '0'),
            'draw_calls': (0, True, -1, 30.0, '30'),
            'draws36': (0, True, -1, 2.0, '2', 31),
        }.items():
            for invalid in values:
                with self.subTest(field=field, invalid=invalid):
                    self.assert_rejected(lambda d: d.update({field: invalid}))
        for draws in ([], self.data['ground_draws'] * 3, {}, [None]):
            with self.subTest(draws=draws):
                self.assert_rejected(lambda d: d.update(ground_draws=draws))
        self.assert_rejected(lambda d: (d['ground_draws'].append(copy.deepcopy(d['ground_draws'][0])),
                                        d.update(draws36=1)))
        self.assert_rejected(lambda d: (d['ground_draws'].append(copy.deepcopy(d['ground_draws'][0])),
                                        d['ground_draws'][1].update(timestamp_ms=4999)))

    def test_reject_uncompiled_draw_bad_gl_and_invalid_draw_timestamp(self):
        changes = {
            'method': ('drawArrays', '', None), 'count': (35, True, 36.0),
            'linked': (False, 1, None), 'fragment_compiled': (False, 1, None),
            'gl_error': (1280, False, 0.0),
            'timestamp_ms': (True, float('nan'), float('inf'), '5000', 10001.5),
        }
        for field, values in changes.items():
            for invalid in values:
                with self.subTest(field=field, invalid=invalid):
                    self.assert_rejected(lambda d: d['ground_draws'][0].update({field: invalid}))
        for field in ('method', 'count', 'linked', 'fragment_compiled', 'gl_error',
                      'timestamp_ms', 'uniforms', 'fragment_source'):
            with self.subTest(missing=field):
                self.assert_rejected(lambda d: d['ground_draws'][0].pop(field))

    def test_reject_wrong_ground_uniform_fingerprint_and_shapes(self):
        expected = self.data['ground_draws'][0]['uniforms']
        pinned = ('u_pbrParams', 'u_baseColorFactor', 'u_lookHemiGround',
                  'u_ambientColor', 'u_dirLights', 'u_lightCounts')
        for field in pinned:
            with self.subTest(fingerprint=field):
                self.assert_rejected(lambda d: d['ground_draws'][0]['uniforms'][field].__setitem__(0, -9))
        for field, values in expected.items():
            invalid_values = (values[:-1], None, {'nested': values}, [True] + values[1:],
                              [float('nan')] + values[1:], [float('inf')] + values[1:],
                              ['0'] + values[1:], tuple(values))
            for invalid in invalid_values:
                with self.subTest(field=field, invalid=invalid):
                    self.assert_rejected(lambda d: d['ground_draws'][0]['uniforms'].update({field: invalid}))
            with self.subTest(missing=field):
                self.assert_rejected(lambda d: d['ground_draws'][0]['uniforms'].pop(field))

    def test_synthetic_unrelated_36_index_program_can_have_partial_selected_uniforms(self):
        # The producer reports a program before testing the ground fingerprint.
        # An unrelated shadow/depth program need not expose ground uniforms.
        for uniforms in ({"u_pbrParams": [0, 0, 0, .5]}, {}):
            with self.subTest(synthetic_non_ground_uniforms=uniforms):
                data = copy.deepcopy(self.data)
                data["program_observations"].insert(0, {
                    "names": ["u_model[0]"], "uniforms": uniforms,
                    "source_bytes": 42, "source_start": "#version 300 es\n// SYNTHETIC shadow source",
                    "contains_finite_helper": False,
                })
                result = validate_web_render_quality(data, ARTIFACT)
                self.assertEqual(result["ground_draw_count"], 1)

    def test_reject_malformed_program_observation_and_missing_source_association(self):
        for observations in ([], {}, [None]):
            with self.subTest(observations=observations):
                self.assert_rejected(lambda d: d.update(program_observations=observations))
        for field, invalid in (('names', []), ('names', 'u_pbrParams'), ('names', [None]),
                               ('uniforms', []), ('source_bytes', True), ('source_bytes', 0),
                               ('source_start', []), ('source_start', 'wrong program'),
                               ('contains_finite_helper', 'true')):
            with self.subTest(field=field, invalid=invalid):
                self.assert_rejected(lambda d: d['program_observations'][0].update({field: invalid}))
        for field in ('names', 'uniforms', 'source_bytes', 'source_start', 'contains_finite_helper'):
            with self.subTest(missing=field):
                self.assert_rejected(lambda d: d['program_observations'][0].pop(field))
        self.assert_rejected(lambda d: d['program_observations'][0].update(
            source_bytes=d['program_observations'][0]['source_bytes'] + 1))
        self.assert_rejected(lambda d: d['program_observations'][0]['uniforms'].update(
            u_csmParams=[0, 0, False, 0]))

    def test_reject_non_json_and_nonfinite_extra_nested_values(self):
        for invalid in ({'nested': [float('nan')]}, {'nested': [float('inf')]},
                        {'nested': (1, 2)}, {'nested': b'bytes'}, {'nested': {1, 2}},
                        {1: 'non-string JSON key'}):
            with self.subTest(invalid=invalid):
                self.assert_rejected(lambda d: d.update(extra_diagnostic=invalid))

    def test_reject_fragment_receipt_identity_and_missing_file(self):
        for field, values in {
            'path': ('relative.glsl', '', None, []),
            'sha256': ('b' * 64, '', None, True),
            'bytes': (0, True, -1, 1.0, '1'),
        }.items():
            for invalid in values:
                with self.subTest(field=field, invalid=invalid):
                    self.assert_rejected(lambda d: d['ground_draws'][0]['fragment_source'].update({field: invalid}))
        for field in ('path', 'sha256', 'bytes'):
            with self.subTest(missing=field):
                self.assert_rejected(lambda d: d['ground_draws'][0]['fragment_source'].pop(field))
        self.assert_rejected(lambda d: d['ground_draws'][0]['fragment_source'].update(
            bytes=d['ground_draws'][0]['fragment_source']['bytes'] + 1))
        self.assert_rejected(lambda d: d['ground_draws'][0]['fragment_source'].update(
            path=str(self.directory / 'missing.glsl')))

    def test_reject_file_changed_after_receipt_even_with_same_byte_count(self):
        receipt = self.data['ground_draws'][0]['fragment_source']
        path = Path(receipt['path'])
        raw = path.read_bytes()
        changed = raw.replace(b'SYNTHETIC', b'CHANGED__', 1)
        self.assertEqual(len(raw), len(changed))
        self.assertNotEqual(raw, changed)
        path.write_bytes(changed)
        with self.assertRaises(ValueError):
            validate_web_render_quality(self.data, ARTIFACT)

    def test_reject_non_utf8_source_even_with_matching_receipt(self):
        receipt = source_receipt(self.directory / 'non-utf8.glsl', b'\xff\xfe' + DIRECT_SOURCE.encode('utf-8'))
        self.data['ground_draws'][0]['fragment_source'] = receipt
        with self.assertRaises(ValueError):
            validate_web_render_quality(self.data, ARTIFACT)

    def test_reject_name_only_missing_guards_or_unwitnessed_lobes(self):
        sources = {
            'name in comment only': '#version 300 es\n// classic_lobe_power\nvoid main() { }',
            'unguarded power': DIRECT_SOURCE.replace(
                'if (exponent == 0.0) return 1.0;\n    if (base <= 0.0) return 0.0;\n    ', ''),
            'missing zero exponent guard': DIRECT_SOURCE.replace(
                '    if (exponent == 0.0) return 1.0;\n', ''),
            'missing base guard': DIRECT_SOURCE.replace('    if (base <= 0.0) return 0.0;\n', ''),
            'incorrect zero exponent value': DIRECT_SOURCE.replace(
                'if (exponent == 0.0) return 1.0;', 'if (exponent == 0.0) return 0.0;'),
            'only two legitimate lobe calls': DIRECT_SOURCE.replace(
                'classic_lobe_power(max(dot(N, H), 0.0), shine)', 'pow(max(dot(N, H), 0.0), shine)'),
            'inline missing guard': INLINE_SOURCE.replace(
                'if ((directional_base <= 0.0))', 'if ((directional_base < 0.0))'),
        }
        for name, source in sources.items():
            with self.subTest(name=name):
                data = copy.deepcopy(self.data)
                self.install_source(data, source)
                with self.assertRaises(ValueError):
                    validate_web_render_quality(data, ARTIFACT)

    def test_reject_same_length_changed_source_tail_with_fresh_receipt(self):
        # Prefix, JS length, helper flag and valid guards remain unchanged.
        changed = DIRECT_SOURCE.replace("128.0", "127.0")
        self.assertNotEqual(changed, DIRECT_SOURCE)
        self.assertEqual(js_length(changed), js_length(DIRECT_SOURCE))
        self.assertEqual(js_prefix(changed), js_prefix(DIRECT_SOURCE))
        data = copy.deepcopy(self.data)
        self.install_source(data, changed)
        with self.assertRaises(ValueError):
            validate_web_render_quality(data, ARTIFACT)

    def test_reject_guard_preserved_but_lobe_overwritten_by_unguarded_pow(self):
        changed = DIRECT_SOURCE.replace(
            "    color = vec4", "    directional_lobe = pow(max(dot(N, H), 0.0), shine);\n    color = vec4")
        self.assertEqual(changed.count("classic_lobe_power("), 4)
        data = copy.deepcopy(self.data)
        self.install_source(data, changed)
        with self.assertRaises(ValueError):
            validate_web_render_quality(data, ARTIFACT)

    def test_archived_actual_web09_failed_observation_is_rejected(self):
        if not ARCHIVED_WEB09.is_file():
            self.skipTest('Actual archived Web09 v2 evidence is absent in this clean checkout.')
        raw = ARCHIVED_WEB09.read_bytes()
        archived = json.loads(raw.decode('utf-8'))
        self.assertEqual(archived['status'], 'FAILED')
        self.assertEqual(archived['ground_draws'][0]['uniforms']['u_lookHemiGround'], [0] * 4)
        with self.assertRaises(ValueError):
            validate_web_render_quality(archived, archived['artifact_sha256'])


if __name__ == '__main__':
    unittest.main()