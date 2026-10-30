"""The shapes and vector suites offline: encoder vectors, the host pipeline, a dry run of every case,
the --dump streams, the svg compiler, paths and rotation, and the vector dump replayed through the host test."""
import asyncio
from contextlib import redirect_stdout
import io
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
from types import SimpleNamespace
import unittest

from cfw import scene
from cfw.scene import (ARC, ASSET_PUT, CIRCLE_FILL, FLUSH, KIND_PATH, KIND_SHAPE, KIND_TEXT, MSG_MAX, OBJ_ANIM, OBJ_CONTROL, OBJ_HIDE,
                       OBJ_PUT, OBJ_SHOW, RECT, SHAPES, TEXT, G1Scene, CANVAS, parse_records, svg_path)
from tools import shapes_suite as suite
from tools import vector_suite

ROOT = Path(__file__).resolve().parents[1]


def le(*values):
    return struct.pack(f'<{len(values)}h', *values)


def options(**overrides):
    base = {'selectors': [], 'dry_run': True, 'dump': None, 'out': None, 'hold_scale': 1.0, 'trace': False,
            'left': None, 'right': None, 'scan_seconds': 1}
    return SimpleNamespace(**{**base, **overrides})


class SelfTestVectors(unittest.TestCase):
    def test_vectors(self):
        checks = suite.self_test_checks()
        self.assertGreaterEqual(len(checks), 40)
        for name, got, want in checks:
            with self.subTest(name):
                self.assertEqual(got, want)


class WireTests(unittest.TestCase):
    def test_record_roundtrip(self):
        records = [scene.record(OBJ_PUT, b'\x01' * 26), scene.record(OBJ_SHOW, b'\x03\x01\x01\x00'), scene.record(FLUSH, bytes(8))]
        message = scene.messages(records)[0]
        self.assertEqual(parse_records(message), [(OBJ_PUT, b'\x01' * 26), (OBJ_SHOW, b'\x03\x01\x01\x00'), (FLUSH, bytes(8))])
        with self.assertRaises(ValueError):
            parse_records(message[:-1])

    def test_shape_record_and_anim_entry(self):
        self.assertEqual(scene.shape_record(ARC, 15, 4, [50, 50, 50, -90, 180]), bytes.fromhex('0c000f04') + struct.pack('<8h', 50, 50, 50, -90, 180, 0, 0, 0))
        self.assertEqual(scene.anim_entry(7, 0x107, 10, 0, [1, 2, 3, 15]), bytes.fromhex('0700 0701 0a00') + struct.pack('<4h', 1, 2, 3, 15))
        with self.assertRaises(ValueError):
            scene.anim_entry(7, 0x107, 10, 0, [1, 2, 3])
        # G1CFW/40: a CSS cubic-bezier rides the entry as 0x80 + four bytes after the values (the host test's `cur_lin`);
        # a linear curve entry and a code-0 entry both encode
        self.assertEqual(scene.anim_entry(6, 0x01, 4, 0, [500], (0, 0, 255, 255)), bytes.fromhex('0600 0100 0480 f401 0000ffff'))
        self.assertEqual(scene.anim_entry(6, 0x01, 4, 0, [500]), bytes.fromhex('0600 0100 0400 f401'))
        self.assertEqual(scene.rotation_entry(9, 1440, 350, 90, 10, curve=scene.CURVES['ease-in']), bytes.fromhex('0900 001c 0a80') + le(1440, 350, 90) + bytes.fromhex('6b00ffff'))
        with self.assertRaises(ValueError):
            scene.anim_entry(6, 0x01, 4, 0, [500], (0, 0, 255))
        self.assertIsNone(scene.easing_curve('linear'))
        self.assertEqual([scene.easing_curve(e) for e in ('ease-in-out', 'ease-out', 'ease-in', 'ease', None, 'bounce')],
                         [(107, 0, 148, 255), (0, 0, 148, 255), (107, 0, 255, 255), (64, 26, 64, 255), (64, 26, 64, 255), (64, 26, 64, 255)])

    def test_utf8_cut_keeps_characters_whole(self):
        text = '日' * 100  # 3 bytes each: 300 bytes
        data = scene.utf8_cut(text)
        self.assertEqual(len(data), 240)
        self.assertEqual(data.decode('utf-8'), '日' * 80)

    def test_tween_entry_rules(self):
        old = scene.shape_slot(CIRCLE_FILL, 15, 0, [10, 10, 10])
        self.assertIsNone(scene.tween_entry(1, old, scene.shape_slot(RECT, 15, 1, [10, 10, 10]), 10, 0))
        # a filled type cannot change its width through a tween
        self.assertIsNone(scene.tween_entry(1, old, scene.shape_slot(CIRCLE_FILL, 15, 3, [10, 10, 10]), 10, 0))
        # a parameter outside the type's mask (p3 of a circle) is not tweenable
        self.assertIsNone(scene.tween_entry(1, old, scene.shape_slot(CIRCLE_FILL, 15, 0, [10, 10, 10, 1]), 10, 0))
        entry = scene.tween_entry(1, old, scene.shape_slot(CIRCLE_FILL, 3, 0, [20, 10, 10]), 10, curve=scene.CURVES['ease-out'])
        self.assertEqual(entry, bytes.fromhex('0100 0701 0a80') + struct.pack('<4h', 20, 10, 10, 3) + bytes.fromhex('000094ff'))


class PipelineTests(unittest.TestCase):
    def elements(self, case_id, step=0):
        shapes_case = next(c for c in suite.SHAPE_CASES if c['id'] == case_id)
        return shapes_case['steps'](CANVAS)[step]['elements']

    def test_bad_point_counts_are_dropped(self):
        processed = scene.process_scene(self.elements('limits-bad-points'), CANVAS)
        self.assertEqual(processed['dropped'], ['bad-tri', 'bad-bez'])
        self.assertTrue(processed['degraded'])
        self.assertEqual([el['id'] for el in processed['elements']], ['suite-border', 'good'])
        good = processed['elements'][1]
        self.assertEqual(good['box'], {'x': 40, 'y': 100, 'w': 561, 'h': 1})
        self.assertEqual(good['points'], [{'x': 40, 'y': 100}, {'x': 600, 'y': 100}])

    def test_duplicate_ids_drop_the_second(self):
        rect = {'type': 'rect', 'id': 'x', 'box': {'x': 0, 'y': 0, 'w': 10, 'h': 10}}
        processed = scene.process_scene([rect, {**rect, 'box': {'x': 20, 'y': 0, 'w': 10, 'h': 10}}, {**rect, 'type': 'text', 'text': 'x'}], CANVAS)
        self.assertEqual(processed['dropped'], ['x'])
        self.assertEqual(len(processed['elements']), 2)
        self.assertTrue(processed['degraded'])

    def test_clipped_line_is_degraded_not_dropped(self):
        processed = scene.process_scene(self.elements('line-clipped'), CANVAS)
        self.assertEqual(processed['dropped'], [])
        self.assertTrue(processed['degraded'])
        line = processed['elements'][1]
        self.assertEqual(line['box'], {'x': 40, 'y': 40, 'w': 600, 'h': 121})
        self.assertEqual(line['points'][1], {'x': 840, 'y': 40})

    def test_anchors_resolve_against_the_canvas(self):
        resolved = scene.resolve_anchors(self.elements('scene-anchored'), CANVAS['width'], CANVAS['height'])
        self.assertEqual(resolved[1]['box'], {'x': 572, 'y': 132, 'w': 60, 'h': 60})
        self.assertEqual(resolved[2]['box'], {'x': 512, 'y': 8, 'w': 120, 'h': 40})
        self.assertEqual(resolved[0], self.elements('scene-anchored')[0])

    def test_images_and_budget(self):
        image = {'type': 'image', 'id': 'img', 'box': {'x': 0, 'y': 0, 'w': 10, 'h': 10}, 'data': 'AAAA'}
        processed = scene.process_scene([image], CANVAS)
        self.assertEqual(processed['dropped'], ['img'])
        processed = scene.process_scene(self.elements('limits-budget'), CANVAS)
        self.assertEqual(processed['dropped'], ['d120', 'd121', 'd122', 'd123'])
        self.assertEqual(len(processed['elements']), 120)

    def test_validation_and_report_ids(self):
        processed = scene.process_scene([None, {'type': 'rect'}, {'type': 'arc', 'box': {'x': 0, 'y': 0, 'w': 10, 'h': 10}},
                                         {'type': 'rect', 'box': {'x': 700, 'y': 0, 'w': 10, 'h': 10}}, {'type': 'rect', 'id': '~~x', 'box': {'x': 0, 'y': 0, 'w': 10, 'h': 10}}], CANVAS)
        self.assertEqual(processed['dropped'], ['text[0]', 'rect[1]', 'arc[2]', 'rect[3]'])
        self.assertEqual(processed['elements'][0]['id'], 'x')
        self.assertIsNone(scene.valid_transition({'durationMs': 0}))
        self.assertIsNone(scene.valid_transition('fast'))
        self.assertEqual(scene.valid_transition({'durationMs': 400, 'easing': 'linear'}), {'durationMs': 400, 'easing': 'linear'})
        self.assertEqual(scene.valid_transition({'durationMs': 400}), {'durationMs': 400})

    def test_content_hash_matches_the_phone(self):
        # FNV-1a over "rect", undefined text/data, "border=1", undefined points and angles, in base 36
        self.assertEqual(scene.content_hash('a'), scene.content_hash('a'))
        self.assertNotEqual(scene.content_hash('a'), scene.content_hash('b'))
        self.assertEqual(scene.content_hash(), 'ztntfp')  # (0x811c9dc5 >>> 0).toString(36)
        self.assertEqual(scene.element_content_hash({'type': 'rect', 'style': {'border': 1, 'fill': True}}),
                         scene.content_hash('rect', None, None, 'border=1,fill=true', None, None))

    def test_diff_changes_and_synthetic_ids(self):
        pipeline = suite.Pipeline()
        rect = {'type': 'rect', 'id': 'r', 'box': {'x': 0, 'y': 0, 'w': 10, 'h': 10}}
        anon = {'type': 'circle', 'box': {'x': 50, 'y': 50, 'w': 10, 'h': 10}}
        _, diffed = pipeline.frame([rect, anon])
        self.assertEqual([(el['id'], el['change']) for el in diffed], [('r', 'created'), ('~1', 'created')])
        _, diffed = pipeline.frame([{**rect, 'box': {'x': 5, 'y': 0, 'w': 10, 'h': 10}}, anon])
        self.assertEqual([(el['id'], el['change']) for el in diffed], [('r', 'moved'), ('~1', 'unchanged')])
        _, diffed = pipeline.frame([{**rect, 'box': {'x': 5, 'y': 0, 'w': 10, 'h': 10}, 'style': {'fill': True}}, {**anon, 'box': {'x': 60, 'y': 50, 'w': 10, 'h': 10}}])
        self.assertEqual([(el['id'], el['change']) for el in diffed], [('r', 'updated'), ('~1', 'moved')])
        result = scene.diff_scene(pipeline.prev, [], pipeline.next_synthetic)
        self.assertEqual(result['removed'], ['r', '~1'])

    def test_evaluate_result(self):
        ok = {'status': 'displayed', 'degraded': False, 'dropped': []}
        self.assertEqual(scene.evaluate_result({'status': 'displayed', 'degraded': False, 'dropped': []}, ok), {'pass': True, 'reason': 'displayed'})
        self.assertEqual(scene.evaluate_result({'status': 'displayed', 'degraded': True, 'dropped': ['a']}, ok)['reason'], 'dropped [a], expected []')
        self.assertEqual(scene.evaluate_result({'status': 'displayed', 'degraded': True, 'dropped': []}, ok)['reason'], 'degraded true, expected false')
        self.assertEqual(scene.evaluate_result({'status': 'blocked', 'reason': 'busy'}, ok)['reason'], 'status blocked (busy), expected displayed')


class EncoderTests(unittest.TestCase):
    def test_right_temple_shifts_by_depth(self):
        left, right = G1Scene(1), G1Scene(-1)
        el = {'id': 'c', 'type': 'circle', 'box': {'x': 100, 'y': 100, 'w': 21, 'h': 21}, 'depth': 4, 'change': 'created'}
        left_put, right_put = left.encode([el])[0], right.encode([el])[0]
        self.assertEqual(struct.unpack_from('<h', left_put, 3 + 6 + 4)[0], 114)
        self.assertEqual(struct.unpack_from('<h', right_put, 3 + 6 + 4)[0], 106)

    def test_hide_mid_tween_forces_a_put(self):
        g1 = G1Scene(1)
        ball = {'id': 'b', 'type': 'circle', 'box': {'x': 0, 'y': 0, 'w': 21, 'h': 21}, 'style': {'fill': True}, 'change': 'created'}
        g1.encode([ball])
        g1.encode([{**ball, 'box': {'x': 100, 'y': 0, 'w': 21, 'h': 21}, 'transition': {'durationMs': 1000}}])
        self.assertEqual(g1.hide(), [scene.record(OBJ_HIDE, b'\x02')])
        records = g1.encode([{**ball, 'box': {'x': 100, 'y': 0, 'w': 21, 'h': 21}}])
        self.assertEqual([r[0] for r in records], [OBJ_PUT, OBJ_SHOW])
        self.assertEqual(struct.unpack_from('<H', records[0], 5)[0], 2)  # version 2

    def test_anim_records_hold_at_most_16_entries(self):
        g1 = G1Scene(1)
        discs = [{'id': f'd{i}', 'type': 'circle', 'box': {'x': i * 30, 'y': 0, 'w': 21, 'h': 21}, 'style': {'fill': True}, 'change': 'created'} for i in range(20)]
        g1.encode(discs)
        records = g1.encode([{**d, 'box': {**d['box'], 'y': 50}, 'transition': {'durationMs': 500}} for d in discs])
        anims = [r for r in records if r[0] == OBJ_ANIM]
        self.assertEqual([r[4] for r in anims], [16, 4])
        self.assertEqual(g1.tweens, 20)

    def test_arena_overflow_resets_the_glasses(self):
        g1 = G1Scene(1)
        g1.arena_capacity = 64
        text = {'id': 't', 'type': 'text', 'box': {'x': 0, 'y': 0, 'w': 600, 'h': 30}, 'text': 'x' * 40, 'change': 'created'}
        other = {'id': 'u', 'type': 'text', 'box': {'x': 0, 'y': 40, 'w': 600, 'h': 30}, 'text': 'y' * 30, 'change': 'created'}
        g1.encode([text])
        records = g1.encode([text, other])
        self.assertEqual([r[0] for r in records[:2]], [OBJ_CONTROL, scene.ASSET_CLEAR])
        self.assertEqual([r[0] for r in records[2:]], [ASSET_PUT, OBJ_PUT, ASSET_PUT, OBJ_PUT, OBJ_SHOW])
        self.assertEqual(g1.resident(), 2)

    def test_text_with_border_puts_the_rect_first(self):
        g1 = G1Scene(1)
        records = g1.encode([{'id': 't', 'type': 'text', 'box': {'x': 10, 'y': 10, 'w': 100, 'h': 30}, 'text': 'hi', 'style': {'border': 2, 'radius': 4}, 'change': 'created'}])
        self.assertEqual([r[0] for r in records], [OBJ_PUT, ASSET_PUT, OBJ_PUT, OBJ_SHOW])
        self.assertEqual(records[0][3 + 4], KIND_SHAPE)
        self.assertEqual(records[0][3 + 6], RECT)
        self.assertEqual(records[2][3 + 4], KIND_TEXT)
        self.assertEqual(g1.slot_ids(), [[1, 2]])


class DryRunTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        dump_path = Path(cls.temp.name) / 'suite.dump'
        out_path = Path(cls.temp.name) / 'results.json'
        cls.output = io.StringIO()
        with redirect_stdout(cls.output):
            cls.results = asyncio.run(suite.run_suite(options(dump=str(dump_path), out=str(out_path)), suite.select_cases([])))
        cls.items = suite.parse_dump(dump_path.read_bytes())
        cls.results_json = out_path.read_text()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_every_case_passes(self):
        self.assertEqual(len(self.results), len(suite.SHAPE_CASES))
        failing = [(r['id'], r['reason']) for r in self.results if r['state'] != 'pass']
        self.assertEqual(failing, [])
        self.assertIn(f'{len(self.results)} passed, 0 failed, 0 errored', self.output.getvalue())
        self.assertIn('"results"', self.results_json)

    def test_messages_fit_and_records_parse(self):
        messages = [payload for tag, payload in self.items if tag == 'message']
        self.assertGreater(len(messages), len(suite.SHAPE_CASES))
        known = {OBJ_PUT, OBJ_SHOW, OBJ_HIDE, OBJ_CONTROL, OBJ_ANIM, ASSET_PUT, scene.ASSET_CLEAR, SHAPES, TEXT, FLUSH}
        for message in messages:
            self.assertLessEqual(len(message), MSG_MAX)
            for cmd, payload in parse_records(message):
                self.assertIn(cmd, known)
                if cmd == OBJ_PUT:
                    self.assertEqual(len(payload), 26 if payload[4] == KIND_SHAPE else 19)
                    self.assertTrue(1 <= struct.unpack_from('<H', payload)[0] <= 96)
                elif cmd == OBJ_SHOW:
                    self.assertEqual(len(payload), 2 + 2 * payload[1])
                    self.assertLessEqual(payload[1], 96)
                elif cmd == OBJ_ANIM:
                    self.assertEqual(payload[0], 33)
                    self.assertLessEqual(payload[1], 16)
                elif cmd == SHAPES:
                    self.assertEqual(len(payload), 2 + 20 * payload[1])
                elif cmd == ASSET_PUT:
                    self.assertEqual(payload[2], 2)
                    self.assertLessEqual(len(payload) - 8, 240)

    def test_dump_structure(self):
        labels = [payload for tag, payload in self.items if tag == 'label']
        self.assertEqual(labels[0], 'setup')
        self.assertEqual(labels[-1], 'teardown')
        self.assertEqual(labels[1:-1], [c['id'] for c in suite.SHAPE_CASES])
        self.assertEqual(self.items[1], ('message', suite.SETUP_MESSAGE))
        self.assertEqual(self.items[-1], ('message', scene.record(OBJ_HIDE, b'\x02')))
        waits = [ms for tag, ms in self.items if tag == 'wait']
        self.assertIn(700, waits)
        self.assertTrue(all(ms > 0 for ms in waits))


class CommandLineTests(unittest.TestCase):
    def test_list_and_selectors(self):
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(suite.main(['--list']), 0)
        self.assertEqual(len(output.getvalue().splitlines()), len(suite.SHAPE_CASES))
        self.assertEqual([c['id'] for c in suite.select_cases(['rect', 'anim-glide'])], ['rect-outline', 'rect-fill', 'rect-gray-ramp', 'anim-glide'])
        self.assertEqual(suite.select_cases(['nothing']), [])

    def test_self_test_passes(self):
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(suite.main(['--self-test']), 0)
        self.assertRegex(output.getvalue(), r'self-test: (\d+)/\1 checks ok')

    def test_dry_run_of_one_case(self):
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(suite.main(['--dry-run', 'anim-glide']), 0)
        self.assertIn('dry run: 1 case', output.getvalue())
        self.assertIn('PASS  anim-glide', output.getvalue())


class SvgPathTests(unittest.TestCase):
    def test_ring_fixture(self):
        want = (bytes([0]) + le(1280, 1280) + bytes([1]) + le(3840, 1280) + bytes([1]) + le(3840, 3840) + bytes([1]) + le(1280, 3840) + bytes([4])
                + bytes([0]) + le(1920, 1920) + bytes([1]) + le(3200, 1920) + bytes([1]) + le(3200, 3200) + bytes([1]) + le(1920, 3200) + bytes([4]))
        self.assertEqual(svg_path('M80 80H240V240H80Z M120 120H200V200H120Z'), want)

    def test_curves_fixture(self):
        want = bytes([0]) + le(1600, 3680) + bytes([3]) + le(1600, 480, 4800, 480, 4800, 3680) + bytes([2]) + le(3200, 5600, 1600, 3680) + bytes([4])
        self.assertEqual(svg_path('M100 230C100 30 300 30 300 230Q200 350 100 230Z'), want)

    def test_relative_implicit_and_reflected(self):
        self.assertEqual(svg_path('m10 10 h20 v20 h-20z'), bytes([0]) + le(160, 160) + bytes([1]) + le(480, 160) + bytes([1]) + le(480, 480) + bytes([1]) + le(160, 480) + bytes([4]))
        self.assertEqual(svg_path('M0 0 10 0 10 10Z'), bytes([0]) + le(0, 0) + bytes([1]) + le(160, 0) + bytes([1]) + le(160, 160) + bytes([4]))
        self.assertEqual(svg_path('M0 0C10 20 30 20 40 0S70 -20 80 0Z'),
                         bytes([0]) + le(0, 0) + bytes([3]) + le(160, 320, 480, 320, 640, 0) + bytes([3]) + le(800, -320, 1120, -320, 1280, 0) + bytes([4]))
        self.assertEqual(svg_path('M0 0Q10 10 20 0T40 0Z'),
                         bytes([0]) + le(0, 0) + bytes([2]) + le(160, 160, 320, 0) + bytes([2]) + le(480, -160, 640, 0) + bytes([4]))
        self.assertEqual(svg_path('M0.03 0.03L1e1 0Z')[1:5], le(0, 0))  # 0.48 rounds to 0

    def test_errors_match_the_compiler(self):
        for d, message in [('M0 0L10 10', 'Expected explicitly closed filled path'), ('L0 0Z', 'Path must start with M'),
                           ('M0 0A1 1 0 0 0 1 1Z', 'Unsupported SVG command: A'), ('M0 0#Z', 'Invalid SVG path syntax'),
                           ('M0 0L10Z', 'Missing SVG coordinate'), ('M0 0 M1 1Z', 'Filled contours must close with Z before another M'),
                           ('M0 0ZZ', 'Z without an open contour'), ('M3000 0Z', 'Q4 coordinate outside -32768..32767: 48000'), ('', 'Expected explicitly closed filled path')]:
            with self.subTest(d):
                with self.assertRaises(ValueError) as caught:
                    svg_path(d)
                self.assertEqual(str(caught.exception), message)

    def test_commands_roundtrip_and_arena_estimate(self):
        commands = svg_path('M100 230C100 30 300 30 300 230Q200 350 100 230Z')
        items = scene.path_commands(commands)
        self.assertEqual([op for op, _ in items], [0, 3, 2, 4])
        self.assertEqual(scene.pack_path_commands(items), commands)
        self.assertEqual(scene.path_arena_bytes(commands), 4 + 16 * (16 + 16 + 1))
        with self.assertRaises(ValueError):
            scene.path_commands(b'\x07')


class PathObjectTests(unittest.TestCase):
    def test_svg_element_is_an_asset_and_a_kind_5_object(self):
        g1 = G1Scene(1)
        el = {'id': 'p', 'type': 'svg', 'box': {'x': 10, 'y': 20, 'w': 100, 'h': 50}, 'path': svg_path('M0 0H60V30H0Z'), 'pathScale': 256, 'fillRule': 1,
              'style': {'color': 9}, 'change': 'created'}
        records = g1.encode([el])
        commands = bytes([0]) + le(0, 0) + bytes([1]) + le(960, 0) + bytes([1]) + le(960, 480) + bytes([1]) + le(0, 480) + bytes([4])
        self.assertEqual(records[0], bytes.fromhex('081d00 0100 03 00 0100 0000') + commands)
        self.assertEqual(records[1], bytes.fromhex('201300 01000100 0501') + struct.pack('<hhHHHBBB', 10, 20, 256, 0, 1, 9, 0, 0))
        self.assertEqual(records[2], bytes.fromhex('210400 0301 0100'))
        # x, y, scale and colour tween; the commands do not
        records = g1.encode([{**el, 'box': {'x': 50, 'y': 20, 'w': 100, 'h': 50}, 'pathScale': 512, 'style': {'color': 15}, 'transition': {'durationMs': 330, 'easing': 'linear'}}])
        self.assertEqual(records, [bytes.fromhex('210400 0301 0100'), bytes.fromhex('251000 2101 0100 0701 0a00') + le(50, 20, 512, 15)])
        records = g1.encode([{**el, 'path': svg_path('M0 0H30V30H0Z'), 'transition': {'durationMs': 330}}])
        self.assertEqual([r[0] for r in records], [ASSET_PUT, OBJ_PUT, OBJ_SHOW])
        self.assertEqual(struct.unpack_from('<H', records[1], 5)[0], 2)

    def test_pipeline_passes_svg_through(self):
        processed = scene.process_scene([{'type': 'svg', 'id': 'p', 'box': {'x': 0, 'y': 0, 'w': 10, 'h': 10}, 'path': b'\x00\x00\x00\x00\x00\x04', 'rotationQ8': 256}], CANVAS)
        self.assertEqual(processed['dropped'], [])
        self.assertEqual(processed['elements'][0]['path'], b'\x00\x00\x00\x00\x00\x04')
        self.assertEqual(processed['elements'][0]['rotationQ8'], 256)


class RotationTests(unittest.TestCase):
    def test_new_rotated_shape_snaps_after_its_put(self):
        g1 = G1Scene(1)
        bar = {'id': 'r', 'type': 'rect', 'box': {'x': 300, 'y': 80, 'w': 100, 'h': 20}, 'style': {'fill': True}, 'rotationQ8': 90 * 256, 'change': 'created'}
        records = g1.encode([bar])
        self.assertEqual([r[0] for r in records], [OBJ_PUT, OBJ_SHOW, OBJ_ANIM])
        # the host test's example: angle 1440 (90 deg) about the centre (350, 90), frames 0 (no curve on a snap)
        self.assertEqual(records[2], bytes.fromhex('250e00 2101 0100 001c 0000') + le(1440, 350, 90))
        self.assertEqual(g1.encode([bar]), [])
        # the rotation glides back with a transition about the pivot the glasses hold; nothing else changes
        records = g1.encode([{**bar, 'rotationQ8': 0, 'transition': {'durationMs': 330, 'easing': 'linear'}}])
        self.assertEqual(records, [bytes.fromhex('210400 0301 0100'), bytes.fromhex('250a00 2101 0100 0004 0a00') + le(0)])
        # a geometry tween and a rotation ride one entry; the new pivot is snapped first while the angle is still 0
        records = g1.encode([{**bar, 'box': {'x': 200, 'y': 80, 'w': 100, 'h': 20}, 'rotationQ8': 45 * 256, 'pivotX': 250, 'pivotY': 90, 'transition': {'durationMs': 330}}])
        self.assertEqual(records, [bytes.fromhex('210400 0301 0100'), bytes.fromhex('250c00 2101 0100 0018 0000') + le(250, 90),
                                   bytes.fromhex('251a00 2101 0100 1f05 0a80') + le(200, 80, 100, 20, 0, 15, 720) + bytes.fromhex('401a40ff')])
        # a border adds a second slot: a new object with its own snap; the first slot is unchanged and keeps its rotation
        records = g1.encode([{**bar, 'box': {'x': 200, 'y': 80, 'w': 100, 'h': 20}, 'style': {'fill': True, 'border': 2}, 'rotationQ8': 45 * 256, 'pivotX': 250, 'pivotY': 90}])
        self.assertEqual([r[0] for r in records], [OBJ_PUT, OBJ_SHOW, OBJ_ANIM])
        self.assertEqual(records[2], bytes.fromhex('250e00 2101 0200 001c 0000') + le(720, 250, 90))
        # a rotated shape that moves with a transition carries its pivot along in the same glide
        records = g1.encode([{**bar, 'box': {'x': 100, 'y': 80, 'w': 100, 'h': 20}, 'style': {'fill': True, 'border': 2}, 'rotationQ8': 45 * 256, 'transition': {'durationMs': 330}}])
        self.assertEqual(records[1][5:5 + 6], bytes.fromhex('0100 1f1d 0a80'))
        self.assertEqual(records[1][-4:], bytes.fromhex('401a40ff'))

    def test_text_never_rotates_and_hide_resnaps(self):
        g1 = G1Scene(1)
        text = {'id': 't', 'type': 'text', 'box': {'x': 0, 'y': 0, 'w': 100, 'h': 30}, 'text': 'hi', 'rotationQ8': 90 * 256, 'change': 'created'}
        self.assertEqual([r[0] for r in g1.encode([text])], [ASSET_PUT, OBJ_PUT, OBJ_SHOW])
        disc = {'id': 'd', 'type': 'circle', 'box': {'x': 0, 'y': 0, 'w': 21, 'h': 21}, 'style': {'fill': True}, 'rotationQ8': 30 * 256, 'change': 'created'}
        g1.encode([disc])
        g1.encode([{**disc, 'box': {'x': 100, 'y': 0, 'w': 21, 'h': 21}, 'transition': {'durationMs': 2000}}])
        self.assertEqual(g1.hide(), [scene.record(OBJ_HIDE, b'\x02')])
        records = g1.encode([{**disc, 'box': {'x': 100, 'y': 0, 'w': 21, 'h': 21}}])
        self.assertEqual([r[0] for r in records], [OBJ_PUT, OBJ_SHOW, OBJ_ANIM])
        self.assertEqual(records[2], bytes.fromhex('250e00 2101 0100 001c 0000') + le(480, 110, 10))


class VectorSuiteTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.steps = vector_suite.vector_cases()
        cls.encoded = vector_suite.encode_steps(cls.steps)
        cls.by_name = {step['name']: step for step in cls.encoded}

    def records(self, name):
        return parse_records(self.by_name[name]['message'])

    def test_steps_and_scaling(self):
        self.assertEqual(len(self.encoded), 24)
        self.assertEqual(self.encoded[0]['name'], 'reset')
        self.assertEqual(self.by_name['reset']['message'], b''.join(scene.RESET_RECORDS) + scene.record(OBJ_HIDE, b'\x02'))
        self.assertEqual(self.by_name['fill-evenodd-hole']['probes'], [(38, 38, 15), (67, 67, 0)])
        for step in self.encoded:
            self.assertLessEqual(len(step['message']), MSG_MAX)
            for x, y, _ in step['probes']:
                self.assertTrue(0 <= x < 640 and 0 <= y < 200, (step['name'], x, y))

    def test_path_definition(self):
        records = self.records('fill-evenodd-hole')
        self.assertEqual([cmd for cmd, _ in records], [ASSET_PUT, OBJ_PUT, OBJ_SHOW])
        asset = records[0][1]
        self.assertEqual(asset[:8], struct.pack('<HBBHH', 1, 3, 0, 1, 0))
        self.assertEqual(scene.path_commands(asset[8:])[0], (0, [533, 533]))      # 80 px * 16 * 200/480
        self.assertEqual(records[1][1], struct.pack('<HHBB', 1, 1, KIND_PATH, 1) + struct.pack('<hhHHHBBB', 0, 0, 256, 0, 1, 15, 0, 0))
        self.assertEqual(records[2][1], bytes.fromhex('0301 0100'))
        # the same commands with the other rule are a new asset and version
        records = self.records('fill-nonzero-same-winding')
        self.assertEqual(records[0][1][:8], struct.pack('<HBBHH', 1, 3, 0, 0, 0))
        self.assertEqual(struct.unpack_from('<H', records[1][1], 2)[0], 2)

    def test_ops_merge_into_one_entry_per_object(self):
        # the first rotation snaps the pivot in its own record, then glides the angle alone
        linear = bytes.fromhex('0000ffff')   # the fixtures' curves ride verbatim behind easing 0x80
        records = self.records('rotation-quarter-turn')
        self.assertEqual(records, [(OBJ_ANIM, bytes.fromhex('2101 0100 0018 0000') + le(58, 50)), (OBJ_ANIM, bytes.fromhex('2101 0100 0004 1580') + le(1440) + linear)])
        self.assertEqual(self.records('rotation-full-turn'), [(OBJ_ANIM, bytes.fromhex('2101 0100 0004 3780') + le(7200) + linear)])
        records = self.records('rotation-retarget-and-move')
        self.assertEqual(records, [(OBJ_ANIM, bytes.fromhex('2101 0100 0304 2880') + le(133, 83, 0) + linear)])
        self.assertIn('the longer wins', self.by_name['rotation-retarget-and-move']['notes'][0])
        records = self.records('path-move-scale-color-rotate')
        self.assertEqual(records, [(OBJ_ANIM, bytes.fromhex('2101 0100 0018 0000') + le(125, 75)),
                                   (OBJ_ANIM, bytes.fromhex('2101 0100 0705 1e80') + le(125, 75, 512, 9, 1440) + linear)])
        # finish snaps everything this port tweened on the slot since its PUT: the glide's x,y and the rotation
        self.assertEqual(self.records('rotation-finish'), [(OBJ_ANIM, bytes.fromhex('2101 0100 031c 0000') + le(133, 83, 0, 150, 92))])
        self.assertEqual(self.by_name['rotation-freeze']['message'], b'')
        self.assertEqual(self.by_name['all-geometric-shapes-rotate-finish']['message'], b'')

    def test_shapes_and_reopen(self):
        records = self.records('all-geometric-shapes-initial')
        self.assertEqual([cmd for cmd, _ in records], [OBJ_PUT] * 13 + [OBJ_SHOW])
        self.assertEqual(records[0][1][:6], struct.pack('<HHBB', 21, 1, 4, 1))
        self.assertEqual(records[0][1][6:10], bytes([1, 0, 15, 3]))
        self.assertEqual(records[-1][1], bytes([3, 13]) + b''.join(struct.pack('<H', i) for i in range(21, 34)))
        # 13 pivot snaps in one record, then 13 angle glides in another (each under the 16-entry limit)
        anim = self.records('all-geometric-shapes-rotate-halfway')
        self.assertEqual([cmd for cmd, _ in anim], [OBJ_ANIM, OBJ_ANIM])
        self.assertEqual((anim[0][1][1], anim[1][1][1]), (13, 13))
        self.assertEqual(anim[0][1][2:8], bytes.fromhex('1500 0018 0000'))
        self.assertEqual(anim[1][1][2:8], bytes.fromhex('1500 0004 4c80'))
        self.assertEqual(struct.unpack_from('<h', anim[1][1], 8)[0], 5760)
        self.assertEqual(anim[1][1][10:14], bytes.fromhex('0000ffff'))
        self.assertEqual(self.records('path-reopen-from-cache'), [(OBJ_SHOW, bytes.fromhex('0301 0100'))])
        self.assertEqual(self.records('path-hide'), [(OBJ_HIDE, b'\x02')])

    def test_dump_roundtrip(self):
        data = vector_suite.dump_bytes(self.encoded)
        tags = []
        pos = 0
        while pos < len(data):
            tag = data[pos]
            tags.append(tag)
            if tag == 1:
                pos += 3 + struct.unpack_from('<H', data, pos + 1)[0]
            elif tag == 2:
                pos += 5
            elif tag == 3:
                pos += 2 + data[pos + 1]
            elif tag == 4:
                pos += 6
            elif tag == 5:
                pos += 1
            else:
                self.fail(f'bad tag {tag} at {pos}')
        self.assertEqual(tags.count(3), 24)
        self.assertEqual(tags.count(1), 22)
        self.assertEqual(tags.count(2), 24)
        self.assertEqual(tags.count(5), 22)
        self.assertEqual(tags.count(4), sum(len(step['probes']) for step in self.encoded))


@unittest.skipUnless(shutil.which('cc'), 'no host C compiler')
class HostReplayTests(unittest.TestCase):
    """Both suites' dumps through the firmware's host build: every message accepted, every probe right."""

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.temp.name) / 'g1cfw_host_test'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-fsanitize=address,undefined',
                        '-O1', '-g', str(ROOT / 'tests/host/g1cfw_host_test.c'), '-o', str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def replay(self, dump):
        result = subprocess.run([str(self.exe), '--replay', str(dump)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_vector_dump_replays_without_errors(self):
        dump = Path(self.temp.name) / 'vector.dump'
        dump.write_bytes(vector_suite.dump_bytes(vector_suite.encode_steps(vector_suite.vector_cases())))
        self.assertIn('replay: 22 messages, 46 snapshots, 0 errors', self.replay(dump))

    def test_shapes_dump_replays_without_errors(self):
        dump = Path(self.temp.name) / 'shapes.dump'
        with redirect_stdout(io.StringIO()):
            results = asyncio.run(suite.run_suite(options(dump=str(dump)), suite.select_cases([])))
        self.assertTrue(all(r['state'] == 'pass' for r in results))
        self.assertRegex(self.replay(dump), r'replay: \d+ messages, \d+ snapshots, 0 errors')
