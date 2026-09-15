#!/usr/bin/env bash
# Cut a release: bump every version surface, commit, tag, pin the AUR
# build to the tag, and push.  Everything downstream is automatic:
# COPR rebuilds via webhook, GitHub Actions publishes AUR and the PPA.
#
#   scripts/release.sh 2.3.1
set -euo pipefail

v=${1:?usage: scripts/release.sh <version>}
[[ $v =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "not a version: $v" >&2; exit 1; }
cd "$(git rev-parse --show-toplevel)"
[[ -z $(git status --porcelain) ]] || { echo "working tree not clean" >&2; exit 1; }
[[ $(git branch --show-current) == main ]] || { echo "not on main" >&2; exit 1; }

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
notes=$(mktemp)
{
    section feat "New features"
    section fix "Fixes"
    section chore "Other"
} > "$notes"
${EDITOR:-vi} "$notes"
git tag -a "v$v" -F "$notes"
rm -f "$notes"

# The AUR build is pinned to the release commit (GitHub tarball checksums
# are unstable, so the PKGBUILD fetches by commit with SKIP sums).
h=$(git rev-parse "v$v^{commit}")
sed -i "s/^pkgver=.*/pkgver=$v/; s/^pkgrel=.*/pkgrel=1/; s/^_commit=.*/_commit=$h/" \
    packaging/aur/PKGBUILD
sed -i "s/pkgver = .*/pkgver = $v/; s/pkgrel = .*/pkgrel = 1/; s|#commit=[0-9a-f]*|#commit=$h|" \
    packaging/aur/.SRCINFO
git add packaging/aur
git commit -m "chore: AUR files for $v"

git push origin main "v$v"
echo "released $v — COPR rebuilds via webhook; AUR + PPA publish via Actions"
