/*!	@file
	@brief Tests for the Mermaid flowchart parser and layered layout

	Copyright (C) 2026, Sakura Editor NEXT

	This source code is designed for sakura editor.
	Please contact the copyright holder to use this code for other purposes.

	This software is provided 'as-is', without any express or implied
	warranty. In no event will the authors be held liable for any damages
	arising from the use of this software.

	Permission is granted to anyone to use this software for any purpose,
	including commercial applications, and to alter it and redistribute it
	freely, subject to the following restrictions:

		1. The origin of this software must not be misrepresented;
		   you must not claim that you wrote the original software.
		   If you use this software in a product, an acknowledgment
		   in the product documentation would be appreciated but is
		   not required.

		2. Altered source versions must be plainly marked as such,
		   and must not be misrepresented as being the original software.

		3. This notice may not be removed or altered from any source
		   distribution.
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "markdown/MermaidDiagram.h"

namespace markdown::mermaid {

namespace {

//! A deterministic stand-in for GDI measurement, so layout is testable.
int FixedWidth(std::wstring_view label)
{
	return static_cast<int>(label.size()) * 8;
}

BuildOutcome Build(std::wstring_view source, Diagram* diagram)
{
	return BuildFlowchart(source, LayoutMetrics{}, BuildLimits{}, FixedWidth, diagram);
}

[[nodiscard]] const DiagramNode* FindNode(const Diagram& diagram, std::wstring_view id)
{
	for (const auto& node : diagram.nodes) {
		if (node.id == id) return &node;
	}
	return nullptr;
}

} // namespace

TEST(MermaidDiagram, ParsesNodesShapesAndLabels)
{
	Diagram diagram;
	ASSERT_EQ(BuildOutcome::Supported, Build(
		L"flowchart TD\n"
		L"  A[Start] --> B(Round)\n"
		L"  B --> C{Choice}\n"
		L"  C --> D([Stadium])\n"
		L"  C --> E((Circle))\n"
		L"  C --> F{{Hexagon}}\n",
		&diagram));
	ASSERT_EQ(6u, diagram.nodes.size());
	EXPECT_EQ(5u, diagram.edges.size());
	EXPECT_EQ(L"Start", FindNode(diagram, L"A")->label);
	EXPECT_EQ(NodeShape::Rectangle, FindNode(diagram, L"A")->shape);
	EXPECT_EQ(NodeShape::Rounded, FindNode(diagram, L"B")->shape);
	EXPECT_EQ(NodeShape::Rhombus, FindNode(diagram, L"C")->shape);
	EXPECT_EQ(NodeShape::Stadium, FindNode(diagram, L"D")->shape);
	EXPECT_EQ(NodeShape::Circle, FindNode(diagram, L"E")->shape);
	EXPECT_EQ(NodeShape::Hexagon, FindNode(diagram, L"F")->shape);
	// A node first mentioned bare keeps the shape a later statement gives it.
	EXPECT_EQ(L"Round", FindNode(diagram, L"B")->label);
}

TEST(MermaidDiagram, ReadsEveryLinkForm)
{
	Diagram diagram;
	ASSERT_EQ(BuildOutcome::Supported, Build(
		L"graph LR\n"
		L"A --> B\n"
		L"B --- C\n"
		L"C -.-> D\n"
		L"D -.- E\n"
		L"E ==> F\n"
		L"F === G\n",
		&diagram));
	ASSERT_EQ(6u, diagram.edges.size());
	EXPECT_EQ(EdgeStyle::Solid, diagram.edges[0].style);
	EXPECT_TRUE(diagram.edges[0].arrow);
	EXPECT_EQ(EdgeStyle::Solid, diagram.edges[1].style);
	EXPECT_FALSE(diagram.edges[1].arrow);
	EXPECT_EQ(EdgeStyle::Dotted, diagram.edges[2].style);
	EXPECT_TRUE(diagram.edges[2].arrow);
	EXPECT_EQ(EdgeStyle::Dotted, diagram.edges[3].style);
	EXPECT_FALSE(diagram.edges[3].arrow);
	EXPECT_EQ(EdgeStyle::Thick, diagram.edges[4].style);
	EXPECT_TRUE(diagram.edges[4].arrow);
	EXPECT_EQ(EdgeStyle::Thick, diagram.edges[5].style);
	EXPECT_FALSE(diagram.edges[5].arrow);
}

TEST(MermaidDiagram, ReadsBothEdgeLabelForms)
{
	Diagram diagram;
	ASSERT_EQ(BuildOutcome::Supported, Build(
		L"flowchart TD\n"
		L"A -->|yes| B\n"
		L"A -- no --> C\n"
		L"C -. maybe .-> D\n"
		L"D == heavy ==> E\n",
		&diagram));
	ASSERT_EQ(4u, diagram.edges.size());
	EXPECT_EQ(L"yes", diagram.edges[0].label);
	EXPECT_EQ(L"no", diagram.edges[1].label);
	EXPECT_EQ(L"maybe", diagram.edges[2].label);
	EXPECT_EQ(L"heavy", diagram.edges[3].label);
}

TEST(MermaidDiagram, ChainsNodesInOneStatement)
{
	Diagram diagram;
	ASSERT_EQ(BuildOutcome::Supported, Build(L"graph TD\nA-->B-->C", &diagram));
	ASSERT_EQ(3u, diagram.nodes.size());
	ASSERT_EQ(2u, diagram.edges.size());
	EXPECT_EQ(0u, diagram.edges[0].from);
	EXPECT_EQ(1u, diagram.edges[0].to);
	EXPECT_EQ(1u, diagram.edges[1].from);
	EXPECT_EQ(2u, diagram.edges[1].to);
}

TEST(MermaidDiagram, RanksByLongestPath)
{
	Diagram diagram;
	ASSERT_EQ(BuildOutcome::Supported, Build(
		L"flowchart TD\nA --> B\nB --> C\nA --> C\n", &diagram));
	// C must sit below B, not beside it, because the longest path to it is two.
	EXPECT_LT(FindNode(diagram, L"A")->y, FindNode(diagram, L"B")->y);
	EXPECT_LT(FindNode(diagram, L"B")->y, FindNode(diagram, L"C")->y);
}

TEST(MermaidDiagram, MapsDirectionOntoTheFlowAxis)
{
	Diagram down;
	ASSERT_EQ(BuildOutcome::Supported, Build(L"flowchart TD\nA --> B", &down));
	EXPECT_LT(FindNode(down, L"A")->y, FindNode(down, L"B")->y);

	Diagram up;
	ASSERT_EQ(BuildOutcome::Supported, Build(L"flowchart BT\nA --> B", &up));
	EXPECT_GT(FindNode(up, L"A")->y, FindNode(up, L"B")->y);

	Diagram right;
	ASSERT_EQ(BuildOutcome::Supported, Build(L"flowchart LR\nA --> B", &right));
	EXPECT_LT(FindNode(right, L"A")->x, FindNode(right, L"B")->x);

	Diagram left;
	ASSERT_EQ(BuildOutcome::Supported, Build(L"flowchart RL\nA --> B", &left));
	EXPECT_GT(FindNode(left, L"A")->x, FindNode(left, L"B")->x);
}

TEST(MermaidDiagram, KeepsSiblingsApartAndInsideTheExtent)
{
	Diagram diagram;
	ASSERT_EQ(BuildOutcome::Supported, Build(
		L"flowchart TD\nA --> B\nA --> C\nA --> D", &diagram));
	const auto* b = FindNode(diagram, L"B");
	const auto* c = FindNode(diagram, L"C");
	const auto* d = FindNode(diagram, L"D");
	ASSERT_NE(nullptr, b);
	EXPECT_EQ(b->y, c->y);
	EXPECT_EQ(c->y, d->y);
	// No overlap along the rank.
	EXPECT_LE(b->x + b->width, c->x);
	EXPECT_LE(c->x + c->width, d->x);
	for (const auto& node : diagram.nodes) {
		EXPECT_GE(node.x, 0);
		EXPECT_GE(node.y, 0);
		EXPECT_LE(node.x + node.width, diagram.width);
		EXPECT_LE(node.y + node.height, diagram.height);
	}
}

TEST(MermaidDiagram, RoutesRankSpanningEdgesThroughWaypoints)
{
	Diagram diagram;
	ASSERT_EQ(BuildOutcome::Supported, Build(
		L"flowchart TD\nA --> B\nB --> C\nA --> C", &diagram));
	for (const auto& edge : diagram.edges) {
		ASSERT_GE(edge.points.size(), 2u);
	}
	// The A->C edge skips a rank, so it gains a waypoint rather than cutting
	// straight through the rank B occupies.
	std::size_t spanning = 0;
	for (const auto& edge : diagram.edges) {
		if (edge.points.size() > 2) ++spanning;
	}
	EXPECT_EQ(1u, spanning);
}

TEST(MermaidDiagram, TerminatesOnACycle)
{
	Diagram diagram;
	EXPECT_EQ(BuildOutcome::Supported, Build(
		L"flowchart TD\nA --> B\nB --> C\nC --> A", &diagram));
	EXPECT_EQ(3u, diagram.nodes.size());
	EXPECT_EQ(3u, diagram.edges.size());
}

TEST(MermaidDiagram, IgnoresCommentsAndSemicolons)
{
	Diagram diagram;
	ASSERT_EQ(BuildOutcome::Supported, Build(
		L"flowchart TD\n%% a comment\nA --> B; B --> C\n", &diagram));
	EXPECT_EQ(3u, diagram.nodes.size());
	EXPECT_EQ(2u, diagram.edges.size());
}

TEST(MermaidDiagram, FailsClosedOnUnsupportedInput)
{
	Diagram diagram;
	// Other diagram families are not this module's concern.
	EXPECT_EQ(BuildOutcome::UnsupportedSyntax,
		Build(L"sequenceDiagram\nAlice->>Bob: Hi", &diagram));
	EXPECT_EQ(BuildOutcome::UnsupportedSyntax,
		Build(L"classDiagram\nAnimal <|-- Duck", &diagram));
	// Flowchart features that change the meaning must not be silently dropped.
	EXPECT_EQ(BuildOutcome::UnsupportedSyntax,
		Build(L"flowchart TD\nsubgraph one\nA --> B\nend", &diagram));
	EXPECT_EQ(BuildOutcome::UnsupportedSyntax,
		Build(L"flowchart TD\nA --> B\nstyle A fill:#f9f", &diagram));
	EXPECT_EQ(BuildOutcome::UnsupportedSyntax,
		Build(L"flowchart TD\nA --> B\nclick A \"http://example.com\"", &diagram));
	// An unknown direction is a real diagram this module would draw wrongly.
	EXPECT_EQ(BuildOutcome::UnsupportedSyntax, Build(L"flowchart XY\nA --> B", &diagram));
	EXPECT_EQ(BuildOutcome::UnsupportedSyntax, Build(L"", &diagram));
}

TEST(MermaidDiagram, ReportsLimitsInsteadOfTruncating)
{
	std::wstring source = L"flowchart TD\n";
	for (int index = 0; index < 40; ++index) {
		source += L"N" + std::to_wstring(index) + L" --> N" + std::to_wstring(index + 1) + L"\n";
	}
	BuildLimits limits;
	limits.maximumNodes = 8;
	Diagram diagram;
	EXPECT_EQ(BuildOutcome::LimitExceeded,
		BuildFlowchart(source, LayoutMetrics{}, limits, FixedWidth, &diagram));
	EXPECT_TRUE(diagram.nodes.empty());
}

} // namespace markdown::mermaid
