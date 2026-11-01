import struct
import tempfile
import unittest
from pathlib import Path

from ps3_shader_cache import (
    ENTRY_STRUCT,
    HEADER_STRUCT,
    MAGIC,
    VERSION,
    build_cache,
    parse_shader_definitions,
)


class ShaderCacheParserTests(unittest.TestCase):
    def test_parses_multiple_materials_with_comments_and_nested_braces(self):
        source = b'''
            // first material
            textures/test/stone
            {
                map textures/test/stone.tga
                { map $lightmap }
                tcMod scroll 0.1 0.2
            }
            /* separator */
            "textures/test/a quoted name"
            { map "textures/test/a{b}.tga" }
        '''
        parsed = parse_shader_definitions(source, "test.shader")
        self.assertEqual(
            [name for name, _ in parsed],
            ["textures/test/stone", "textures/test/a quoted name"],
        )
        self.assertIn(b"map textures/test/stone.tga", parsed[0][1])
        self.assertTrue(parsed[0][1].endswith(b"}\0"))

    def test_rejects_unterminated_material(self):
        with self.assertRaises(ValueError):
            parse_shader_definitions(b"textures/bad { map image\n", "bad.shader")

    def test_cache_is_deterministic_and_keeps_file_manifest(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "z.shader").write_text("textures/duplicate { map z.tga }\n", encoding="utf-8")
            (root / "a.shader").write_text("textures/duplicate { map a.tga }\n", encoding="utf-8")

            cache1, stats = build_cache(root)
            cache2, _ = build_cache(root)
            self.assertEqual(cache1, cache2)
            self.assertEqual(stats["source_files"], 2)
            self.assertEqual(stats["definitions"], 2)

            magic, version, entry_count, source_count, text_size, src_size, entry_size = (
                HEADER_STRUCT.unpack_from(cache1, 0)
            )
            self.assertEqual(magic, MAGIC)
            self.assertEqual(version, VERSION)
            self.assertEqual(entry_count, 2)
            self.assertEqual(source_count, 2)
            self.assertEqual(src_size, 64)
            self.assertEqual(entry_size, ENTRY_STRUCT.size)

            manifest_offset = HEADER_STRUCT.size
            names = [
                cache1[manifest_offset + i * src_size:
                       manifest_offset + (i + 1) * src_size].split(b"\0", 1)[0]
                for i in range(source_count)
            ]
            self.assertEqual(names, [b"a.shader", b"z.shader"])

            entries_offset = manifest_offset + source_count * src_size
            first = ENTRY_STRUCT.unpack_from(cache1, entries_offset)
            second = ENTRY_STRUCT.unpack_from(cache1, entries_offset + entry_size)
            payload_offset = entries_offset + entry_count * entry_size
            first_text = cache1[payload_offset + first[2]:payload_offset + first[2] + first[3]]
            second_text = cache1[payload_offset + second[2]:payload_offset + second[2] + second[3]]
            # The index is emitted in reverse file order so prepend-based hash
            # linking preserves the legacy earlier-file-wins precedence.
            self.assertIn(b"map z.tga", first_text)
            self.assertIn(b"map a.tga", second_text)


if __name__ == "__main__":
    unittest.main()
