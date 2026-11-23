from __future__ import annotations

import io
import json
import sys
import unittest
from argparse import Namespace
from contextlib import redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import call, patch


TOOLS_BUILD = Path(__file__).resolve().parents[1]
REPO_ROOT = TOOLS_BUILD.parents[1]
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

import sakura_build
from sakura_build_lib.component_tests import select_component_test_runners
from sakura_build_lib.model import load_semantic_graph
from sakura_build_lib.runner import BuildError, EventWriter


CONTEXT = "msvc-x64-debug"


class ComponentTestSelectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.graph = load_semantic_graph(REPO_ROOT)
        manifest_path = REPO_ROOT / "src/main/modules/modules.json"
        cls.manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    def test_msvc_debug_selects_generated_runners_in_manifest_order(self) -> None:
        expected = tuple(
            component["id"]
            for component in self.manifest["components"]
            if component["kind"] == "test"
            and component["build_definition"] == "generated"
            and CONTEXT in component["supported_contexts"]
        )

        selected = select_component_test_runners(self.graph, CONTEXT)

        self.assertEqual(selected, expected)
        self.assertNotIn("tests1", selected)

    def test_shareddata_runner_requires_a_supported_context(self) -> None:
        runner_id = "sakura_shareddata_contract_tests"
        shareddata = next(
            component for component in self.manifest["components"] if component["id"] == runner_id
        )
        context = next(
            entry["id"]
            for entry in self.manifest["contexts"]
            if entry["id"] not in shareddata["supported_contexts"]
        )

        self.assertNotIn(runner_id, select_component_test_runners(self.graph, context))

    def test_empty_selection_raises_a_usage_error(self) -> None:
        graph = SimpleNamespace(context=lambda context_id: None, components={})

        with self.assertRaises(BuildError) as error:
            select_component_test_runners(graph, CONTEXT)

        self.assertEqual(error.exception.code, "COMPONENT_TESTS_EMPTY")
        self.assertEqual(error.exception.exit_code, sakura_build.EXIT_USAGE)


class ComponentTestCommandTests(unittest.TestCase):
    def test_aggregates_runner_results_and_preserves_execution_order(self) -> None:
        runner_ids = ("first_tests", "middle_tests", "last_tests")
        graph = SimpleNamespace(
            context=lambda context_id: None,
            components={
                runner_id: SimpleNamespace(
                    id=runner_id,
                    kind="test",
                    build_definition="generated",
                    supported_contexts=(CONTEXT,),
                )
                for runner_id in runner_ids
            },
        )
        args = Namespace(context=CONTEXT, format="json")
        events = EventWriter()
        cases = (
            ("all pass", (0, 0, 0), ("passed", "passed", "passed"), 0),
            (
                "middle runner fails",
                (0, sakura_build.EXIT_TEST, 0),
                ("passed", "test_failed", "passed"),
                sakura_build.EXIT_TEST,
            ),
            (
                "build failure takes precedence",
                (sakura_build.EXIT_BUILD, sakura_build.EXIT_TEST, 0),
                ("build_failed", "test_failed", "passed"),
                sakura_build.EXIT_BUILD,
            ),
        )

        for name, codes, statuses, expected_exit in cases:
            with self.subTest(name=name):
                stdout = io.StringIO()
                with (
                    patch.object(sakura_build, "_run_test_component", side_effect=codes) as run,
                    patch.object(sakura_build, "output") as output,
                    redirect_stdout(stdout),
                ):
                    exit_code = sakura_build._run_test_components(args, graph, events)

                self.assertEqual(exit_code, expected_exit)
                self.assertEqual(
                    run.call_args_list,
                    [call(runner_id, args, graph, events) for runner_id in runner_ids],
                )
                output.assert_called_once_with(
                    {
                        "context": CONTEXT,
                        "ok": expected_exit == 0,
                        "runners": [
                            {"component": runner_id, "status": status, "exit_code": code}
                            for runner_id, status, code in zip(runner_ids, statuses, codes)
                        ],
                    },
                    "json",
                )
                self.assertEqual(
                    stdout.getvalue().splitlines(),
                    [
                        *(f"component-test {runner_id} {status}" for runner_id, status in zip(runner_ids, statuses)),
                        f"component-tests {CONTEXT}: {statuses.count('passed')}/{len(runner_ids)} passed",
                    ],
                )

    def test_parser_accepts_components_execution_options(self) -> None:
        args = sakura_build.parser().parse_args(
            ["test", "components", "--context", CONTEXT, "--jobs", "4", "--dry-run"]
        )

        self.assertEqual(args.command, "test")
        self.assertEqual(args.test_command, "components")
        self.assertEqual(args.context, CONTEXT)
        self.assertEqual(args.jobs, 4)
        self.assertTrue(args.dry_run)
