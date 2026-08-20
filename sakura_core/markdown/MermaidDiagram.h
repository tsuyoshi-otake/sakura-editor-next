/*!	@file
	@brief Native layout for the Mermaid flowchart subset

	The preview renders Mermaid itself rather than shelling out to a browser, so
	this module owns the whole path from Mermaid source to placed geometry. It is
	deliberately free of Win32: text measurement arrives through a callback, and
	the result is plain coordinates, so the layout is unit-testable without a
	device context.

	The layout is the layered (Sugiyama) pipeline that dagre - and therefore
	Mermaid itself - uses, in the same three phases those implementations use and
	that the Rust `mermaid-text` crate documents: rank by longest path from the
	sources, order within each rank by an iterative barycenter sweep to reduce
	crossings, then assign coordinates. Edges that span more than one rank are
	broken by virtual nodes first, which is what keeps a long edge from being
	drawn straight through an unrelated box.

	Only the flowchart family is handled. Every other diagram type, and every
	flowchart feature this module does not implement, returns an explicit
	unsupported outcome so the caller can fall back to inert literal source
	rather than drawing something that is not what the author wrote.
*/
/*
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
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace markdown::mermaid {

//! The node shapes the flowchart syntax can request.
enum class NodeShape {
	Rectangle,   //!< `id[label]`
	Rounded,     //!< `id(label)`
	Stadium,     //!< `id([label])`
	Circle,      //!< `id((label))`
	Rhombus,     //!< `id{label}`
	Hexagon,     //!< `id{{label}}`
};

//! How an edge's line is stroked.
enum class EdgeStyle {
	Solid,
	Dotted,
	Thick,
};

enum class Direction {
	TopDown,
	BottomUp,
	LeftRight,
	RightLeft,
};

//! Why a diagram is not being drawn natively.
enum class BuildOutcome {
	Supported,
	UnsupportedSyntax,
	LimitExceeded,
};

struct DiagramPoint {
	int x = 0;
	int y = 0;
};

struct DiagramNode {
	std::wstring id;
	std::wstring label;
	NodeShape shape = NodeShape::Rectangle;
	//! Top-left corner and size, in the diagram's own coordinate space.
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

struct DiagramEdge {
	std::size_t from = 0;
	std::size_t to = 0;
	EdgeStyle style = EdgeStyle::Solid;
	//! False for the plain link forms (`---`, `-.-`, `===`), which have no head.
	bool arrow = true;
	std::wstring label;
	//! The routed polyline, always at least two points.
	std::vector<DiagramPoint> points;
	//! Centre of the edge label, meaningful only when `label` is non-empty.
	DiagramPoint labelCenter;
};

struct Diagram {
	Direction direction = Direction::TopDown;
	std::vector<DiagramNode> nodes;
	std::vector<DiagramEdge> edges;
	int width = 0;
	int height = 0;
};

//! Spacing, in device pixels, that the caller has already scaled for its DPI.
struct LayoutMetrics {
	int nodePaddingX = 18;
	int nodePaddingY = 10;
	//! Gap between consecutive ranks, along the flow direction.
	int rankSeparation = 44;
	//! Gap between siblings inside one rank.
	int nodeSeparation = 26;
	int minimumNodeWidth = 44;
	int lineHeight = 20;
	int edgeLabelPadding = 4;
};

/*!
	@brief Bounds that keep a hostile diagram from becoming a denial of service

	Exceeding any of these is reported as `LimitExceeded` rather than truncated,
	so the caller shows the source instead of a diagram missing half its nodes.
*/
struct BuildLimits {
	std::size_t maximumNodes = 512;
	std::size_t maximumEdges = 1024;
	std::size_t maximumStatements = 4096;
};

//! Returns the rendered width of a label, in device pixels.
using MeasureText = std::function<int(std::wstring_view)>;

/*!
	@brief Parses and lays out a Mermaid flowchart

	@param source	The fenced block's contents, without the fence lines
	@param metrics	Already DPI-scaled spacing
	@param limits	Size bounds
	@param measure	Label width measurement, in the face the caller will draw with
	@param diagram	Receives the placed geometry, untouched unless `Supported`

	@retval BuildOutcome::Supported			`diagram` holds a complete layout
	@retval BuildOutcome::UnsupportedSyntax	Not a flowchart, or uses a feature this module does not draw
	@retval BuildOutcome::LimitExceeded		The diagram is larger than `limits` allows
*/
[[nodiscard]] BuildOutcome BuildFlowchart(std::wstring_view source, const LayoutMetrics& metrics,
	const BuildLimits& limits, const MeasureText& measure, Diagram* diagram);

} // namespace markdown::mermaid
