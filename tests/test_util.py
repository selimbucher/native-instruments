from pathlib import Path

import pytest

from ni_wine import util


def test_wrap_keeps_short_lines_and_indentation():
    msg = "first line\n  indented second line"
    assert util._wrap(msg) == msg


def test_wrap_wraps_long_lines_without_splitting_words():
    path = "/very/long/path/" + "x" * 80
    wrapped = util._wrap(f"the file {path} is missing", width=40)
    lines = wrapped.splitlines()
    assert path in lines
    assert all(len(line) <= 40 for line in lines if line != path)


def test_wrap_keeps_indent_on_continuation_lines():
    msg = "    " + " ".join(["word"] * 30)
    for line in util._wrap(msg, width=40).splitlines():
        assert line.startswith("    ")


def make_prefix(root: Path) -> Path:
    prefix = root / "deep" / "wine-ni"
    (prefix / "drive_c" / "users").mkdir(parents=True)
    return prefix


def test_guarded_rmtree_deletes_inside_prefix(tmp_path):
    prefix = make_prefix(tmp_path)
    target = prefix / "drive_c" / "users"
    (target / "file").write_text("x")
    util.guarded_rmtree(target, prefix=prefix)
    assert not target.exists()
    assert prefix.exists()


def test_guarded_rmtree_deletes_the_prefix_itself(tmp_path):
    prefix = make_prefix(tmp_path)
    util.guarded_rmtree(prefix, prefix=prefix)
    assert not prefix.exists()


def test_guarded_rmtree_missing_path_is_a_noop(tmp_path):
    prefix = make_prefix(tmp_path)
    util.guarded_rmtree(prefix / "drive_c" / "nope", prefix=prefix)


def test_guarded_rmtree_refuses_outside_prefix(tmp_path):
    prefix = make_prefix(tmp_path)
    outside = tmp_path / "other"
    outside.mkdir()
    with pytest.raises(RuntimeError, match="outside the prefix"):
        util.guarded_rmtree(outside, prefix=prefix)
    assert outside.exists()


def test_guarded_rmtree_refuses_a_prefix_without_drive_c(tmp_path):
    prefix = tmp_path / "deep" / "not-a-prefix"
    (prefix / "sub").mkdir(parents=True)
    with pytest.raises(RuntimeError, match="not a Wine prefix"):
        util.guarded_rmtree(prefix / "sub", prefix=prefix)
    assert (prefix / "sub").exists()


def test_guarded_rmtree_refuses_home_and_root(tmp_path):
    with pytest.raises(RuntimeError, match="implausible"):
        util.guarded_rmtree(Path.home() / "x", prefix=Path.home())
    with pytest.raises(RuntimeError, match="implausible"):
        util.guarded_rmtree(Path("/x"), prefix=Path("/"))


def test_guarded_rmtree_refuses_shallow_prefix():
    with pytest.raises(RuntimeError, match="implausible"):
        util.guarded_rmtree(Path("/tmp/x"), prefix=Path("/tmp"))


def test_which_first_returns_first_found(monkeypatch, tmp_path):
    exe = tmp_path / "second"
    exe.write_text("")
    exe.chmod(0o755)
    monkeypatch.setenv("PATH", str(tmp_path))
    assert util.which_first("first", "second") == str(exe)
    assert util.which_first("first") is None
