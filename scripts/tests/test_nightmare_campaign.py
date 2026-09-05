import unittest
from dataclasses import dataclass, field
from pathlib import Path

from charm.nightmare import campaign as C
from charm.nightmare import codec
from charm.nightmare import suite as S
from charm.paths import nightmare_dir

SUITES = nightmare_dir() / "suites"
FIXTURES = nightmare_dir() / "fixtures" / "suites"

GATE_SEED = 0xFEEDFACE


def tokens(line: str) -> dict[str, str]:
    return dict(tok.split("=", 1) for tok in line.split(" "))


def limine_would_accept(text: str) -> bool:
    return bool(text.replace("\r", " ").replace("\n", " ").strip())


@dataclass
class RecordingBootRunner:
    status: str = C.BootStatus.OK.value
    calls: list[dict] = field(default_factory=list)

    def run_boot(
        self,
        manifest: C.CampaignManifest,
        task: S.Task,
        boot_index: int,
        cmdline: str,
        timeout_ms: int,
        out_dir: Path,
    ) -> C.BootResult:
        self.calls.append(
            {
                "task": task,
                "boot_index": boot_index,
                "cmdline": cmdline,
                "timeout_ms": timeout_ms,
                "out_dir": out_dir,
            }
        )
        return C.BootResult(
            boot_index=boot_index,
            task_name=task.name,
            cmdline=cmdline,
            seed=None,
            duration_ms=task.boot.duration_ms,
            exit_code=0,
            status=self.status,
            reason="stub",
            progress=1,
            findings=[],
        )


class GateCmdlineTests(unittest.TestCase):
    def setUp(self) -> None:
        self.task = S.load(SUITES / "overnight_locks.toml").task("locks_storm")

    def render(self) -> str:
        return codec.render_gate(self.task, base_seed=GATE_SEED)

    def test_the_gate_cmdline_is_never_empty(self) -> None:
        self.assertTrue(limine_would_accept(self.render()))

    def test_the_written_gate_file_is_one_limine_would_accept(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            path = codec.write(Path(tmp) / "gate" / "cmdline.txt", self.render())
            self.assertTrue(limine_would_accept(path.read_text()))

    def test_the_gate_runs_the_same_subject_as_the_campaign(self) -> None:
        self.assertEqual(tokens(self.render())["nightmare"], "locks_storm")

    def test_the_gate_runs_briefly(self) -> None:
        t = tokens(self.render())

        self.assertEqual(t["nightmare.duration_ms"], f"{codec.GATE_DURATION_MS}ms")
        self.assertLess(codec.GATE_DURATION_MS, self.task.boot.duration_ms)

    def test_the_gate_keeps_the_task_perturbers_and_options(self) -> None:
        t = tokens(self.render())

        self.assertEqual(t["nightmare.perturb"], "migrator,waker,stutter")
        self.assertEqual(t["nightmare.locks_storm.worker_stall_ms"], "10000ms")

    def test_the_gate_timeout_is_derived_from_the_gate_duration(self) -> None:
        gate = codec.gate_task(self.task)

        self.assertLess(gate.boot.host_timeout_ms, self.task.boot.host_timeout_ms)
        self.assertEqual(
            gate.boot.host_timeout_ms,
            codec.GATE_DURATION_MS + gate.boot.drain_grace_ms + S.FLUSH_MARGIN_MS,
        )

    def test_a_seeded_gate_carries_the_campaign_base_seed(self) -> None:
        self.assertEqual(tokens(self.render())["nightmare.seed"], "0x00000000feedface")

    def test_a_seedless_gate_renders_without_a_seed(self) -> None:
        seedless = S.load(FIXTURES / "valid" / "seedless.toml").tasks[0]
        line = codec.render_gate(seedless, base_seed=GATE_SEED)

        self.assertTrue(limine_would_accept(line))
        self.assertNotIn("nightmare.seed=", line)


class GateDefaultTests(unittest.TestCase):
    def test_gating_is_on_unless_a_suite_opts_out(self) -> None:
        self.assertTrue(S.Boot(duration_ms=1000).gate_first)


class GateExecutionTests(unittest.TestCase):
    def manifest(self, tmp: Path, **kw) -> C.CampaignManifest:
        return C.CampaignManifest(
            suite=S.load(SUITES / "overnight_locks.toml"),
            base_seed=GATE_SEED,
            campaign_id="test-campaign",
            budget_ms=1,  # no room for a real boot
            out_dir=tmp,
            **kw,
        )

    def test_the_gate_boot_runs_first_and_into_its_own_directory(self) -> None:
        import tempfile

        runner = RecordingBootRunner()
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            C.CampaignRunner(self.manifest(out), boot_runner=runner).execute()

        self.assertTrue(runner.calls)
        gate = runner.calls[0]
        self.assertEqual(gate["boot_index"], 0)
        self.assertEqual(gate["out_dir"], out / "gate")
        self.assertTrue(limine_would_accept(gate["cmdline"]))

    def test_the_gate_boot_is_given_the_gate_timeout_not_the_task_timeout(
        self,
    ) -> None:
        import tempfile

        runner = RecordingBootRunner()
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            manifest = self.manifest(out)
            C.CampaignRunner(manifest, boot_runner=runner).execute()

        real = manifest.suite.task("locks_storm").boot.host_timeout_ms
        self.assertEqual(
            runner.calls[0]["timeout_ms"],
            codec.gate_task(manifest.suite.task("locks_storm")).boot.host_timeout_ms,
        )
        self.assertLess(runner.calls[0]["timeout_ms"], real)

    def test_a_failing_gate_stops_the_campaign(self) -> None:
        import tempfile

        runner = RecordingBootRunner(status=C.BootStatus.CRASH.value)
        with tempfile.TemporaryDirectory() as tmp:
            result = C.CampaignRunner(
                self.manifest(Path(tmp)), boot_runner=runner
            ).execute()

        self.assertEqual(len(runner.calls), 1)
        self.assertFalse(result.ok)
        self.assertEqual(result.status, C.CampaignStatus.INFRASTRUCTURE.value)


if __name__ == "__main__":
    unittest.main()
