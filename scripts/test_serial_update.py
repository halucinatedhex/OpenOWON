#!/usr/bin/env python3
"""Tests for scripts/serial-update.py against a simulated OWON bootloader."""
import importlib.util
import struct
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("su", HERE / "serial-update.py")
su = importlib.util.module_from_spec(spec)
spec.loader.exec_module(su)


def image(size=78212, base=su.APP_BASE, sp=0x2000C000):
    """A plausible app image: vector table linked at `base`, rest filler."""
    img = bytearray((i * 7) & 0xFF for i in range(size))
    struct.pack_into("<II", img, 0, sp, base + 0x400 | 1)
    for i in range(2, 16):
        struct.pack_into("<I", img, 4 * i, 0 if i in (7, 8, 9, 10, 13) else base + 0x500 | 1)
    return bytes(img)


class Clock:
    """Virtual time, so the 10 s and 120 s timeouts run instantly."""
    def __init__(self):
        self.t = 0.0

    def now(self):
        return self.t

    def sleep(self, s):
        self.t += s


class SimBootloader:
    """OWON's bootloader as captured: `start` -> 7, area -> 7, count -> 7,
    each 10,240-byte pack -> 7, 0x14 -> run the app.

    `wakes_at`: virtual time the meter is switched on (it ignores `start`
    before then). `noise`: bytes it emits at power-on (ours sends 00 01 00).
    `drop_ack_at`: a step whose 7 never comes ("area", "count", or pack n).
    """
    def __init__(self, clock, wakes_at=2.0, noise=b"\x00\x01\x00", drop_ack_at=None):
        self.clock, self.wakes_at, self.noise = clock, wakes_at, noise
        self.drop = drop_ack_at
        self.state, self.out, self.buf = "off", b"", b""
        self.area = self.count = None
        self.packs, self.exited, self.starts_seen = [], False, 0

    def _ack(self, step):
        if step != self.drop:
            self.out += b"7"

    def write(self, b):
        b = bytes(b)
        if self.state == "off":
            if self.clock.now() >= self.wakes_at:
                self.state, self.out = "boot", self.out + self.noise
            else:
                return
        if self.state == "boot":
            if b == su.ENTRY:
                self.starts_seen += 1
                self.state = "area"
                self._ack("entry")
            return
        if self.state == "area":
            if b == su.ENTRY:        # a `start` still in flight: answered again
                self._ack("entry")
                return
            assert len(b) == 1, f"area must be one byte, got {b[:8]!r}"
            if b[0] == 0x02:
                raise AssertionError("AREA 2 SENT: this would rewrite calibration")
            self.area, self.state = b[0], "count"
            self._ack("area")
            return
        if self.state == "count":
            assert len(b) == 1
            self.count, self.state = b[0], "data"
            self._ack("count")
            return
        if self.state == "data":
            self.buf += b
            while len(self.buf) >= su.PACK:
                self.packs.append(self.buf[:su.PACK])
                self.buf = self.buf[su.PACK:]
                self._ack(len(self.packs))
            if len(self.packs) == self.count:
                self.state = "exit"
            return
        if self.state == "exit":
            assert b == su.EXIT, f"expected exit 0x14, got {b!r}"
            self.exited = True
            self.state = "app"

    def read(self, n):
        chunk, self.out = self.out[:n], self.out[n:]
        return chunk


class OurFirmwareThenBootloader:
    """Our firmware answering *IDN? and DIAG:RESET; the reset hands over to
    the simulated bootloader, as a chip reset does on the meter."""
    def __init__(self, clock, idn=b"OpenOWON,serial-console,dev\r\n", knows_reset=True):
        self.clock, self.idn, self.sent, self.out = clock, idn, [], b"t=5s heartbeat=5\n"
        self.knows_reset = knows_reset
        self.boot = SimBootloader(clock, wakes_at=float("inf"))

    def write(self, b):
        self.sent.append(bytes(b))
        if self.boot.wakes_at != float("inf"):
            return self.boot.write(b)
        if bytes(b).strip() == b"*IDN?":
            self.out += self.idn
        elif bytes(b).strip() == b"DIAG:RESET" and not self.knows_reset:
            self.out += b"ERR unknown command\r\n"
        elif bytes(b).strip() == b"DIAG:RESET" and su.ours(self.idn):
            self.out += b"OK\r\n"
            self.boot.wakes_at = self.clock.now()

    def read(self, n):
        if self.out:
            chunk, self.out = self.out[:n], self.out[n:]
            return chunk
        return self.boot.read(n)


def run(img, **sim):
    clock = Clock()
    boot = SimBootloader(clock, **sim)
    link = su.Link(boot, clock=clock.now, sleep=clock.sleep, say=lambda *_: None)
    link.enter()
    link.install(img, su.packs_of(img))
    return boot


class ResetIntoBootloader(unittest.TestCase):
    def test_reset_then_install_with_no_power_cycle(self):
        clock = Clock()
        m = OurFirmwareThenBootloader(clock)
        link = su.Link(m, clock=clock.now, sleep=clock.sleep, say=lambda *_: None)
        self.assertTrue(link.request_reset())
        link.enter(prompt=False)
        link.install(image(), su.packs_of(image()))
        self.assertTrue(m.boot.exited)
        self.assertEqual(m.sent[:2], [b"*IDN?\r\n", b"DIAG:RESET\r\n"])

    def test_a_build_still_named_xdm1241_open_is_ours(self):
        clock = Clock()
        m = OurFirmwareThenBootloader(clock, idn=b"OWON,XDM1241,0,XDM1241-OPEN-0.1-dev\r\n")
        link = su.Link(m, clock=clock.now, sleep=clock.sleep, say=lambda *_: None)
        self.assertTrue(link.request_reset())

    def test_an_older_build_without_reset_falls_back_to_the_button(self):
        clock = Clock()
        m = OurFirmwareThenBootloader(clock, knows_reset=False)
        link = su.Link(m, clock=clock.now, sleep=clock.sleep, say=lambda *_: None)
        self.assertFalse(link.request_reset())

    def test_stock_gets_only_idn(self):
        clock = Clock()
        m = OurFirmwareThenBootloader(clock, idn=b"OWON,XDM1241,12345678,V4.3.0,3\r\n")
        link = su.Link(m, clock=clock.now, sleep=clock.sleep, say=lambda *_: None)
        self.assertFalse(link.request_reset())
        self.assertEqual(m.sent, [b"*IDN?\r\n"])


class ImageChecks(unittest.TestCase):
    def test_our_image_shape_is_accepted(self):
        self.assertEqual(su.check_image(image(), su.OUR_LIMIT), 8)

    def test_the_real_builds_if_present(self):
        found = 0
        for env in ("xdm1241", "xdm1241-serial", "xdm1241-panel", "xdm1241-bench"):
            built = HERE.parent / f".pio/build/{env}/firmware.bin"
            if built.exists():
                found += 1
                with self.subTest(env=env):
                    su.check_image(built.read_bytes(), su.OUR_LIMIT)
        if not found:
            self.skipTest("no build (make fw-all)")

    def test_image_linked_at_flash_base_is_refused(self):
        with self.assertRaisesRegex(su.UpdateError, "app slot|outside"):
            su.check_image(image(base=0x08000000), su.OUR_LIMIT)

    def test_not_firmware_is_refused(self):
        with self.assertRaisesRegex(su.UpdateError, "SRAM"):
            su.check_image(b"\xff" * 4096, su.OUR_LIMIT)
        with self.assertRaisesRegex(su.UpdateError, "too small"):
            su.check_image(b"\x00" * 10, su.OUR_LIMIT)

    def test_packs_must_end_before_our_log(self):
        biggest = (su.OUR_LIMIT - su.APP_BASE) // su.PACK * su.PACK   # 18 packs
        self.assertEqual(su.check_image(image(biggest), su.OUR_LIMIT), 18)
        with self.assertRaisesRegex(su.UpdateError, "past 0x08030000"):
            su.check_image(image(biggest + 1), su.OUR_LIMIT)

    def test_stock_restore_stops_before_stock_records(self):
        biggest = (su.STOCK_LIMIT - su.APP_BASE) // su.PACK * su.PACK  # 23 packs
        self.assertEqual(su.check_image(image(biggest), su.STOCK_LIMIT), 23)
        self.assertLessEqual(su.APP_BASE + 23 * su.PACK, 0x0803D800)
        with self.assertRaisesRegex(su.UpdateError, "past 0x0803D800"):
            su.check_image(image(biggest + 1), su.STOCK_LIMIT)

    def test_last_pack_padded_with_erased_flash(self):
        img = image(su.PACK + 5)
        p = su.packs_of(img)
        self.assertEqual(len(p), 2)
        self.assertTrue(all(len(x) == su.PACK for x in p))
        self.assertEqual(p[1][:5], img[su.PACK:])
        self.assertEqual(set(p[1][5:]), {0xFF})


class Protocol(unittest.TestCase):
    def test_one_pack_install_writes_one_padded_pack(self):
        boot = run(image(2552))
        self.assertEqual(boot.count, 1)
        self.assertEqual(len(boot.packs), 1)
        self.assertEqual(set(boot.packs[0][2552:]), {0xFF})

    def test_full_install(self):
        img = image()
        boot = run(img)
        self.assertEqual(boot.area, 0x05)
        self.assertEqual(boot.count, 8)
        self.assertEqual(b"".join(boot.packs)[:len(img)], img)
        self.assertTrue(boot.exited)

    def test_waits_for_the_meter_to_be_switched_on(self):
        boot = run(image(), wakes_at=37.0)
        self.assertTrue(boot.exited)

    def test_power_on_noise_is_not_an_ack(self):
        boot = run(image(), noise=b"\x00\x01\x00\xfc")
        self.assertTrue(boot.exited)

    def test_late_acks_to_start_are_drained_not_taken_for_the_area_ack(self):
        # Two `start`s answered: the second 7 must not satisfy the area step.
        clock = Clock()
        boot = SimBootloader(clock, drop_ack_at="area")
        link = su.Link(boot, clock=clock.now, sleep=clock.sleep, say=lambda *_: None)
        link.enter()
        boot.out += b"7"            # a straggling answer to an earlier `start`
        link.drain()
        with self.assertRaisesRegex(su.UpdateError, "area byte"):
            link.send_area(su.AREA_APP)

    def test_missing_ack_stops_the_transfer(self):
        with self.assertRaisesRegex(su.UpdateError, "pack 3/8"):
            run(image(), drop_ack_at=3)

    def test_never_switched_on_times_out(self):
        clock = Clock()
        boot = SimBootloader(clock, wakes_at=1e9)
        link = su.Link(boot, clock=clock.now, sleep=clock.sleep, say=lambda *_: None)
        with self.assertRaisesRegex(su.UpdateError, "did not answer"):
            link.enter(timeout=5)

    def test_area_2_cannot_be_sent(self):
        clock = Clock()
        boot = SimBootloader(clock)
        link = su.Link(boot, clock=clock.now, sleep=clock.sleep, say=lambda *_: None)
        link.enter()
        for area in (0x02, 0x00, 0x14, 0xFF):
            with self.assertRaisesRegex(su.UpdateError, "refusing area"):
                link.send_area(area)
        self.assertIsNone(boot.area)       # nothing reached the bootloader


class CommandLine(unittest.TestCase):
    def setUp(self):
        import tempfile
        self.dir = Path(tempfile.mkdtemp())
        self.ours = self.dir / "ours.bin"
        self.stock = self.dir / "stock.bin"
        self.ours.write_bytes(image())
        self.stock.write_bytes(image(120000, sp=0x20004B08))

    def test_refuses_without_a_way_back(self):
        self.assertEqual(su.main(["/dev/null", str(self.ours), "--check"]), 2)

    def test_check_passes_with_stock_image(self):
        self.assertEqual(su.main(["/dev/null", str(self.ours), "--stock",
                                  str(self.stock), "--check"]), 0)

    def test_restore_needs_no_second_image(self):
        self.assertEqual(su.main(["/dev/null", str(self.stock), "--restore", "--check"]), 0)

    def test_v2_image_is_not_a_way_back_for_this_meter(self):
        v2 = HERE.parent / "firmware/XDM1041_OS_V2.1.2_20210525.bin"
        if not v2.exists():
            self.skipTest("stock v2.1.2 image not present")
        self.assertEqual(su.main(["/dev/null", str(self.ours), "--stock", str(v2), "--check"]), 2)
        self.assertEqual(su.main(["/dev/null", str(v2), "--restore", "--check"]), 2)

    def test_no_way_back_needs_the_calibration_saved(self):
        root = su.ROOT
        try:
            su.ROOT = self.dir
            self.assertEqual(su.main(["/dev/null", str(self.ours), "--no-way-back", "--check"]), 2)
            cal = self.dir / "backups" / "serial-20260101-000000"
            cal.mkdir(parents=True)
            (cal / "factory.bin").write_bytes(b"\xff" * 0x800)
            self.assertEqual(su.main(["/dev/null", str(self.ours), "--no-way-back", "--check"]), 0)
        finally:
            su.ROOT = root

    def test_stock_gone_without_a_dump_needs_reset(self):
        root = su.ROOT
        try:
            su.ROOT = self.dir                       # no backups/*-flash here
            self.assertEqual(su.main(["/dev/null", str(self.ours), "--stock-gone", "--check"]), 2)
            self.assertEqual(su.main(["/dev/null", str(self.ours), "--stock-gone", "--reset",
                                      "--check"]), 0)
        finally:
            su.ROOT = root

    def test_dumper_mode_needs_no_stock_but_only_takes_the_dumper(self):
        dumper = self.dir / "dumper.bin"
        img = bytearray(image(2552))
        img[600:600 + len(su.DUMPER_BANNER)] = su.DUMPER_BANNER
        dumper.write_bytes(bytes(img))
        self.assertEqual(su.main(["/dev/null", str(dumper), "--dumper", "--check"]), 0)
        # anything else, or anything bigger than one pack, is refused
        self.assertEqual(su.main(["/dev/null", str(self.ours), "--dumper", "--check"]), 2)
        big = bytearray(image(su.PACK + 1))
        big[600:600 + len(su.DUMPER_BANNER)] = su.DUMPER_BANNER
        dumper.write_bytes(bytes(big))
        self.assertEqual(su.main(["/dev/null", str(dumper), "--dumper", "--check"]), 2)

    def test_the_real_dumper_build_if_present(self):
        built = HERE.parent / ".pio/build/xdm1241-dumper/firmware.bin"
        if not built.exists():
            self.skipTest("no dumper build")
        self.assertEqual(su.main(["/dev/null", str(built), "--dumper", "--check"]), 0)

    def test_stock_gone_needs_a_readout_showing_it_erased(self):
        flash = bytearray(b"\xff" * 0x40000)
        (self.dir / "backups/x-flash").mkdir(parents=True)
        f = self.dir / "backups/x-flash/flash.bin"
        flash[0x9000] = 0x12                       # stock code still there
        f.write_bytes(bytes(flash))
        self.assertIsNone(su.stock_gone_evidence(self.dir))
        flash[0x9000] = 0xFF                       # all erased
        f.write_bytes(bytes(flash))
        self.assertEqual(su.stock_gone_evidence(self.dir), f)
        saved, su.ROOT = su.ROOT, self.dir
        try:
            self.assertEqual(su.main(["/dev/null", str(self.ours), "--stock-gone", "--check"]), 0)
            f.unlink()
            self.assertEqual(su.main(["/dev/null", str(self.ours), "--stock-gone", "--check"]), 2)
        finally:
            su.ROOT = saved

    def test_confirm_must_be_the_exact_phrase(self):
        self.assertEqual(su.main(["/dev/null", str(self.ours), "--stock", str(self.stock),
                                  "--confirm", "yes"]), 1)

    def test_bad_stock_image_is_refused(self):
        self.stock.write_bytes(b"\xff" * 50000)
        self.assertEqual(su.main(["/dev/null", str(self.ours), "--stock",
                                  str(self.stock), "--check"]), 2)


if __name__ == "__main__":
    unittest.main(verbosity=1)
