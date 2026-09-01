"""用人手施加的位移/速度验证力反馈方向与接触行为，不访问硬件。"""
import math
import contextlib
import io
from unittest.mock import patch
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sdk/neofsa/example"))
from force_demo import command, interaction_state, interaction_torque, pd_target, configure_pd, check_load_torque, main

from adapter_names import adapter_id, adapter_label


class HumanInteractionTest(unittest.TestCase):
    def torque(self, kind, q, v=0, q0=0):
        return interaction_torque(kind, q, v, q0, kp=2, kd=0.2)

    def test_hand_guiding_resists_motion_without_returning_home(self):
        for q in [-0.4, 0, 0.4]:
            self.assertEqual(self.torque("hand_guiding", q), 0)
            for v in [-2, -0.1, 0, 0.1, 2]:
                self.assertLessEqual(self.torque("hand_guiding", q, v) * v, 0)
        self.assertAlmostEqual(self.torque("hand_guiding", 0.1, 1), -0.2)
        self.assertAlmostEqual(self.torque("hand_guiding", 0.1, -1), 0.2)

    def test_spring_returns_towards_captured_start_position(self):
        self.assertAlmostEqual(self.torque("impedance", 1.1, q0=1), -0.2)
        self.assertAlmostEqual(self.torque("impedance", 0.9, q0=1), 0.2)
        self.assertEqual(self.torque("impedance", 1, q0=1), 0)
        self.assertAlmostEqual(self.torque("impedance", 1.1, 0.5, 1), -0.3)

    def test_wall_is_free_inside_and_pushes_back_on_both_sides(self):
        for q in [-0.2, -0.1, 0, 0.1, 0.2]:
            for v in [-1, 0, 1]:
                self.assertEqual(self.torque("wall", q, v), 0)
        self.assertAlmostEqual(self.torque("wall", 0.3), -0.2)
        self.assertAlmostEqual(self.torque("wall", -0.3), 0.2)
        self.assertAlmostEqual(self.torque("wall", 1.3, q0=1), -0.2)

    def test_wall_damping_fades_in_continuously_and_never_attracts(self):
        self.assertLess(abs(self.torque("wall", 0.2 + 1e-9, 1)), 1e-7)
        self.assertLess(abs(self.torque("wall", -0.2 - 1e-9, -1)), 1e-7)
        for q in [-0.4, -0.21, 0.21, 0.4]:
            for v in [-2, -0.1, 0, 0.1, 2]:
                self.assertLessEqual(self.torque("wall", q, v) * q, 0)
        self.assertEqual(self.torque("wall", 0.21, -2), 0)
        self.assertEqual(self.torque("wall", -0.21, 2), 0)

    def test_human_push_and_release_sequence(self):
        # 推到右侧、松手回行、经过自由区，再推到左侧。
        samples = [(0, 0), (0.1, 0.2), (0.25, 0.1), (0.25, 0), (0.21, -0.3), (0.1, -0.2), (-0.25, -0.1)]
        torques = [self.torque("wall", q, v) for q, v in samples]
        self.assertEqual(torques[:2], [0, 0])
        self.assertLess(torques[2], torques[3])
        self.assertLess(torques[3], 0)
        self.assertEqual(torques[4:6], [0, 0])
        self.assertGreater(torques[6], 0)
        self.assertEqual(interaction_state("wall", 0.25, 0, 0), "正向墙接触")
        self.assertEqual(interaction_state("wall", -0.25, 0, 0), "反向墙接触")
        self.assertEqual(interaction_state("wall", 0, 0, 0), "自由区")

    def test_all_interaction_outputs_are_limited_and_start_end_at_zero(self):
        for kind in ["hand_guiding", "impedance", "wall"]:
            for t in [0, 0.5, 2, 4.5, 5]:
                value = command(kind, t, 5, 1, 0, 1, 20, 5, 0.4, 2, 0, 0.3)
                self.assertTrue(math.isfinite(value))
                self.assertLessEqual(abs(value), 0.3)
                if t in [0, 5]:
                    self.assertEqual(value, 0)
        with self.assertRaises(ValueError):
            self.torque("typo", 0)

    def test_pd_reference_uses_native_spring_and_damper(self):
        p, v, torque = pd_target("impedance", 2, 5, 1, 0, 1, 2, .2, .1, .3, 0, 1)
        self.assertEqual(p, 0)
        self.assertEqual(v, 0)
        self.assertAlmostEqual(torque, -.26)
        p, v, torque = pd_target("hand_guiding", 2, 5, 1, 0, 1, 0, .2, .1, .3, 0, 1)
        self.assertEqual(p, .1)
        self.assertEqual(v, 0)
        self.assertAlmostEqual(torque, -.06)

    def test_native_pd_reference_matches_interaction_law(self):
        for kind in ["hand_guiding", "impedance", "wall"]:
            kp = 0 if kind == "hand_guiding" else 2
            for q in [-.4, -.21, -.1, 0, .1, .21, .4]:
                for v in [-1, 0, 1]:
                    for t in [0, .5, 2, 4.5, 5]:
                        p_ref, v_ref, estimate = pd_target(kind, t, 5, 1, 0, 1, kp, .2, q, v, 0, .3)
                        self.assertAlmostEqual(kp*(p_ref-q) + .2*(v_ref-v), estimate)
                        expected = command(kind, t, 5, 1, 0, 1, kp, .2, q, v, 0, .3)
                        self.assertAlmostEqual(estimate, expected)
                        self.assertLessEqual(abs(estimate), .3)

    def test_pd_gain_setup_requires_acknowledged_matching_request(self):
        class FakeNeo:
            def __init__(self, state=2, mismatch=False):
                self.calls = []
                self.state, self.mismatch = state, mismatch
            def call(self, name, value):
                self.calls.append(name)
                if name == "write_pd_param":
                    self.requested = (value.kp, value.kd)
                    value.sequence = 7
                else:
                    value.sequence = 8 if self.mismatch else 7
                    value.state = self.state
                    value.sent_kp, value.sent_kd = .5, .04998779296875
        neo = FakeNeo()
        self.assertEqual(configure_pd(neo, "npcap", .5, .05, .001), (.5, .04998779296875))
        self.assertEqual(neo.calls, ["write_pd_param", "read_pd_param"])
        for neo in [FakeNeo(3), FakeNeo(mismatch=True)]:
            with self.assertRaises(RuntimeError):
                configure_pd(neo, "npcap", .5, .05, .001)
        neo = FakeNeo()
        self.assertEqual(configure_pd(neo, "sim", .5, .05, .001), (.5, .05))
        self.assertEqual(neo.calls, [])

    def test_sensor_limit_reports_signed_value_and_keeps_threshold(self):
        check_load_torque(1, 1, initial=True)
        check_load_torque(-1, 1)
        for measured in (1.25, -1.25):
            for initial in (True, False):
                with self.assertRaises(RuntimeError) as error:
                    check_load_torque(measured, 1, initial=initial)
                message = str(error.exception)
                self.assertIn(f"{measured:+g} Nm", message)
                self.assertIn("绝对值=1.25 Nm", message)
                self.assertIn("max_load_torque=1 Nm", message)
                self.assertIn("不使能" if initial else "请求失能", message)

    def test_expected_demo_error_is_printed_without_traceback(self):
        output = io.StringIO()
        with patch("force_demo.run", side_effect=RuntimeError("负载传感器 +1.25 Nm")):
            with contextlib.redirect_stderr(output):
                self.assertEqual(main("hand_guiding"), 1)
        self.assertIn("负载传感器 +1.25 Nm", output.getvalue())
        self.assertNotIn("Traceback", output.getvalue())

    def test_adapter_name_matches_exact_guid_and_preserves_path(self):
        path = r"\Device\NPF_{01234567-89AB-CDEF-0123-456789ABCDEF}"
        key = "01234567-89ab-cdef-0123-456789abcdef"
        names = {key: {"name": "以太网 2", "description": "Intel 网卡", "connected": True}}
        self.assertEqual(adapter_id(path), key)
        self.assertEqual(adapter_label(path, names), "以太网 2 · Intel 网卡 [已连接]")
        names[key]["connected"] = False
        self.assertIn("[未连接]", adapter_label(path, names))
        missing = r"\Device\NPF_{11234567-89AB-CDEF-0123-456789ABCDEF}"
        self.assertEqual(adapter_label(missing, names), "未解析名称 · " + missing)
        self.assertEqual(adapter_label(r"\Device\NPF_Loopback", names), r"未解析名称 · \Device\NPF_Loopback")


if __name__ == "__main__":
    unittest.main()
