#!/usr/bin/env bash
# Cut a release: bump every version surface, commit, tag, pin the AUR
# build to the tag, and push.  Everything downstream is automatic:
# COPR rebuilds via webhook, GitHub Actions publishes AUR and the PPA.
#
#   scripts/release.sh 2.3.1
#
# Nothing reaches origin until the last step, and any failure before it
# rolls the repository back to where it started.  A half-cut release --
# the bump commit without its tag, say -- is worse than no release: the
# next run stacks a second bump on top and duplicates every changelog
# stanza.
set -euo pipefail

v=${1:?usage: scripts/release.sh <version>}
[[ $v =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "not a version: $v" >&2; exit 1; }
cd "$(git rev-parse --show-toplevel)"

# --- Preflight: everything that can refuse, before anything is changed ---
#
# The editor is checked here rather than where it is used, halfway down:
# on a machine without it (EDITOR unset and no vi -- not installed by
# default on Arch) the release used to die after committing the bump.

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

# The in-place edits below pass `sed -i` with no backup suffix and use \n
# in replacements; BSD/macOS sed accepts neither.
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
# Ahead of origin/main is fine (those commits go out with the release);
# behind or diverged means the bump would land on a stale tree.
if ! git merge-base --is-ancestor origin/main main; then
    echo "main is behind or diverged from origin/main -- pull first" >&2; exit 1
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
        echo "release failed -- rolling back to $(git rev-parse --short "$start")" >&2
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

# Fresh changelog stanza for the base series; the PPA workflow rewrites
# the series suffix per upload.
{
    printf 'ni-wine (%sppa1~noble1) noble; urgency=medium\n\n' "$v"
    printf '  * Release %s.\n\n' "$v"
    printf ' -- Selim Bucher <me@selim.one>  %s\n\n' "$(LC_ALL=C date -R)"
    cat debian/changelog
} > debian/changelog.new
mv debian/changelog.new debian/changelog

git add -A
git commit -m "chore: release $v"
# Draft the release notes in the repo's established style (New features /
# Fixes / Other with "component:" bullets) from the conventional commits
# since the last tag, then open the editor for curation.  The final text
# is stored in the tag annotation and becomes the GitHub Release body.
prev=$(git describe --tags --abbrev=0 HEAD^ 2>/dev/null || true)
range=${prev:+$prev..}HEAD
section() {
    local lines
    lines=$(git log --format='%s' "$range" \
        | grep -E "^$1[(:]" \
        | sed -E "s/^$1\(([^)]*)\): /\1: /; s/^$1: //; s/^/- /") || true
    # An empty section must not fail the function: under `set -e` a bare
    # `[ -n "$lines" ] && printf` returns 1 when the section is empty (e.g.
    # a release with no feat: commits), which would abort the release.
    if [ -n "$lines" ]; then
        printf '**%s**\n\n%s\n\n' "$2" "$lines"
    fi
}
{
    section feat "New features"
    section fix "Fixes"
    section chore "Other"
} > "$notes"
# Intentionally unquoted: EDITOR may carry arguments (e.g. "code -w").
# shellcheck disable=SC2086
$editor "$notes"
# Quitting the editor without saving, or emptying the buffer, would
# otherwise tag the release with a blank annotation and publish a GitHub
# Release with no body.
if ! grep -q '[^[:space:]]' "$notes"; then
    echo "release notes are empty -- aborting" >&2; exit 1
fi
git tag -a "v$v" -F "$notes"
tagged="v$v"

# The AUR build is pinned to the release commit (GitHub tarball checksums
# are unstable, so the PKGBUILD fetches by commit with SKIP sums).
h=$(git rev-parse "v$v^{commit}")
sed -i "s/^pkgver=.*/pkgver=$v/; s/^pkgrel=.*/pkgrel=1/; s/^_commit=.*/_commit=$h/" \
    packaging/aur/PKGBUILD
sed -i "s/pkgver = .*/pkgver = $v/; s/pkgrel = .*/pkgrel = 1/; s|#commit=[0-9a-f]*|#commit=$h|" \
    packaging/aur/.SRCINFO
git add packaging/aur
git commit -m "chore: AUR files for $v"

# --atomic: without it a partial push could leave the tag on origin while
# main was rejected, and the rollback below would then undo local state
# that origin already has.
git push --atomic origin main "v$v"

# Past the point of no return: nothing after this may roll back.
trap - EXIT
rm -f "$notes"
echo "released $v — COPR rebuilds via webhook; AUR + PPA publish via Actions"
