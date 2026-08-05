from __future__ import annotations

import json
import struct
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
FIXTURE_PATH = REPO_ROOT / "tools" / "build" / "fixtures" / "controlipc" / "protocol-v1.json"


class ControlIpcProtocolFixtureTests(unittest.TestCase):
    def test_v1_fixture_is_self_consistent_and_bounded(self) -> None:
        fixture = json.loads(FIXTURE_PATH.read_text(encoding="utf-8"))

        self.assertEqual(1, fixture["fixture_schema_version"])
        self.assertEqual("control-ipc", fixture["protocol"])
        self.assertEqual(1, fixture["protocol_major"])
        self.assertEqual(0, fixture["protocol_minor"])
        self.assertEqual("sakura_controlipc_protocol", fixture["producer"])
        self.assertEqual("exact-wire-bytes-for-v1", fixture["compatibility"])
        self.assertGreaterEqual(len(fixture["frames"]), 2)

        for frame in fixture["frames"]:
            payload = bytes.fromhex(frame["payload_hex"])
            body = (
                struct.pack(
                    "<IHHHHQQ",
                    0x50494353,
                    fixture["protocol_major"],
                    fixture["protocol_minor"],
                    frame["kind_value"],
                    frame["flags_value"],
                    frame["request_id"],
                    frame["generation"],
                )
                + payload
            )
            encoded = struct.pack("<I", len(body)) + body

            self.assertEqual(28 + len(payload), len(body), frame["id"])
            self.assertEqual(encoded.hex(), frame["frame_hex"], frame["id"])
            self.assertEqual(28, int.from_bytes(encoded[:4], "little"), frame["id"])
            self.assertLessEqual(len(encoded), 1024 * 1024, frame["id"])

            if "Request" in frame["flags"]:
                self.assertNotIn("Response", frame["flags"], frame["id"])
            if "Terminal" in frame["flags"]:
                self.assertIn("Response", frame["flags"], frame["id"])


if __name__ == "__main__":
    unittest.main()
