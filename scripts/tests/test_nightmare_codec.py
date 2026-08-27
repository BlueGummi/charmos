import unittest
from pathlib import Path

from charm.nightmare import codec as C
from charm.nightmare import suite as S
from charm.paths import nightmare_dir

SUITES = nightmare_dir() / "suites"
FIXTURES = nightmare_dir() / "fixtures" / "suites"


def tokens(line: str) -> dict[str, str]:
    return dict(tok.split("=", 1) for tok in line.split(" "))


class RenderTests(unittest.TestCase):
    def setUp(self) -> None:
        self.suite = S.load(SUITES / "overnight_locks.toml")
        self.task = self.suite.task("locks_storm")

    def render(self, **kw) -> dict[str, str]:
        kw.setdefault("seed", 0xDEADBEEF)
        return tokens(C.render(self.task, C.BootRequest(**kw)))

    def test_selection_comes_first_and_is_the_bare_root(self) -> None:
        line = C.render(self.task, C.BootRequest(seed=1))

        self.assertTrue(line.startswith("nightmare=locks_storm "))

    def test_durations_carry_their_unit(self) -> None:
        t = self.render()

        self.assertEqual(t["nightmare.duration_ms"], "300000ms")
        self.assertEqual(t["nightmare.drain_grace_ms"], "20000ms")
        self.assertEqual(t["nightmare.perturb.migrator.interval_us"], "500us")

    def test_the_perturb_list_is_comma_separated(self) -> None:
        t = self.render()

        self.assertEqual(t["nightmare.perturb"], "migrator,waker,stutter")

    def test_identity_is_passed_through_untouched(self) -> None:
        t = self.render(boot_index=12, campaign_id="run-9:2")

        self.assertEqual(t["nightmare.boot_index"], "12")
        self.assertEqual(t["nightmare.campaign_id"], "run-9:2")

    def test_campaign_id_is_absent_rather_than_empty_when_unset(self) -> None:
        t = self.render()

        self.assertNotIn("nightmare.campaign_id", t)

    def test_rendering_is_deterministic(self) -> None:
        req = C.BootRequest(boot_index=1, seed=5)

        self.assertEqual(C.render(self.task, req), C.render(self.task, req))

    def test_nightmare_never_nests_under_test(self) -> None:
        line = C.render(self.task, C.BootRequest(seed=1))

        self.assertNotIn("test.", line)

    def test_a_campaign_id_the_cmdline_cannot_carry_is_refused(self) -> None:
        with self.assertRaises(C.CodecError):
            C.render(self.task, C.BootRequest(seed=1, campaign_id="two words"))


class SeedTests(unittest.TestCase):
    def setUp(self) -> None:
        self.split = S.load(SUITES / "overnight_locks.toml").task("locks_storm")
        self.seedless = S.load(FIXTURES / "valid" / "seedless.toml").tasks[0]

    def test_a_seeded_mode_refuses_to_render_without_a_seed(self) -> None:
        with self.assertRaises(C.CodecError) as cm:
            C.render(self.split, C.BootRequest())

        self.assertIn("requires a seed", str(cm.exception))

    def test_a_seedless_task_refuses_a_seed(self) -> None:
        with self.assertRaises(C.CodecError) as cm:
            C.render(self.seedless, C.BootRequest(seed=1))

        self.assertIn("seedless", str(cm.exception))

    def test_a_seedless_task_emits_no_seed(self) -> None:
        t = tokens(C.render(self.seedless, C.BootRequest()))

        self.assertNotIn("nightmare.seed", t)
        self.assertEqual(t["nightmare.seed_mode"], "seedless")

    def test_a_seed_is_rendered_as_hex(self) -> None:
        t = tokens(C.render(self.split, C.BootRequest(seed=0x1F)))

        self.assertEqual(t["nightmare.seed"], "0x000000000000001f")


class SeedPartitionTests(unittest.TestCase):
    def test_runners_never_share_a_seed(self) -> None:
        per_runner = 100
        seen = [
            C.seed_for(1000, runner=r, boot_index=b, boots_per_runner=per_runner)
            for r in range(4)
            for b in range(per_runner)
        ]

        self.assertEqual(len(seen), len(set(seen)))

    def test_a_runner_owns_a_contiguous_block(self) -> None:
        a = C.seed_for(0, runner=2, boot_index=0, boots_per_runner=50)
        b = C.seed_for(0, runner=2, boot_index=49, boots_per_runner=50)
        c = C.seed_for(0, runner=3, boot_index=0, boots_per_runner=50)

        self.assertEqual(a, 100)
        self.assertEqual(b, 149)
        self.assertEqual(c, 150)

    def test_the_space_wraps_rather_than_overflowing(self) -> None:
        seed = C.seed_for(
            0xFFFFFFFFFFFFFFFF, runner=0, boot_index=1, boots_per_runner=1
        )

        self.assertEqual(seed, 0)


class BuildTests(unittest.TestCase):
    def setUp(self) -> None:
        self.suite = S.load(SUITES / "overnight_locks.toml")

    def test_every_declared_definition_reaches_cmake(self) -> None:
        args = C.build_args(self.suite)

        for d in self.suite.build.cmake_definitions:
            self.assertIn(f"-D{d}", args)

    def test_topology_is_a_build_time_definition_not_a_boot_knob(self) -> None:
        args = C.build_args(self.suite)
        line = C.render(self.suite.task("locks_storm"), C.BootRequest(seed=1))

        self.assertIn("-DQEMU_SMP_TOPO=sockets=2,cores=2,threads=2", args)
        self.assertNotIn("smp", line)

    def test_memory_renders_in_gibibytes_when_exact(self) -> None:
        self.assertEqual(C._mem_size(8192), "8G")
        self.assertEqual(C._mem_size(1536), "1536M")

    def test_definitions_precede_the_passthrough_separator(self) -> None:
        cmd = C.build_command(self.suite)

        self.assertIn("--", cmd)
        self.assertLess(cmd.index("iso"), cmd.index("--"))
        self.assertGreater(cmd.index("-DDEBUG_ASAN=ON"), cmd.index("--"))


class ArtifactTests(unittest.TestCase):
    def test_the_written_cmdline_is_one_line_ending_in_a_newline(self) -> None:
        import tempfile

        task = S.load(SUITES / "overnight_locks.toml").task("locks_storm")
        line = C.render(task, C.BootRequest(seed=1))

        with tempfile.TemporaryDirectory() as tmp:
            path = C.write(Path(tmp) / "sub" / "n.txt", line)
            text = path.read_text()

        self.assertEqual(text, line + "\n")
        self.assertEqual(len(text.splitlines()), 1)


if __name__ == "__main__":
    unittest.main()
