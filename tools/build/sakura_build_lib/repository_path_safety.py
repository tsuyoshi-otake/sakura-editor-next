"""Generic repository path safety helpers for build evidence consumers.

Evidence readers accept paths from untrusted JSON or command-line input.  This
module owns the shared lexical containment, reparse-point, and regular-file
checks so specialised evidence producers do not depend on one another.
"""

from __future__ import annotations

import os
import stat
from pathlib import Path, PureWindowsPath

from .runner import BuildError


_REPARSE_POINT = 0x400


class RepositoryPathSafetyError(BuildError):
    """Typed, fail-closed error for an unsafe repository path."""

    def __init__(self, code: str, message: str, exit_code: int = 5) -> None:
        super().__init__(code, message, exit_code)


def _absolute(root: Path, value: Path) -> Path:
    candidate = value if value.is_absolute() else root / value
    return Path(os.path.abspath(os.fspath(candidate)))


def has_reparse_attribute(path: Path, code: str = "REPOSITORY_PATH_UNSAFE") -> bool:
    """Return whether *path* is a symlink or Windows reparse point.

    Missing paths are allowed while walking a prospective destination.  Every
    other inspection failure is typed rather than treated as a safe path.
    """

    try:
        info = os.lstat(path)
    except FileNotFoundError:
        return False
    except OSError as error:
        raise RepositoryPathSafetyError(code, "could not inspect path safety") from error
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, "st_file_attributes", 0) & _REPARSE_POINT)


def assert_inside_without_reparse(root: Path, candidate: Path, code: str) -> Path:
    """Resolve *candidate* while rejecting lexical escapes and reparse edges."""

    root_absolute = Path(os.path.abspath(os.fspath(root)))
    candidate_absolute = Path(os.path.abspath(os.fspath(candidate)))
    root_real = Path(os.path.realpath(os.fspath(root_absolute)))
    candidate_real = Path(os.path.realpath(os.fspath(candidate_absolute)))
    try:
        common = os.path.commonpath((os.fspath(root_real), os.fspath(candidate_real)))
    except ValueError as error:
        raise RepositoryPathSafetyError(code, "path is on a different volume") from error
    if os.path.normcase(common) != os.path.normcase(os.fspath(root_real)):
        raise RepositoryPathSafetyError(code, "path escapes the repository root")
    if not root_real.is_dir() or has_reparse_attribute(root_absolute, code):
        raise RepositoryPathSafetyError(code, "repository root is not a regular directory")

    # Walk the lexical path, rather than only the resolved path.  A symlink
    # that resolves back inside the repository is still a reparse boundary.
    current = candidate_absolute
    while True:
        if has_reparse_attribute(current, code):
            raise RepositoryPathSafetyError(code, "path contains a reparse point")
        if os.path.normcase(os.path.realpath(os.fspath(current))) == os.path.normcase(os.fspath(root_real)):
            break
        parent = current.parent
        if parent == current:
            raise RepositoryPathSafetyError(code, "path escapes the repository root")
        current = parent

    # Return the canonical spelling after the lexical reparse walk.  Temporary
    # directories on Windows are often passed through an 8.3 alias; without
    # this normalization relative-to-root checks would reject safe paths.
    return candidate_real


def regular_file(root: Path, value: Path, code: str) -> Path:
    """Resolve *value* below *root* and require a non-reparse regular file."""

    path = assert_inside_without_reparse(root, _absolute(root, value), code)
    try:
        info = os.stat(path, follow_symlinks=False)
    except OSError as error:
        raise RepositoryPathSafetyError(code, "regular file is unavailable") from error
    if not stat.S_ISREG(info.st_mode) or has_reparse_attribute(path, code):
        raise RepositoryPathSafetyError(code, "path is not a regular file")
    return path


def safe_repository_path(
    root: Path,
    value: str | Path,
    *,
    code: str,
    reject_parent_segments: bool = False,
    reject_absolute: bool = False,
    require_regular_file: bool = False,
) -> Path:
    """Resolve an untrusted repository path without crossing a reparse edge.

    Lexical ``..`` segments are rejected when requested even if they happen to
    resolve back inside the root.  The lexical ancestor walk in
    :func:`assert_inside_without_reparse` rejects symlinks and Windows reparse
    points, including ancestors that resolve back into the repository.
    """

    candidate = Path(value)
    if reject_parent_segments and ".." in candidate.parts:
        raise RepositoryPathSafetyError(code, "path contains a parent segment")
    if reject_absolute and (candidate.is_absolute() or bool(PureWindowsPath(str(value)).anchor)):
        raise RepositoryPathSafetyError(code, "path must be repository-relative")
    candidate_absolute = _absolute(root, candidate)
    if require_regular_file:
        return regular_file(root, candidate_absolute, code)
    return assert_inside_without_reparse(root, candidate_absolute, code)


__all__ = [
    "RepositoryPathSafetyError",
    "assert_inside_without_reparse",
    "has_reparse_attribute",
    "regular_file",
    "safe_repository_path",
]
