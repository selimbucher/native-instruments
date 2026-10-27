import pytest

from ni_wine import wine


@pytest.mark.parametrize(
    "build, severity",
    [
        (None, "warn"),
        ("not a version", "warn"),
        ("wine-8.0", "error"),
        ("wine-9.21 (Staging)", "error"),
        ("wine-10.0", "warn"),
        ("wine-11.6", "warn"),
    ],
)
def test_wine_version_problem_flags_bad_builds(build, severity):
    problem = wine.wine_version_problem(build)
    assert problem is not None
    assert problem[0] == severity


def test_wine_version_problem_11_6_names_the_bug():
    assert "59632" in wine.wine_version_problem("wine-11.6")[1]


@pytest.mark.parametrize("build", ["wine-11.0", "wine-11.14 (Staging)", "wine-12.1"])
def test_wine_version_problem_accepts_wine_11(build):
    assert wine.wine_version_problem(build) is None


REG = (
    "WINE REGISTRY Version 2\n"
    ";; All keys relative to \\\\Machine\n"
    "\n"
    "[Software\\\\Classes\\\\native-access] 1700000000\n"
    "#time=1dc1234\n"
    '@="URL:native-access"\n'
    '"URL Protocol"=""\n'
    "\n"
    "[Software\\\\Classes\\\\native-access\\\\shell\\\\open\\\\command] 1700000000\n"
    "#time=1dc1234\n"
    '@="C:\\\\windows\\\\system32\\\\start.exe /unix \\"/home/u/open-url.sh\\" \\"%1\\""\n'
    "\n"
)


def test_reg_default_unescapes_the_value():
    value = wine._reg_default(REG, r"Software\Classes\native-access\shell\open\command")
    assert value == r'C:\windows\system32\start.exe /unix "/home/u/open-url.sh" "%1"'


def test_reg_default_of_the_scheme_key():
    assert wine._reg_default(REG, r"Software\Classes\native-access") == "URL:native-access"


def test_reg_default_missing_key():
    assert wine._reg_default(REG, r"Software\Classes\other") is None
