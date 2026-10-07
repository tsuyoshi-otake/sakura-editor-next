#!/usr/bin/env python3
"""Unit tests for tools/architecture/include_layers.py.

Plain unittest, standard library only:
    py -3 -m unittest tools/architecture/tests/test_include_layers.py
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import include_layers as il  # noqa: E402


def make_layers(overrides):
    return {
        "schema_version": 2,
        "source_of_truth": "fake/modules.json",
        "derivation": {
            "contract_layer": "L0-contracts",
            "contract_prefixes": ["mono/include/api/"],
            "monolith_families": ["legacy"],
            "family_rank": {
                "legacy": 5,
                "platform-primitives": 2,
                "document-editor": 3,
                "win32-adapters": 4,
            },
            "rules": [],
        },
        "layers": [
            {"id": "L0-contracts", "rank": 0},
            {"id": "L1-foundation", "rank": 1},
            {"id": "L2-platform", "rank": 2},
            {"id": "L3-domain", "rank": 3},
            {"id": "L3-workbench-model", "rank": 3, "derived_target": True},
            {"id": "L4-legacy-ui", "rank": 4},
            {"id": "L5-composition", "rank": 5},
        ],
        "monolith_overrides": overrides,
        "exclude_prefixes": ["mono/vendor/"],
    }


def make_manifest(components=None, edges=None):
    if components is None:
        components = [
            {
                "id": "mono_app",
                "family": "legacy",
                "kind": "executable",
                "sources": ["mono"],
            },
            {
                "id": "comp_uri",
                "family": "platform-primitives",
                "kind": "implementation",
                "sources": ["mono/platform/uri/UriIdentity.cpp"],
                "public_headers": ["mono/include/api/uri/UriIdentity.h"],
                "private_headers": ["mono/platform/uri/UriIdentityInternal.h"],
            },
            {
                "id": "comp_selection",
                "family": "document-editor",
                "kind": "implementation",
                "sources": ["mono/workbench/editor/selection"],
                "public_headers": ["mono/include/api/editor/SelectionSession.h"],
            },
        ]
    return {
        "schema_version": 1,
        "components": components,
        "contracts": [],
        "edges": edges or [],
    }


def override(prefix, layer, reason="because", removal="when component_x exists"):
    return {
        "prefix": prefix,
        "layer": layer,
        "reason": reason,
        "removal_condition": removal,
    }


DEFAULT_OVERRIDES = [
    override("mono/platform/", "L2-platform"),
    override("mono/workbench/", "L3-workbench-model"),
    override("mono/doc/", "L3-domain"),
    override("mono/", "L5-composition"),
]


def build(overrides=None, components=None, edges=None):
    return il.build_model(
        make_layers(DEFAULT_OVERRIDES if overrides is None else overrides),
        make_manifest(components, edges),
    )


class TestDerivedClassification(unittest.TestCase):
    def test_component_source_takes_family_layer(self):
        model = build()
        self.assertEqual(
            il.classify("mono/platform/uri/UriIdentity.cpp", model),
            ("L2-platform", 2, il.KIND_COMPONENT),
        )

    def test_private_header_is_derived_from_ownership(self):
        model = build()
        self.assertEqual(
            il.classify("mono/platform/uri/UriIdentityInternal.h", model)[2],
            il.KIND_COMPONENT,
        )

    def test_directory_ownership_uses_family_rank(self):
        model = build()
        layer, rank, kind = il.classify(
            "mono/workbench/editor/selection/SelectionSession.cpp", model
        )
        self.assertEqual((layer, rank, kind), ("L3-workbench-model", 3, il.KIND_COMPONENT))

    def test_public_header_and_contract_prefix_are_contracts(self):
        model = build()
        self.assertEqual(
            il.classify("mono/include/api/uri/UriIdentity.h", model),
            ("L0-contracts", 0, il.KIND_CONTRACT),
        )
        self.assertEqual(
            il.classify("mono/include/api/anything/Else.h", model)[2], il.KIND_CONTRACT
        )

    def test_monolith_file_falls_back_to_longest_override(self):
        model = build()
        self.assertEqual(
            il.classify("mono/doc/CEditDoc.cpp", model),
            ("L3-domain", 3, il.KIND_OVERRIDE),
        )
        self.assertEqual(
            il.classify("mono/CEditApp.cpp", model),
            ("L5-composition", 5, il.KIND_OVERRIDE),
        )

    def test_unknown_path_is_reported_not_dropped(self):
        model = build()
        layer, rank, kind = il.classify("other/tree/File.cpp", model)
        self.assertIsNone(layer)
        self.assertIsNone(rank)
        self.assertEqual(kind, il.KIND_UNCLASSIFIED)

    def test_excluded_prefix_returns_none(self):
        model = build()
        self.assertIsNone(il.classify("mono/vendor/third_party.cpp", model))


class TestCheckDerivation(unittest.TestCase):
    def assertNoErrors(self, result):
        self.assertEqual(result["errors"], [], msg="unexpected errors")

    def test_clean_map_has_no_errors(self):
        result = il.check_derivation(build(), files=[])
        self.assertNoErrors(result)

    def test_override_on_component_owned_path_is_a_contradiction(self):
        overrides = DEFAULT_OVERRIDES + [
            override("mono/workbench/editor/selection/", "L4-legacy-ui")
        ]
        result = il.check_derivation(build(overrides), files=[])
        self.assertTrue(
            any("owned by non-legacy component 'comp_selection'" in e for e in result["errors"]),
            msg=result["errors"],
        )

    def test_override_rank_conflicting_with_family_rank_fails(self):
        overrides = [
            override("mono/workbench/", "L4-legacy-ui"),
            override("mono/", "L5-composition"),
        ]
        result = il.check_derivation(
            build(overrides),
            files=["mono/workbench/editor/selection/SelectionSession.cpp"],
        )
        self.assertTrue(
            any(
                "contradicts component 'comp_selection'" in e and "document-editor" in e
                for e in result["errors"]
            ),
            msg=result["errors"],
        )

    def test_matching_rank_is_accepted(self):
        result = il.check_derivation(
            build(),
            files=["mono/workbench/editor/selection/SelectionSession.cpp"],
        )
        self.assertNoErrors(result)

    def test_duplicate_prefix_with_different_layers_is_an_overlap(self):
        overrides = DEFAULT_OVERRIDES + [override("mono/doc/", "L4-legacy-ui")]
        result = il.check_derivation(build(overrides), files=[])
        self.assertTrue(
            any("overrides overlap" in e and "mono/doc" in e for e in result["errors"]),
            msg=result["errors"],
        )

    def test_nested_prefixes_are_not_an_overlap(self):
        overrides = DEFAULT_OVERRIDES + [override("mono/doc/legacy/", "L4-legacy-ui")]
        result = il.check_derivation(build(overrides), files=[])
        self.assertNoErrors(result)

    def test_missing_reason_and_removal_condition_fail(self):
        overrides = [
            override("mono/", "L5-composition", reason="", removal="   "),
        ]
        result = il.check_derivation(build(overrides), files=[])
        self.assertTrue(any("empty reason" in e for e in result["errors"]), msg=result["errors"])
        self.assertTrue(
            any("empty removal_condition" in e for e in result["errors"]), msg=result["errors"]
        )

    def test_unknown_layer_in_override_fails(self):
        overrides = [override("mono/", "L9-nope")]
        result = il.check_derivation(build(overrides), files=[])
        self.assertTrue(
            any("unknown layer" in e for e in result["errors"]), msg=result["errors"]
        )

    def test_stale_override_is_a_warning_not_an_error(self):
        overrides = [override("mono/", "L5-composition", removal="Remove when comp_uri owns it")]
        result = il.check_derivation(build(overrides), files=[])
        self.assertNoErrors(result)
        self.assertTrue(
            any("may be stale" in w and "comp_uri" in w for w in result["warnings"]),
            msg=result["warnings"],
        )

    def test_manifest_upward_edge_is_reported(self):
        edges = [
            {"from": "comp_uri", "to": "comp_selection", "kind": "implementation"},
            {"from": "comp_selection", "to": "comp_uri", "kind": "implementation"},
        ]
        result = il.check_derivation(build(edges=edges), files=[])
        self.assertNoErrors(result)
        self.assertEqual(len(result["manifest_upward_edges"]), 1)
        item = result["manifest_upward_edges"][0]
        self.assertEqual((item["from"], item["to"]), ("comp_uri", "comp_selection"))
        self.assertEqual((item["from_rank"], item["to_rank"]), (2, 3))


class TestRealMap(unittest.TestCase):
    def test_repository_map_is_consistent_with_the_manifest(self):
        model = il.load_model()
        result = il.check_derivation(model)
        self.assertEqual(result["errors"], [], msg="\n".join(result["errors"]))
        self.assertEqual(result["manifest_upward_edges"], [])

    def test_every_override_carries_a_justification(self):
        model = il.load_model()
        for item in model["_overrides"]:
            self.assertTrue(item["reason"], msg=item["prefix"])
            self.assertTrue(item["removal_condition"], msg=item["prefix"])


if __name__ == "__main__":
    unittest.main()
