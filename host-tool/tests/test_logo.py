"""Packet building and JPEG checks of the `logo` command (no hardware)."""
import struct
import unittest

import akp05_tool as T


def make_jpeg(w, h, progressive=False, body=b"\x00" * 10):
    sof = b"\xff" + (b"\xc2" if progressive else b"\xc0") + struct.pack(">HBHHB", 11, 8, h, w, 1) + b"\x01\x11\x00"
    return b"\xff\xd8" + sof + body + b"\xff\xd9"


class LogoTest(unittest.TestCase):
    def test_jpeg_info(self):
        self.assertEqual(T.jpeg_info(make_jpeg(800, 480)), (800, 480, False))
        self.assertEqual(T.jpeg_info(make_jpeg(100, 50, progressive=True)), (100, 50, True))

    def test_jpeg_rejects(self):
        with self.assertRaises(ValueError):
            T.jpeg_info(b"\x89PNG....")
        with self.assertRaises(ValueError):
            T.jpeg_info(make_jpeg(800, 480)[:-2])

    def test_packets(self):
        data = make_jpeg(800, 480, body=b"\x55" * 2500)
        pk = T.logo_packets(data)
        self.assertEqual(len(pk), 1 + (len(data) + 1023) // 1024)
        self.assertTrue(all(len(p) == 1024 for p in pk))
        self.assertEqual(pk[0][:8], b"CRT\x00\x00LOG")
        self.assertEqual(struct.unpack(">I", pk[0][8:12])[0], len(data))
        self.assertEqual(b"".join(pk[1:])[:len(data)], data)


if __name__ == "__main__":
    unittest.main()
