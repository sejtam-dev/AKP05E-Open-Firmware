"""Packet building and frame decoding of the hid-talk command (no hardware)."""
import unittest

import akp05_tool as T


class TalkTest(unittest.TestCase):
    def test_commands(self):
        self.assertEqual(T.talk_packet("DIS")[:8], b"CRT\x00\x00DIS")
        self.assertEqual(T.talk_packet("LIG:42")[10], 42)
        self.assertEqual(T.talk_packet("CLE:C")[11], ord("C"))
        self.assertEqual(T.talk_packet("hex:43 52 54"), b"CRT")

    def test_decode_ack(self):
        frame = bytearray(513)
        frame[1:4] = b"ACK"
        frame[10], frame[11] = 0x35, 1            # body byte 9 / 10 -> report bytes 10 / 11
        self.assertEqual(T.describe_input(bytes(frame)), "ACK code 0x35 state 1")
        frame[12], frame[13] = 0x01, 0x2c         # x = 300 (body bytes 11/12)
        frame[14], frame[15] = 0x00, 0x20         # y = 32  (body bytes 13/14)
        self.assertIn("x=300 y=32", T.describe_input(bytes(frame)))

    def test_decode_swipe(self):
        frame = bytearray(513)
        frame[1:4] = b"ACK"
        frame[10], frame[11] = 0x39, 1            # horizontal swipe, state 1
        frame[12] = 1                              # direction: left
        frame[13:15] = (-60 & 0xffff).to_bytes(2, "big")    # dx = -60
        frame[15:17] = (5).to_bytes(2, "big")               # dy = 5
        self.assertEqual(T.describe_input(bytes(frame)), "ACK code 0x39 state 1  swipe left dx=-60 dy=5")
        frame[12] = 0                              # stock frame without direction: plain output
        frame[13:17] = bytes(4)
        self.assertEqual(T.describe_input(bytes(frame)), "ACK code 0x39 state 1")


if __name__ == "__main__":
    unittest.main()
