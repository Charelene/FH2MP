"""Independent byte contracts for Xls primitives; no game schema/assets required.

Literal bytes are the oracle. Using encode() to generate decode() expectations
would let matching endian/length bugs pass the LSP handler round trips.
Wire types: docs/lsp.md; recovered type table in xls_schema.py.
"""
import os
import sys
from types import SimpleNamespace
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xls


def field(name, kind, **kw):
    return SimpleNamespace(name=name, type=kind, size=kw.get('size', 0),
                           elem=kw.get('elem'), nested=kw.get('nested'))


class Schema:
    def sub(self, f):
        return f.nested

    def field(self, descriptor):
        return descriptor


class Golden(unittest.TestCase):
    def contract(self, schema, values, expected):
        sc = Schema()
        raw = bytes.fromhex(expected)
        self.assertEqual(xls.encode(sc, schema, values), raw)
        self.assertEqual(xls.decode(sc, schema, raw), (values, 0))
        for end in range(len(raw)):
            with self.subTest(truncated_at=end), self.assertRaises(ValueError):
                xls.decode(sc, schema, raw[:end])

    def test_numeric_layout_is_big_endian_with_native_widths(self):
        schema = [field('u16', 1), field('s16', 2), field('u32', 4), field('s64', 5),
                  field('u64', 6), field('f32', 7), field('bool', 0x11)]
        values = dict(u16=0x1234, s16=-2, u32=0x12345678, s64=-2,
                      u64=0x0102030405060708, f32=1.5, bool=True)
        self.contract(schema, values, '1234 fffe 12345678 fffffffffffffffe 0102030405060708 3fc00000 01')

    def test_utf8_byte_length_blob_and_guid_layout(self):
        schema = [field('label', 9), field('blob', 0x14), field('guid', 0x12)]
        values = dict(label='é', blob=b'\x00\xff\x01', guid=bytes.fromhex('00112233445566778899aabbccddeeff'))
        self.contract(schema, values, '00000002 c3a9 00000003 00ff01 00112233445566778899aabbccddeeff')

    def test_array_count_and_nested_struct_layout(self):
        schema = [field('items', 0xE, size=4, elem=field('item', 1, size=2)),
                  field('child', 0xC, nested=[field('flag', 0x11), field('label', 9)])]
        values = dict(items=[1, 65535], child=dict(flag=False, label='A'))
        self.contract(schema, values, '00000002 0001 ffff 00 00000001 41')
        with self.assertRaisesRegex(ValueError, 'capacity'):
            xls.encode(Schema(), schema, dict(items=[1, 2, 3], child=values['child']))

    def test_untrusted_lengths_and_extra_bytes_are_not_silently_consumed(self):
        for schema in ([field('text', 9)], [field('blob', 0x14)],
                       [field('items', 0xE, elem=field('item', 4))]):
            with self.assertRaises(ValueError):
                xls.decode(Schema(), schema, b'\xff\xff\xff\xff')
        self.assertEqual(xls.decode(Schema(), [field('n', 4)], b'\x01\x02\x03\x04tail'), ({'n': 0x01020304}, 4))

