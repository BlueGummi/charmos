import re
import unittest

from charm.nightmare import grammar as g
from charm.paths import repo_root


class KernelAgreementTests(unittest.TestCase):
    def test_length_caps_match_the_kernel(self) -> None:
        text = (repo_root() / "kernel/cmdline/internal.h").read_text()
        found = dict(
            re.findall(r"^#define\s+(MAX_VAR_LEN|MAX_VAL_LEN)\s+(\d+)", text, re.M)
        )

        self.assertEqual(int(found["MAX_VAR_LEN"]), g.MAX_VAR_LEN)
        self.assertEqual(int(found["MAX_VAL_LEN"]), g.MAX_VAL_LEN)

    def test_every_unit_we_emit_is_one_the_kernel_accepts(self) -> None:
        text = (repo_root() / "kernel/parse.c").read_text()
        body = text[text.index("parse_duration_internal") :]
        body = body[: body.index("\n}\n")]
        accepted = set(re.findall(r'strncasecmp\(unit_start,\s*"(\w+)"', body))

        self.assertTrue(accepted, "could not read units out of parse.c")
        for unit in g.DURATION_UNITS:
            self.assertIn(unit, accepted, f"the kernel does not accept {unit!r}")

    def test_the_list_separator_is_still_a_comma(self) -> None:
        text = (repo_root() / "kernel/parse.c").read_text()
        body = text[text.index("static bool parse_list_internal") :][:1200]

        self.assertIn("*p == ','", body)


class RenderingTests(unittest.TestCase):
    def test_a_duration_always_carries_its_unit(self) -> None:
        self.assertEqual(g.duration_ms(300000), "300000ms")
        self.assertEqual(g.duration_us(500), "500us")

    def test_a_seed_is_hex_so_strtoull_cannot_read_it_as_octal(self) -> None:
        self.assertEqual(g.hex_u64(0o17), "0x000000000000000f")
        self.assertTrue(g.hex_u64(1).startswith("0x"))

    def test_fx_renders_a_plain_decimal(self) -> None:
        self.assertEqual(g.fx(0.75), "0.75")
        self.assertEqual(g.fx(1.0), "1.0")
        self.assertEqual(g.fx(0.0), "0.0")

    def test_fx_outside_the_unit_interval_is_refused(self) -> None:
        with self.assertRaises(g.GrammarError):
            g.fx(1.5)
        with self.assertRaises(g.GrammarError):
            g.fx(-0.1)

    def test_a_value_with_a_space_is_quoted(self) -> None:
        self.assertEqual(g.quote("two words"), '"two words"')

    def test_a_bare_value_is_not_quoted(self) -> None:
        # Commas are legal unquoted; that is how a list is spelled.
        self.assertEqual(g.quote("a,b,c"), "a,b,c")

    def test_an_embedded_quote_is_escaped(self) -> None:
        self.assertEqual(g.quote('say "hi"'), '"say \\"hi\\""')

    def test_an_empty_value_is_refused(self) -> None:
        with self.assertRaises(g.GrammarError):
            g.quote("")

    def test_an_over_long_value_is_refused_rather_than_truncated(self) -> None:
        with self.assertRaises(g.GrammarError) as cm:
            g.quote("x" * g.MAX_VAL_LEN)

        self.assertIn("MAX_VAL_LEN", str(cm.exception))

    def test_an_over_long_key_is_refused(self) -> None:
        with self.assertRaises(g.GrammarError):
            g.key("nightmare." + "x" * g.MAX_VAR_LEN)

    def test_an_empty_list_cannot_be_spelled(self) -> None:
        with self.assertRaises(g.GrammarError):
            g.name_list([])

    def test_join_refuses_a_runaway_command_line(self) -> None:
        with self.assertRaises(g.GrammarError) as cm:
            g.join([f"k{i}=" + "v" * 200 for i in range(40)])

        self.assertIn("exceeds limit", str(cm.exception))


if __name__ == "__main__":
    unittest.main()
