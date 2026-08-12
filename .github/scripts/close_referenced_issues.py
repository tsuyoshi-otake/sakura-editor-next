"""Resolve GitHub closing keywords for a pull request merged into develop.

GitHub applies closing keywords only when a pull request merges into the
repository's default branch.  This repository's default branch is ``main`` while
the documented branch policy requires every ordinary pull request to target
``develop``, so the native mechanism can never fire for normal work.  This
helper reproduces the keyword resolution for the ``develop`` merge.

Two deliberate divergences from GitHub's own behaviour keep the automation safe
in this repository:

* Only references that belong to this repository are resolved.  A qualified
  ``owner/repo#N`` reference or an issue URL is accepted only when it names this
  repository exactly, so the fork-only boundary in ``CLAUDE.md`` cannot be
  violated by closing an upstream issue.
* Keywords inside HTML comments, fenced code blocks, and inline code spans are
  ignored, so the pull request template's own commentary cannot close anything.
"""

from __future__ import annotations

import os
import re
import sys
from dataclasses import dataclass


CLOSING_KEYWORDS = (
    "close",
    "closes",
    "closed",
    "fix",
    "fixes",
    "fixed",
    "resolve",
    "resolves",
    "resolved",
)

_KEYWORD = r"clos(?:e|es|ed)|fix(?:es|ed)?|resolv(?:e|es|ed)"

_REPOSITORY_NAME = r"[A-Za-z0-9._-]+/[A-Za-z0-9._-]+"

_REFERENCE = "|".join(
    (
        r"https?://github\.com/(?P<url_repository>" + _REPOSITORY_NAME + r")/issues/(?P<url_number>\d+)",
        r"(?P<qualified_repository>" + _REPOSITORY_NAME + r")#(?P<qualified_number>\d+)",
        r"#(?P<bare_number>\d+)",
        r"GH-(?P<gh_number>\d+)",
    )
)

# The keyword and its reference must share a line.  GitHub is more permissive,
# but a line-bounded rule cannot join an unrelated sentence to the number that
# happens to open the next line.
_CLOSING_REFERENCE = re.compile(
    r"(?<![0-9A-Za-z_-])(?:" + _KEYWORD + r")[ \t]*:?[ \t]*(?:" + _REFERENCE + r")",
    re.IGNORECASE,
)

_HTML_COMMENT = re.compile(r"<!--.*?-->", re.DOTALL)
_INLINE_CODE = re.compile(r"`[^`\n]*`")


@dataclass(frozen=True)
class ClosingReferences:
    """Issue numbers to close, plus the references deliberately left alone."""

    numbers: tuple
    foreign: tuple


def strip_ignored_regions(text):
    """Remove HTML comments, fenced code blocks, and inline code spans."""

    lines = []
    fence = None
    for line in _HTML_COMMENT.sub(" ", text).splitlines():
        stripped = line.lstrip()
        marker = None
        if stripped.startswith("```"):
            marker = "```"
        elif stripped.startswith("~~~"):
            marker = "~~~"

        if fence is None:
            if marker is not None:
                fence = marker
                continue
        else:
            if marker == fence:
                fence = None
            continue

        lines.append(_INLINE_CODE.sub(" ", line))
    return "\n".join(lines)


def resolve_closing_references(text, repository):
    """Resolve same-repository closing references from ``text``."""

    owner_repository = repository.strip().lower()
    if not owner_repository:
        raise ValueError("repository must name an owner/repository pair")

    numbers = []
    foreign = []
    for match in _CLOSING_REFERENCE.finditer(strip_ignored_regions(text or "")):
        groups = match.groupdict()
        if groups["url_number"] is not None:
            target = groups["url_repository"].lower()
            number = int(groups["url_number"])
        elif groups["qualified_number"] is not None:
            target = groups["qualified_repository"].lower()
            number = int(groups["qualified_number"])
        else:
            target = owner_repository
            number = int(groups["bare_number"] or groups["gh_number"])

        if target != owner_repository:
            # Collapse whitespace so a matched reference can never carry a
            # newline into a GitHub workflow command when it is logged.
            foreign.append(" ".join(match.group(0).split()))
            continue

        if number > 0 and number not in numbers:
            numbers.append(number)

    return ClosingReferences(numbers=tuple(sorted(numbers)), foreign=tuple(foreign))


def main(argv=None):
    del argv

    repository = os.environ.get("REPOSITORY", "").strip()
    if not repository:
        print("::error::REPOSITORY must name the owner/repository pair.", file=sys.stderr)
        return 1

    text = "\n".join(
        part for part in (os.environ.get("PR_TITLE", ""), os.environ.get("PR_BODY", "")) if part
    )
    references = resolve_closing_references(text, repository)

    for reference in references.foreign:
        print("::notice::Ignored a closing reference outside " + repository + ": " + reference)

    rendered = " ".join(str(number) for number in references.numbers)
    print("Resolved closing references: " + (rendered or "(none)"))

    output_path = os.environ.get("GITHUB_OUTPUT")
    if output_path:
        with open(output_path, "a", encoding="utf-8") as handle:
            handle.write("numbers=" + rendered + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
