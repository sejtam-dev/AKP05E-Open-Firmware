"""The flash command refuses partitions whose offsets differ from the stock layout (wrong updater table)."""
import unittest

import akp05_tool as T
from upg_proto import parse_mtd

STOCK = "spi0.0:512k(spl),128k(env),128k(env_r),2m(os),1m(rodata),10m(data)"
VENDOR_DEFAULT_IMG = "spi0.0:512k(spl),128k(env),128k(env_r),2m(os),5m(rodata),5m(data)"
SHORT_SPL = "spi0.0:256k(spl),128k(env),128k(env_r),2m(os),1m(rodata),10m(data)"


def plan(*names):
    return [{"parts": [n]} for n in names]


class LayoutTest(unittest.TestCase):
    def test_stock_ok(self):
        T.check_layout(parse_mtd(STOCK), plan("os", "rodata", "data", "env", "env_r"))

    def test_vendor_default_img_allows_only_os_and_env(self):
        parts = parse_mtd(VENDOR_DEFAULT_IMG)
        T.check_layout(parts, plan("os", "env", "env_r"))
        with self.assertRaises(SystemExit):
            T.check_layout(parts, plan("data"))

    def test_short_spl_table_is_refused(self):
        with self.assertRaises(SystemExit):
            T.check_layout(parse_mtd(SHORT_SPL), plan("os"))

    def test_unknown_partition(self):
        with self.assertRaises(SystemExit):
            T.check_layout(parse_mtd(STOCK), plan("nothere"))


if __name__ == "__main__":
    unittest.main()
