"""CLI reporting controls using synthetic bytes, without game images."""
import contextlib
import io
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import sigscan


def image(path, data):
    header=bytearray(512)
    header[:2]=b'MZ'
    struct.pack_into('<I', header, 0x3c, 0x80)
    header[0x80:0x84]=b'PE\0\0'
    struct.pack_into('<HH', header, 0x84, 0x8664, 1)
    struct.pack_into('<H', header, 0x94, 0xf0)
    struct.pack_into('<H', header, 0x98, 0x20b)
    section=0x188
    header[section:section+5]=b'.text'
    struct.pack_into('<IIII', header, section+8, len(data), 0x1000, len(data), 512)
    struct.pack_into('<I', header, section+36, 0x60000020)
    path.write_bytes(header+data)


class SigscanCliTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        self.source=self.root/'signatures.c'
        self.source.write_text('BACKEND_ENGINE_SIGNATURES[] = {\n'
                               '{ "Example", "11 22 33", 0x1000u },\n};\n', encoding='utf-8')
        self.first=self.root/'one.exe'
        image(self.first, b'\x11\x22\x33\x00')

    def run_cli(self, args):
        stdout,stderr=io.StringIO(),io.StringIO()
        with contextlib.redirect_stdout(stdout),contextlib.redirect_stderr(stderr):
            code=sigscan.main([str(self.source), *map(str,args)])
        return code,stdout.getvalue(),stderr.getvalue()

    def test_default_works_from_unrelated_directory_without_writes(self):
        previous=os.getcwd()
        try:
            os.chdir(self.root)
            before=set(self.root.iterdir())
            code,out,error=self.run_cli([self.first])
            self.assertEqual((code,error),(0,''))
            self.assertIn('1/1 unique',out)
            self.assertEqual(set(self.root.iterdir()),before)
        finally:
            os.chdir(previous)

    def test_explicit_report_retains_both_images_and_missing_match(self):
        second=self.root/'two.exe'
        image(second,b'\x00\x00\x00\x00')
        report=self.root/'report.json'
        code,out,error=self.run_cli([self.first,second,'--output',report])
        self.assertEqual((code,error),(1,''))
        result=json.loads(report.read_text())
        self.assertEqual(result[str(self.first)][0]['hits'],['0x1000'])
        self.assertEqual(result[str(second)][0]['status'],'NOT_FOUND')
        self.assertIn('NOT_FOUND',out)

    def test_ambiguous_match_returns_failure(self):
        image(self.first,b'\x11\x22\x33\x11\x22\x33')
        code,out,error=self.run_cli([self.first])
        self.assertEqual((code,error),(1,''))
        self.assertIn('AMBIGUOUS',out)

    def test_output_errors_precede_scan_and_preserve_existing_report(self):
        report=self.root/'existing.json'
        report.write_bytes(b'previous evidence')
        with patch.object(sigscan,'scan') as scan:
            for target in (report,self.root/'missing'/'report.json'):
                code,_,error=self.run_cli([self.first,'--output',target])
                self.assertEqual(code,2)
                self.assertTrue(error)
            scan.assert_not_called()
        self.assertEqual(report.read_bytes(),b'previous evidence')

    def test_empty_table_and_missing_image_do_not_report_success(self):
        self.source.write_text('BACKEND_ENGINE_SIGNATURES[] = {\n};\n',encoding='utf-8')
        code,_,error=self.run_cli([self.first])
        self.assertEqual(code,2)
        self.assertIn('no signatures',error)
        with contextlib.redirect_stderr(io.StringIO()),self.assertRaises(SystemExit) as caught:
            sigscan.main([str(self.source)])
        self.assertEqual(caught.exception.code,2)


if __name__=='__main__':
    unittest.main()
