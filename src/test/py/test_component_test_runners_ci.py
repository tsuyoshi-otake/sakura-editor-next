"""Workflow contracts for running generated component tests in MSVC CI (#440)."""

from __future__ import annotations

import re
import unittest

from test_ci_plan_workflow import BUILD, WORKFLOWS, job, read_workflow
from test_mingw_crt_pin import _steps


class ComponentTestRunnersCiTests(unittest.TestCase):
    def test_component_runners_follow_the_headless_suite(self) -> None:
        build = job(read_workflow(BUILD), "build")
        self.assertIn("\n    name: MSBuild\n", build)
        steps = _steps(build)
        names = [step.lstrip().splitlines()[0] for step in steps]
        headless = names.index("- name: Run headless tests")
        self.assertEqual(
            names[headless + 1 : headless + 3],
            [
                "- name: Run component test runners",
                "- name: Clean up test process survivors",
            ],
        )

        component_tests = steps[headless + 1]
        self.assertNotRegex(component_tests, r"(?m)^      (?:if|continue-on-error|shell):")
        self.assertRegex(component_tests, r"(?m)^      timeout-minutes: (?:[1-9]|1[0-9]|20)$")
        self.assertIn(
            '$context = "msvc-${{ matrix.platform }}-${{ matrix.config }}".ToLowerInvariant()',
            component_tests,
        )
        self.assertIn("sakura_build.py test components --context $context", component_tests)
        self.assertIn(
            '        if ($LASTEXITCODE -ne 0) {\n'
            '          throw "component test runners failed with exit code $LASTEXITCODE."\n'
            '        }',
            component_tests,
        )

    def test_msvc_workflows_use_graph_selected_component_runners(self) -> None:
        single_runner = re.compile(r"""\bsakura_build\.py["']?\s+test\s+component\s+\S+""")
        msvc_context = re.compile(r"\bmsvc-", re.IGNORECASE)
        for path in sorted([*WORKFLOWS.glob("*.yml"), *WORKFLOWS.glob("*.yaml")]):
            for step in _steps(read_workflow(path)):
                command_text = re.sub(r"[`\\]\n[ \t]*", " ", step)
                if single_runner.search(command_text):
                    self.assertNotRegex(
                        command_text, msvc_context, f"{path.name}: {step.lstrip().splitlines()[0]}"
                    )
