"""Select generated component test runners for a build context."""

from __future__ import annotations

from .model import SemanticGraph
from .runner import BuildError


def select_component_test_runners(graph: SemanticGraph, context_id: str) -> tuple[str, ...]:
    """Return the applicable runners in manifest declaration order."""
    graph.context(context_id)
    runners = tuple(
        component.id
        for component in graph.components.values()
        if component.kind == "test"
        and component.build_definition == "generated"
        and context_id in component.supported_contexts
    )
    if not runners:
        raise BuildError(
            "COMPONENT_TESTS_EMPTY",
            f"no generated component test runners support context {context_id}",
            2,
        )
    return runners
