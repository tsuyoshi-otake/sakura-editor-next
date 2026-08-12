"""Contracts for the scope of the explicit-package-restore gate (#116).

``Directory.Build.targets`` sits at the repository root, so MSBuild imports it
into *every* project it evaluates beneath that root - not just the projects
that consume the restored package context. When the gate was introduced it
carried no scope, and two project families that never restore anything started
failing on the trusted-push path:

* ``tools/ToolBarTools/**/*.csproj`` builds at ``Platform=AnyCPU``, for which no
  ``SakuraModulesContextId`` exists, so "Build BmpTools" hit
  "No Sakura package context exists for Platform=AnyCPU".
* CMake writes throwaway probe projects under ``build/`` (``VCTargetsPath``,
  compiler identification). "Build CHM" configures CMake in a job that
  deliberately never restores, so its probe demanded an active package root
  that job had no reason to produce.

Both failures reached ``main`` because neither owning job runs on
``pull_request``. The invariant pinned here is the scope itself: the gate, and
the matching design-time projection gate, apply only to this repository's own
native projects.

The wording here deliberately avoids the filter/omission vocabulary that
``tools/build/sakura_build_lib/semantic_inventory.py`` counts as
``test.filtered_or_skipped`` in any path containing "test": that gate exists to
catch real test omission, and prose about MSBuild project scope must not spend
its budget.

Like ``test_develop_issue_closure.py``, this lives in ``src/test/py`` because
the CTest ``pytest`` target runs from the repository root under pytest's
default ``norecursedirs``, which never collects ``tools/build/tests``.
"""

from __future__ import annotations

import unittest
import xml.etree.ElementTree as ET
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
TARGETS = REPO_ROOT / "Directory.Build.targets"

SCOPE_PROPERTY = "SakuraOwnedNativeProject"
SCOPE_CONDITION = "'$(SakuraOwnedNativeProject)' == 'true'"

# Every target that refuses to build without a restored package context or a
# committed projection. Both are unbuildable for a project that owns neither.
SCOPED_TARGETS = (
    "SakuraValidateExplicitPackageRestore",
    "SakuraValidateGeneratedModelForDesignTime",
)

# Text that only ever appears in an error raised because the evaluated project
# has no package context. A new target carrying it must be scoped as well.
CONTEXTLESS_ERROR_MARKERS = (
    "No Sakura package context exists",
    "No committed Sakura component projection exists",
)


def _load() -> ET.Element:
    # MSBuild files here have no default namespace, so tags are unqualified.
    return ET.parse(TARGETS).getroot()


def _properties(root: ET.Element, name: str) -> list[ET.Element]:
    return [
        node
        for group in root.iter("PropertyGroup")
        for node in group
        if node.tag == name
    ]


def _target(root: ET.Element, name: str) -> ET.Element:
    for node in root.iter("Target"):
        if node.get("Name") == name:
            return node
    raise AssertionError(f"{TARGETS.name} declares no target named {name}")


class ScopePropertyTests(unittest.TestCase):
    def test_defaults_to_false_then_opts_in(self) -> None:
        nodes = _properties(_load(), SCOPE_PROPERTY)
        self.assertEqual(
            len(nodes),
            2,
            f"{SCOPE_PROPERTY} must be a default plus one opt-in, not "
            f"{len(nodes)} definitions",
        )
        default, opt_in = nodes
        self.assertIsNone(
            default.get("Condition"),
            "the default must be unconditional so an unrecognized project "
            "family is out of scope rather than accidentally in it",
        )
        self.assertEqual((default.text or "").strip(), "false")
        self.assertEqual((opt_in.text or "").strip(), "true")
        self.assertIsNotNone(opt_in.get("Condition"))

    def test_opt_in_requires_a_native_project_outside_the_generated_tree(self) -> None:
        _, opt_in = _properties(_load(), SCOPE_PROPERTY)
        condition = opt_in.get("Condition") or ""
        self.assertIn(
            "'$(MSBuildProjectExtension)' == '.vcxproj'",
            condition,
            "managed helper projects build at AnyCPU and consume no package "
            "context; they must stay out of scope",
        )
        self.assertIn(
            "!$(SakuraProjectDirectoryUpper.StartsWith("
            "'$(SakuraGeneratedBuildRootUpper)'))",
            condition,
            "CMake's probe projects under build/ must stay out of scope",
        )

    def test_scope_comparands_are_case_normalized_directories(self) -> None:
        root = _load()
        values = {
            name: (_properties(root, name)[0].text or "").strip()
            for name in (
                "SakuraGeneratedBuildRoot",
                "SakuraGeneratedBuildRootUpper",
                "SakuraProjectDirectoryNormalized",
                "SakuraProjectDirectoryUpper",
            )
        }
        # NormalizeDirectory appends the trailing separator on both sides, so a
        # project sitting directly in build\ is inside the prefix and a sibling
        # directory such as build-tools\ is not.
        self.assertEqual(
            values["SakuraGeneratedBuildRoot"],
            "$([MSBuild]::NormalizeDirectory('$(SakuraRepositoryRoot)', 'build'))",
        )
        self.assertEqual(
            values["SakuraProjectDirectoryNormalized"],
            "$([MSBuild]::NormalizeDirectory('$(MSBuildProjectDirectory)'))",
        )
        # Windows paths are case-insensitive; StartsWith is not.
        self.assertEqual(
            values["SakuraGeneratedBuildRootUpper"],
            "$(SakuraGeneratedBuildRoot.ToUpperInvariant())",
        )
        self.assertEqual(
            values["SakuraProjectDirectoryUpper"],
            "$(SakuraProjectDirectoryNormalized.ToUpperInvariant())",
        )


class ScopedTargetTests(unittest.TestCase):
    def test_validation_targets_carry_the_scope(self) -> None:
        root = _load()
        for name in SCOPED_TARGETS:
            with self.subTest(target=name):
                condition = _target(root, name).get("Condition") or ""
                self.assertIn(SCOPE_CONDITION, condition)

    def test_no_unscoped_target_demands_a_package_context(self) -> None:
        root = _load()
        for target in root.iter("Target"):
            errors = [
                node
                for node in target.iter("Error")
                if any(
                    marker in (node.get("Text") or "")
                    for marker in CONTEXTLESS_ERROR_MARKERS
                )
            ]
            if not errors:
                continue
            with self.subTest(target=target.get("Name")):
                self.assertIn(
                    SCOPE_CONDITION,
                    target.get("Condition") or "",
                    "a target that fails a project for having no package "
                    "context must apply only to projects that have one",
                )

    def test_escape_hatch_survives_the_scope(self) -> None:
        # The scope narrows which projects are validated; it must not quietly
        # remove the documented way to turn the validation off.
        condition = _target(_load(), "SakuraValidateExplicitPackageRestore").get(
            "Condition"
        )
        self.assertIn("'$(SakuraSkipPackageRestoreValidation)' != 'true'", condition)


if __name__ == "__main__":
    unittest.main()
