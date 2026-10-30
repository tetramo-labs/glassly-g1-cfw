"""The shapes suite offline: encoder vectors, the host pipeline, a dry run of every case, the --dump stream."""
import asyncio
from contextlib import redirect_stdout
import io
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest

from cfw import scene
from cfw.scene import (ARC, ASSET_PUT, CIRCLE_FILL, FLUSH, KIND_SHAPE, KIND_TEXT, MSG_MAX, OBJ_ANIM, OBJ_CONTROL, OBJ_HIDE,
                       OBJ_PUT, OBJ_SHOW, RECT, SHAPES, TEXT, G1Scene, CANVAS, parse_records)
from tools import shapes_suite as suite


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
        entry = scene.tween_entry(1, old, scene.shape_slot(CIRCLE_FILL, 3, 0, [20, 10, 10]), 10, 2)
        self.assertEqual(entry, bytes.fromhex('0100 0701 0a02') + struct.pack('<4h', 20, 10, 10, 3))


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
