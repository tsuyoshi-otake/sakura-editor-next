"""Contracts for the release-promotion publish step (#156).

``Publish-ReleasePromotion.ps1`` creates a draft release, uploads the packaged
assets to it, verifies the uploaded digests against the values it hashed before
upload, and only then flips the draft to published.  Every one of those steps
has to look that draft up again, and the obvious endpoint cannot do it:
``GET /repos/{owner}/{repo}/releases/tags/{tag}`` resolves published releases
only, so a draft answers ``HTTP 404`` and reads as absent.

That is what broke ``v3.1.0-build.7360``.  ``resolve``, ``package`` and
``smoke`` were all green, the draft and all eight assets existed, and publish
died with ``Release 'v3.1.0-build.7360' was not found ... after draft
creation.``  Measured against a throwaway draft: the tag endpoint returned
``HTTP 404`` while enumerating ``/releases`` returned that same release with
``draft=True``.  It is not a race and no amount of waiting fixes it.

The damage is worse than one red job, because the same lookup is also the guard
that is supposed to notice a draft left behind by an earlier attempt.  A re-run
could not see the draft it had already created, so it walked past the guard
into draft creation and died there instead -- the workflow had no path forward
through itself, and the release had to be finished by hand.

Each contract below is pinned because breaking it fails in that same shape: a
green build with an unpublishable release, rather than a loud error.

* If any release lookup goes back to the tag endpoint, drafts become invisible
  again and publish returns to failing every single time.
* If the lookup stops paging, a repository with more than one page of releases
  silently reports a real draft as absent -- the same bug with a delay fuse.
* If "not found after draft creation" stops being an error, the script proceeds
  to publish a release it never confirmed, or reports success having published
  nothing.
* If the digest check stops requiring draft status, assets can be verified on a
  release that is already public, which is the one ordering the smoke/publish
  boundary exists to prevent.

Like ``test_cppcheck_analyzer_cache.py``, this lives in ``src/test/py`` because
the CTest ``pytest`` target runs from the repository root under pytest's
default ``norecursedirs``, which never collects ``tools/build/tests``.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
PUBLISH_SCRIPT = REPO_ROOT / ".github" / "scripts" / "Publish-ReleasePromotion.ps1"


def _read_powershell(path: Path) -> str:
    """Read a repository PowerShell script whatever encoding it checked out as.

    ``.gitattributes`` declares ``*.ps1 working-tree-encoding=utf-16le-bom``,
    so the working tree normally holds UTF-16LE even though the committed blob
    is UTF-8.  A checkout that did not apply the attribute still has to be
    readable here, otherwise this test fails for a reason that has nothing to
    do with what it is pinning.
    """
    raw = path.read_bytes()
    if raw[:2] in (b"\xff\xfe", b"\xfe\xff"):
        return raw.decode("utf-16")
    return raw.decode("utf-8-sig")


def _function_body(source: str, name: str) -> str:
    """Return the text of a PowerShell function definition, braces balanced."""
    match = re.search(r"^function\s+" + re.escape(name) + r"\s*\{", source, re.MULTILINE)
    if match is None:
        raise AssertionError("function '{0}' is not defined".format(name))
    depth = 0
    for index in range(match.end() - 1, len(source)):
        character = source[index]
        if character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                return source[match.start() : index + 1]
    raise AssertionError("function '{0}' has unbalanced braces".format(name))


class ReleaseLookupTests(unittest.TestCase):
    """The publish step must be able to see its own draft."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.source = _read_powershell(PUBLISH_SCRIPT)
        cls.lookup = _function_body(cls.source, "Get-ExistingRelease")

    def test_no_release_is_resolved_through_the_tag_endpoint(self) -> None:
        """A draft answers 404 there, so no lookup may use it.

        The comment above the implementation names the endpoint deliberately,
        so match the call rather than the mention.
        """
        calls = re.findall(r"gh\s+api\s+\"[^\"]*releases/tags/[^\"]*\"", self.source)
        self.assertEqual(
            [],
            calls,
            "release lookups must not use GET /releases/tags/<tag>; it cannot see drafts",
        )

    def test_the_lookup_enumerates_releases_and_matches_the_tag(self) -> None:
        """Enumerating /releases is what makes a draft visible."""
        self.assertRegex(
            self.lookup,
            r"gh\s+api\s+\"repos/\$Repo/releases\?",
            "Get-ExistingRelease must enumerate the release list",
        )
        self.assertRegex(
            self.lookup,
            r"\$_\.tag_name\s+-eq\s+\$Tag",
            "Get-ExistingRelease must match the release list on tag_name",
        )

    def test_the_lookup_pages_through_the_release_list(self) -> None:
        """One page of results is not the whole list.

        Without paging this reports a real draft as absent as soon as the
        repository accumulates more releases than a single page holds, which is
        the original bug with a delay fuse on it.
        """
        self.assertRegex(
            self.lookup, r"per_page=\$perPage", "the lookup must request a full page"
        )
        self.assertRegex(self.lookup, r"page=\$page", "the lookup must request a page number")
        self.assertRegex(
            self.lookup,
            r"\$page\+\+",
            "the lookup must advance to the next page",
        )
        self.assertRegex(
            self.lookup,
            r"\$releases\.Count\s+-lt\s+\$perPage",
            "a short page is the last page; the lookup must stop there",
        )
        self.assertRegex(
            self.lookup,
            r"\$page\s+-le\s+\$maxPages",
            "paging must be bounded so a pathological list cannot spin forever",
        )

    def test_a_failed_release_listing_is_an_error(self) -> None:
        """A listing that failed must not be reported as 'no such release'."""
        self.assertRegex(
            self.lookup,
            r"if\s*\(\$LASTEXITCODE\s+-ne\s+0\)\s*\{[^}]*throw",
            "a failed release listing must throw, not return null",
        )


class PublishOrderingTests(unittest.TestCase):
    """The draft must stay a draft until its uploaded assets are verified."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.source = _read_powershell(PUBLISH_SCRIPT)

    def test_a_release_missing_after_draft_creation_is_an_error(self) -> None:
        """Publish must not continue against a release it could not confirm."""
        resolver = _function_body(self.source, "Get-ReleaseJson")
        self.assertRegex(
            resolver,
            r"throw\s+\"Release '\$Tag' was not found",
            "a release that cannot be found after draft creation must be an error",
        )

    def test_assets_are_verified_while_the_release_is_still_a_draft(self) -> None:
        """Verification before publication is the point of the draft."""
        self.assertRegex(
            self.source,
            r"if\s*\(-not\s+\$release\.draft\)\s*\{[^}]*became public before",
            "the digest check must refuse a release that is already public",
        )

    def test_publication_happens_after_the_digest_comparison(self) -> None:
        """Order matters: compare digests, then flip the draft."""
        comparison = self.source.find("expected $expectedDigest, got")
        publication = self.source.find("gh release edit $TagName --repo $Repository --draft=false")
        self.assertNotEqual(-1, comparison, "the digest comparison is missing")
        self.assertNotEqual(-1, publication, "the publish command is missing")
        self.assertLess(
            comparison,
            publication,
            "assets must be compared before the release is published",
        )

    def test_an_existing_release_blocks_publication(self) -> None:
        """The guard that a visible draft finally makes useful."""
        self.assertRegex(
            self.source,
            r"already exists as a \$state release",
            "an existing release must stop publication rather than be overwritten",
        )


if __name__ == "__main__":
    unittest.main()
