#!/usr/bin/env bash
# Cut a release: bump the version everywhere, commit, tag, pin the AUR
# build to the tag and push. GitHub Actions then publishes AUR, PPA and COPR.
#
#   scripts/release.sh 2.3.1
#
# Nothing is pushed until the last step, any failure before it rolls back
# to the starting commit. A half-cut release (bump commit without the tag)
# would make the next run stack a second bump and duplicate the changelogs.
set -euo pipefail

v=${1:?usage: scripts/release.sh <version>}
[[ $v =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "not a version: $v" >&2; exit 1; }
cd "$(git rev-parse --show-toplevel)"

# --- preflight, before anything is changed ---
# the editor is checked up front: without one (EDITOR unset, no vi on Arch)
# the release used to die after committing the bump

editor=${EDITOR:-vi}
missing=()
for tool in git sed date mktemp "${editor%% *}"; do
    command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
if (( ${#missing[@]} )); then
    printf 'missing required command: %s\n' "${missing[@]}" >&2
    if [[ " ${missing[*]} " == *" ${editor%% *} "* ]]; then
        echo "set EDITOR to one you have, e.g. EDITOR=nano $0 $v" >&2
    fi
    exit 1
fi

# sed -i without a suffix and \n in replacements, BSD sed does neither
if ! sed --version >/dev/null 2>&1; then
    echo "GNU sed required" >&2; exit 1
fi

[[ -z $(git status --porcelain) ]] || { echo "working tree not clean" >&2; exit 1; }
[[ $(git branch --show-current) == main ]] || { echo "not on main" >&2; exit 1; }

if git rev-parse -q --verify "refs/tags/v$v" >/dev/null; then
    echo "tag v$v already exists locally" >&2; exit 1
fi
if ! git fetch --quiet origin; then
    echo "cannot reach origin" >&2; exit 1
fi
if [ -n "$(git ls-remote --tags origin "refs/tags/v$v")" ]; then
    echo "tag v$v already exists on origin" >&2; exit 1
fi
# ahead of origin/main is fine, behind or diverged is not
if ! git merge-base --is-ancestor origin/main main; then
    echo "main is behind or diverged from origin/main, pull first" >&2; exit 1
fi

# --- Rollback -----------------------------------------------------------

start=$(git rev-parse HEAD)
notes=$(mktemp)
tagged=""

cleanup() {
    local status=$?
    trap - EXIT
    rm -f "$notes" || true
    if [ "$status" -ne 0 ]; then
        echo "release failed, rolling back to $(git rev-parse --short "$start")" >&2
        if [ -n "$tagged" ]; then
            git tag -d "$tagged" >/dev/null 2>&1 || true
        fi
        git reset --hard --quiet "$start" >/dev/null 2>&1 || true
    fi
    exit "$status"
}
trap cleanup EXIT

sed -i "s/^__version__ = .*/__version__ = \"$v\"/" src/ni_wine/__init__.py
sed -i "s/^      version = .*/      version = \"$v\";/" flake.nix
sed -i "s/^Version:.*/Version:        $v/" packaging/fedora/ni-wine.spec
sed -i "/^%changelog/a * $(LC_ALL=C date '+%a %b %d %Y') Selim Bucher <me@selim.one> - $v-1\n- Release $v\n" \
    packaging/fedora/ni-wine.spec

# changelog stanza for the base series, the PPA workflow rewrites the suffix per series
{
    printf 'ni-wine (%sppa1~noble1) noble; urgency=medium\n\n' "$v"
    printf '  * Release %s.\n\n' "$v"
    printf ' -- Selim Bucher <me@selim.one>  %s\n\n' "$(LC_ALL=C date -R)"
    cat debian/changelog
} > debian/changelog.new
mv debian/changelog.new debian/changelog

git add -A
git commit -m "chore: release $v"
# draft the release notes from the conventional commits since the last tag
# and open the editor. The tag annotation becomes the GitHub release body.
prev=$(git describe --tags --abbrev=0 HEAD^ 2>/dev/null || true)
range=${prev:+$prev..}HEAD
section() {
    local lines
    lines=$(git log --format='%s' "$range" \
        | grep -E "^$1[(:]" \
        | sed -E "s/^$1\(([^)]*)\): /\1: /; s/^$1: //; s/^/- /") || true
    # not `[ -n ] && printf`: under set -e an empty section would abort the release
    if [ -n "$lines" ]; then
        printf '**%s**\n\n%s\n\n' "$2" "$lines"
    fi
}
{
    section feat "New features"
    section fix "Fixes"
    section chore "Other"
} > "$notes"
# unquoted, EDITOR may carry arguments (code -w)
# shellcheck disable=SC2086
$editor "$notes"
# an empty buffer would tag with a blank annotation and publish an empty release
if ! grep -q '[^[:space:]]' "$notes"; then
    echo "release notes are empty, aborting" >&2; exit 1
fi
git tag -a "v$v" -F "$notes"
tagged="v$v"

# the AUR build is pinned to the release commit, GitHub tarball checksums are unstable
h=$(git rev-parse "v$v^{commit}")
sed -i "s/^pkgver=.*/pkgver=$v/; s/^pkgrel=.*/pkgrel=1/; s/^_commit=.*/_commit=$h/" \
    packaging/aur/PKGBUILD
sed -i "s/pkgver = .*/pkgver = $v/; s/pkgrel = .*/pkgrel = 1/; s|#commit=[0-9a-f]*|#commit=$h|" \
    packaging/aur/.SRCINFO
git add packaging/aur
git commit -m "chore: AUR files for $v"

# --atomic: a partial push could leave the tag on origin while main was
# rejected, and the rollback would undo state origin already has
git push --atomic origin main "v$v"

# pushed, no rollback from here
trap - EXIT
rm -f "$notes"
echo "released $v, Actions publishes AUR, PPA and COPR"
