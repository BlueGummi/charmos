"""mkiso: the kernel command line spliced into limine.conf"""

from pathlib import Path

import pytest

import mkiso

CONF = "timeout: 0\n/charmOS\n    protocol: limine\n    path: boot():/boot/kernel\n    cmdline: root=nvme1p1\n"
CONF_NO_CMDLINE = (
    "timeout: 0\n/charmOS\n    protocol: limine\n    path: boot():/boot/kernel\n"
)


def conf(base: str = CONF, **env: str) -> str:
    return mkiso.render(base, mkiso.resolve_cmdline(env))


def refused(**env: str) -> str:
    with pytest.raises(mkiso.ConfError) as error:
        mkiso.resolve_cmdline(env)
    return str(error.value)


def test_no_environment_copies_the_conf_verbatim() -> None:
    assert conf() == CONF


def test_tests_becomes_a_filter_appended_to_the_existing_cmdline() -> None:
    assert "cmdline: root=nvme1p1 test.filter=avl,sort" in conf(TESTS="avl,sort")


def test_a_ragged_tests_list_is_normalised() -> None:
    assert "test.filter=avl,sort,hash" in conf(TESTS="avl,  sort  ,hash")


def test_a_trailing_comma_in_tests_is_refused() -> None:
    assert "trailing comma" in refused(TESTS="avl,")


def test_nightmare_tests_takes_exactly_one_name() -> None:
    assert "nightmare=locks" in conf(NIGHTMARE_TESTS="locks")
    assert "exactly one test name" in refused(NIGHTMARE_TESTS="locks,wake")


def test_a_cmdline_file_is_flattened_onto_one_line(tmp_path: Path) -> None:
    cmdline = tmp_path / "cmdline.txt"
    cmdline.write_text("nightmare=locks\nseed=0x1\n", encoding="utf-8")
    assert "cmdline: root=nvme1p1 nightmare=locks seed=0x1\n" in conf(
        CMDLINE=str(cmdline)
    )


def test_a_blank_cmdline_file_is_refused(tmp_path: Path) -> None:
    cmdline = tmp_path / "cmdline.txt"
    cmdline.write_text("\n  \n", encoding="utf-8")
    assert "is empty" in refused(CMDLINE=str(cmdline))


def test_a_missing_cmdline_file_is_refused(tmp_path: Path) -> None:
    assert "does not exist" in refused(CMDLINE=str(tmp_path / "nope.txt"))


def test_two_sources_together_are_refused() -> None:
    assert "set together" in refused(TESTS="avl", NIGHTMARE_TESTS="locks")


def test_extra_cmdline_composes_with_a_source() -> None:
    assert "test.filter=avl debug=1" in conf(TESTS="avl", EXTRA_CMDLINE="debug=1")


def test_extra_cmdline_alone_still_lands() -> None:
    assert "cmdline: root=nvme1p1 debug=1" in conf(EXTRA_CMDLINE="debug=1")


def test_a_conf_without_a_cmdline_line_gets_one_after_path() -> None:
    assert "    path: boot():/boot/kernel\n    cmdline: test.filter=avl" in conf(
        CONF_NO_CMDLINE, TESTS="avl"
    )


def test_wrong_arity_is_a_usage_error() -> None:
    assert mkiso.main(["mkiso.py"]) == 2
