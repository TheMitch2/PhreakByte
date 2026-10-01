import os, sys, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from canary_cmd import canary_cmd_name


class CanaryCmd(unittest.TestCase):
    def test_known(self):
        self.assertEqual(canary_cmd_name('engage', 0x50), 'HLTA')
        self.assertEqual(canary_cmd_name('poll', 0x26), 'REQA')
        self.assertEqual(canary_cmd_name('select', 0x93), 'SEL CL1')
        self.assertEqual(canary_cmd_name('engage', 0xE0), 'RATS')

    def test_field_has_no_cmd(self):
        self.assertEqual(canary_cmd_name('field', 0x00), '-')

    def test_iso_dep_blocks(self):
        self.assertIn('I-block', canary_cmd_name('engage', 0x02))
        self.assertIn('I-block', canary_cmd_name('engage', 0x03))
        self.assertIn('R(ACK)', canary_cmd_name('engage', 0xA3))
        self.assertIn('DESELECT', canary_cmd_name('engage', 0xC2))
        self.assertIn('WTX', canary_cmd_name('engage', 0xF2))

    def test_unknown(self):
        self.assertIn('unknown', canary_cmd_name('engage', 0x6A))


if __name__ == '__main__':
    unittest.main()
