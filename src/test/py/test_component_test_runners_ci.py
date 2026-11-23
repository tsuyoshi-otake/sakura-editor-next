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
            names[headless + 1 : headless + 4],
            [
                "- name: Run component test runners",
                "- name: Verify the test inventory against the built runners",
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

    def test_inventory_verification_follows_component_runners_in_debug(self) -> None:
        workflow = read_workflow(BUILD)
        inventory_name = "- name: Verify the test inventory against the built runners"
        self.assertEqual(
            sum(step.lstrip().splitlines()[0] == inventory_name for step in _steps(workflow)),
            1,
        )

        steps = _steps(job(workflow, "build"))
        names = [step.lstrip().splitlines()[0] for step in steps]
        inventory_index = names.index(inventory_name)
        self.assertGreater(inventory_index, names.index("- name: Run component test runners"))
        inventory = steps[inventory_index]
        self.assertEqual(
            re.findall(r"(?m)^      if: (.*)$", inventory),
            ["${{ matrix.config == 'Debug' }}"],
        )
        self.assertNotRegex(inventory, r"(?m)^      continue-on-error:")
        self.assertIn("\n      timeout-minutes: 10\n", inventory)
        self.assertIn(
            "\n        & py -3 tools/build/sakura_build.py --format json "
            "test inventory verify-runtime src/test/test-inventory.json\n",
            inventory,
        )
        self.assertNotIn("--runner", inventory)
        self.assertIn(
            '\n        if ($LASTEXITCODE -ne 0) { throw "test inventory runtime verification '
            'failed with exit code $LASTEXITCODE." }\n',
            inventory,
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
