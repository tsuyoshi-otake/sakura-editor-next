/*!	@file
	@brief Mermaid flowchart parsing and layered layout

	See MermaidDiagram.h for the boundary this module keeps.
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
#include "StdAfx.h"

#include "MermaidDiagram.h"

#include <algorithm>
#include <limits>
#include <map>
#include <unordered_map>

namespace markdown::mermaid {

namespace {

constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();

[[nodiscard]] bool IsSpace(wchar_t value) noexcept
{
	return value == L' ' || value == L'\t' || value == L'\r' || value == L'\n';
}

[[nodiscard]] std::wstring_view Trim(std::wstring_view value) noexcept
{
	while (!value.empty() && IsSpace(value.front())) value.remove_prefix(1);
	while (!value.empty() && IsSpace(value.back())) value.remove_suffix(1);
	return value;
}

//! Strips the optional quoting Mermaid allows around a label.
[[nodiscard]] std::wstring_view Unquote(std::wstring_view value) noexcept
{
	value = Trim(value);
	if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
		value.remove_prefix(1);
		value.remove_suffix(1);
	}
	return Trim(value);
}

[[nodiscard]] bool IsIdentifierChar(wchar_t value) noexcept
{
	return (value >= L'a' && value <= L'z') || (value >= L'A' && value <= L'Z')
		|| (value >= L'0' && value <= L'9') || value == L'_' || value == L'.';
}

//! A leading word, used to recognize both the header and unsupported keywords.
[[nodiscard]] std::wstring_view FirstWord(std::wstring_view line) noexcept
{
	line = Trim(line);
	std::size_t length = 0;
	while (length < line.size() && !IsSpace(line[length])) ++length;
	return line.substr(0, length);
}

/*!
	@brief Splits the block into statements

	Mermaid separates statements by newlines and by `;`, and treats `%%` as a
	comment to end of line.
*/
[[nodiscard]] std::vector<std::wstring_view> SplitStatements(std::wstring_view source)
{
	std::vector<std::wstring_view> statements;
	std::size_t start = 0;
	const auto flush = [&statements](std::wstring_view piece) {
		const auto comment = piece.find(L"%%");
		if (comment != std::wstring_view::npos) piece = piece.substr(0, comment);
		piece = Trim(piece);
		if (!piece.empty()) statements.push_back(piece);
	};
	for (std::size_t index = 0; index <= source.size(); ++index) {
		if (index == source.size() || source[index] == L'\n' || source[index] == L';') {
			flush(source.substr(start, index - start));
			start = index + 1;
		}
	}
	return statements;
}

struct EdgeToken {
	std::size_t start = 0;
	std::size_t end = 0;
	EdgeStyle style = EdgeStyle::Solid;
	bool arrow = true;
	std::wstring label;
};

/*!
	@brief Reads a complete link operator that begins at `index`

	Handles `-->`, `---`, `-.->`, `-.-`, `==>` and `===`. Returns false when the
	text at `index` opens a labelled form (`--`, `-.`, `==`) instead, in which
	case the caller looks for the matching closing operator.
*/
[[nodiscard]] bool ReadCompleteOperator(std::wstring_view source, std::size_t index,
	EdgeStyle style, std::size_t* end, bool* arrow) noexcept
{
	const auto size = source.size();
	if (style == EdgeStyle::Dotted) {
		// `-.->` and `-.-`: a dash, a run of dots, then the closing dash.
		std::size_t scan = index + 1;
		while (scan < size && source[scan] == L'.') ++scan;
		if (scan == index + 1 || scan >= size || source[scan] != L'-') return false;
		++scan;
		*arrow = scan < size && source[scan] == L'>';
		*end = *arrow ? scan + 1 : scan;
		return true;
	}
	const auto marker = style == EdgeStyle::Thick ? L'=' : L'-';
	std::size_t scan = index;
	while (scan < size && source[scan] == marker) ++scan;
	const auto run = scan - index;
	if (run < 2) return false;
	if (scan < size && source[scan] == L'>') {
		*arrow = true;
		*end = scan + 1;
		return true;
	}
	if (run >= 3) {
		*arrow = false;
		*end = scan;
		return true;
	}
	return false;
}

//! Where a link operator of `style` could begin at `index`.
[[nodiscard]] bool OpensOperator(std::wstring_view source, std::size_t index, EdgeStyle* style) noexcept
{
	if (index + 1 >= source.size()) return false;
	if (source[index] == L'=' && source[index + 1] == L'=') { *style = EdgeStyle::Thick; return true; }
	if (source[index] != L'-') return false;
	if (source[index + 1] == L'-') { *style = EdgeStyle::Solid; return true; }
	if (source[index + 1] == L'.') { *style = EdgeStyle::Dotted; return true; }
	return false;
}

/*!
	@brief Finds the next link operator at or after `from`

	@retval true  `token` describes the operator, including any label
	@retval false No operator remains, or one is malformed
*/
[[nodiscard]] bool FindEdge(std::wstring_view source, std::size_t from, EdgeToken* token, bool* malformed)
{
	*malformed = false;
	for (std::size_t index = from; index + 1 < source.size(); ++index) {
		EdgeStyle style = EdgeStyle::Solid;
		if (!OpensOperator(source, index, &style)) continue;
		token->start = index;
		token->style = style;
		std::size_t end = 0;
		bool arrow = true;
		if (ReadCompleteOperator(source, index, style, &end, &arrow)) {
			token->arrow = arrow;
			token->end = end;
			// `A -->|yes| B` carries the label after the operator.
			std::size_t scan = end;
			while (scan < source.size() && IsSpace(source[scan])) ++scan;
			if (scan < source.size() && source[scan] == L'|') {
				const auto closing = source.find(L'|', scan + 1);
				if (closing == std::wstring_view::npos) { *malformed = true; return false; }
				token->label.assign(Unquote(source.substr(scan + 1, closing - scan - 1)));
				token->end = closing + 1;
			} else {
				token->label.clear();
			}
			return true;
		}
		// A labelled form: `A -- yes --> B`, `A -. no .-> B`, `A == x ==> B`.
		const std::size_t openLength = 2;
		std::size_t scan = index + openLength;
		while (scan + 1 < source.size()) {
			std::size_t closeEnd = 0;
			bool closeArrow = true;
			const std::size_t candidate = style == EdgeStyle::Dotted ? scan : scan;
			EdgeStyle candidateStyle = style;
			if (style == EdgeStyle::Dotted) {
				// The closing form is `.->` or `.-`, so back the scan onto the dot.
				if (source[candidate] == L'.'
					&& ReadCompleteOperator(source, candidate - 1, style, &closeEnd, &closeArrow)
					&& closeEnd > candidate) {
					token->arrow = closeArrow;
					token->end = closeEnd;
					token->label.assign(Unquote(source.substr(index + openLength,
						candidate - 1 - (index + openLength))));
					return true;
				}
			} else if (OpensOperator(source, candidate, &candidateStyle) && candidateStyle == style
				&& ReadCompleteOperator(source, candidate, style, &closeEnd, &closeArrow)) {
				token->arrow = closeArrow;
				token->end = closeEnd;
				token->label.assign(Unquote(source.substr(index + openLength,
					candidate - (index + openLength))));
				return true;
			}
			++scan;
		}
		*malformed = true;
		return false;
	}
	return false;
}

struct ParsedNode {
	std::wstring id;
	std::wstring label;
	NodeShape shape = NodeShape::Rectangle;
	bool labelled = false;
};

/*!
	@brief Reads one node reference, with its optional shape and label

	@retval false The text is not a usable node reference
*/
[[nodiscard]] bool ParseNodeChunk(std::wstring_view chunk, ParsedNode* node)
{
	chunk = Trim(chunk);
	if (chunk.empty()) return false;
	std::size_t idLength = 0;
	while (idLength < chunk.size() && IsIdentifierChar(chunk[idLength])) ++idLength;
	if (idLength == 0) return false;
	node->id.assign(chunk.substr(0, idLength));
	node->label = node->id;
	node->shape = NodeShape::Rectangle;
	node->labelled = false;
	auto rest = Trim(chunk.substr(idLength));
	if (rest.empty()) return true;

	std::wstring_view open;
	std::wstring_view close;
	if (rest.starts_with(L"((")) { open = L"(("; close = L"))"; node->shape = NodeShape::Circle; }
	else if (rest.starts_with(L"([")) { open = L"(["; close = L"])"; node->shape = NodeShape::Stadium; }
	else if (rest.starts_with(L"{{")) { open = L"{{"; close = L"}}"; node->shape = NodeShape::Hexagon; }
	else if (rest.starts_with(L"(")) { open = L"("; close = L")"; node->shape = NodeShape::Rounded; }
	else if (rest.starts_with(L"{")) { open = L"{"; close = L"}"; node->shape = NodeShape::Rhombus; }
	else if (rest.starts_with(L"[")) { open = L"["; close = L"]"; node->shape = NodeShape::Rectangle; }
	else return false;

	if (!rest.ends_with(close)) return false;
	node->label.assign(Unquote(rest.substr(open.size(), rest.size() - open.size() - close.size())));
	if (node->label.empty()) node->label = node->id;
	node->labelled = true;
	return true;
}

//! One entry in the layered graph, either a real node or an edge waypoint.
struct LayoutEntry {
	std::size_t node = kNoIndex;  //!< Index into Diagram::nodes, or kNoIndex for a waypoint
	int rank = 0;
	//! Extent across the rank and along the flow, before direction mapping.
	int breadth = 0;
	int depth = 0;
	double position = 0.0;  //!< Centre along the breadth axis
	int offset = 0;         //!< Start along the depth axis
};

[[nodiscard]] bool ParseDirection(std::wstring_view value, Direction* direction) noexcept
{
	if (value == L"TD" || value == L"TB" || value.empty()) { *direction = Direction::TopDown; return true; }
	if (value == L"BT") { *direction = Direction::BottomUp; return true; }
	if (value == L"LR") { *direction = Direction::LeftRight; return true; }
	if (value == L"RL") { *direction = Direction::RightLeft; return true; }
	return false;
}

/*!
	@brief Keywords this module deliberately refuses

	Each of these changes what the diagram means. Drawing the graph while
	silently ignoring them would produce a picture the author did not write, so
	the whole diagram falls back to literal source instead.
*/
[[nodiscard]] bool IsUnsupportedKeyword(std::wstring_view word) noexcept
{
	return word == L"subgraph" || word == L"end" || word == L"click" || word == L"style"
		|| word == L"classDef" || word == L"class" || word == L"linkStyle"
		|| word == L"direction" || word == L"accTitle" || word == L"accDescr";
}

} // namespace

BuildOutcome BuildFlowchart(std::wstring_view source, const LayoutMetrics& metrics,
	const BuildLimits& limits, const MeasureText& measure, Diagram* diagram)
{
	if (diagram == nullptr || !measure) return BuildOutcome::UnsupportedSyntax;

	const auto statements = SplitStatements(source);
	if (statements.empty()) return BuildOutcome::UnsupportedSyntax;
	if (statements.size() > limits.maximumStatements) return BuildOutcome::LimitExceeded;

	const auto header = statements.front();
	const auto keyword = FirstWord(header);
	if (keyword != L"graph" && keyword != L"flowchart") return BuildOutcome::UnsupportedSyntax;
	Direction direction = Direction::TopDown;
	if (!ParseDirection(Trim(header.substr(keyword.size())), &direction)) {
		return BuildOutcome::UnsupportedSyntax;
	}

	Diagram built;
	built.direction = direction;
	std::unordered_map<std::wstring, std::size_t> byId;

	const auto intern = [&built, &byId, &limits](const ParsedNode& parsed, bool* overflow) {
		const auto existing = byId.find(parsed.id);
		if (existing != byId.end()) {
			auto& node = built.nodes[existing->second];
			// A later statement may be the one that carries the shape and label.
			if (parsed.labelled) {
				node.label = parsed.label;
				node.shape = parsed.shape;
			}
			return existing->second;
		}
		if (built.nodes.size() >= limits.maximumNodes) { *overflow = true; return kNoIndex; }
		DiagramNode node;
		node.id = parsed.id;
		node.label = parsed.label;
		node.shape = parsed.shape;
		built.nodes.push_back(std::move(node));
		const auto index = built.nodes.size() - 1;
		byId.emplace(parsed.id, index);
		return index;
	};

	bool overflow = false;
	for (std::size_t statementIndex = 1; statementIndex < statements.size(); ++statementIndex) {
		const auto statement = statements[statementIndex];
		if (IsUnsupportedKeyword(FirstWord(statement))) return BuildOutcome::UnsupportedSyntax;

		std::size_t cursor = 0;
		std::size_t previous = kNoIndex;
		EdgeToken pendingLink;
		bool hasPendingLink = false;
		while (true) {
			EdgeToken token;
			bool malformed = false;
			const bool hasEdge = FindEdge(statement, cursor, &token, &malformed);
			if (malformed) return BuildOutcome::UnsupportedSyntax;
			const auto chunk = hasEdge
				? statement.substr(cursor, token.start - cursor)
				: statement.substr(cursor);
			ParsedNode parsed;
			if (!ParseNodeChunk(chunk, &parsed)) {
				// A statement that is neither a node nor a link is a feature this
				// module does not model, so the whole diagram falls back.
				return BuildOutcome::UnsupportedSyntax;
			}
			const auto current = intern(parsed, &overflow);
			if (overflow) return BuildOutcome::LimitExceeded;
			if (hasPendingLink && previous != kNoIndex) {
				if (built.edges.size() >= limits.maximumEdges) return BuildOutcome::LimitExceeded;
				DiagramEdge edge;
				edge.from = previous;
				edge.to = current;
				edge.style = pendingLink.style;
				edge.arrow = pendingLink.arrow;
				edge.label = pendingLink.label;
				built.edges.push_back(std::move(edge));
			}
			previous = current;
			hasPendingLink = false;
			if (!hasEdge) break;
			// The operator's style and label belong to the link that follows it.
			pendingLink = token;
			hasPendingLink = true;
			cursor = token.end;
		}
		if (hasPendingLink) return BuildOutcome::UnsupportedSyntax;
	}
	if (built.nodes.empty()) return BuildOutcome::UnsupportedSyntax;

	// ---------------------------------------------------------------- sizing
	for (auto& node : built.nodes) {
		const auto textWidth = std::max(0, measure(node.label));
		node.width = std::max(metrics.minimumNodeWidth, textWidth + 2 * metrics.nodePaddingX);
		node.height = metrics.lineHeight + 2 * metrics.nodePaddingY;
		if (node.shape == NodeShape::Rhombus) {
			// A diamond only shows its label if the box around it grows.
			node.width += metrics.nodePaddingX * 2;
			node.height += metrics.nodePaddingY * 2;
		} else if (node.shape == NodeShape::Circle) {
			const auto side = std::max(node.width, node.height + metrics.nodePaddingY * 2);
			node.width = side;
			node.height = side;
		} else if (node.shape == NodeShape::Hexagon) {
			node.width += metrics.nodePaddingX;
		}
	}

	// ------------------------------------------------------------ ranking
	// Longest path from the sources, over the graph with its back edges removed.
	// Back edges are found by a depth-first sweep, which is what dagre does
	// before it ranks.
	const auto nodeCount = built.nodes.size();
	std::vector<std::vector<std::size_t>> outgoing(nodeCount);
	for (std::size_t edgeIndex = 0; edgeIndex < built.edges.size(); ++edgeIndex) {
		outgoing[built.edges[edgeIndex].from].push_back(edgeIndex);
	}
	std::vector<char> state(nodeCount, 0);  // 0 unvisited, 1 on stack, 2 done
	std::vector<char> backEdge(built.edges.size(), 0);
	std::vector<std::pair<std::size_t, std::size_t>> stack;
	for (std::size_t root = 0; root < nodeCount; ++root) {
		if (state[root] != 0) continue;
		stack.push_back({ root, 0 });
		state[root] = 1;
		while (!stack.empty()) {
			auto& frame = stack.back();
			if (frame.second >= outgoing[frame.first].size()) {
				state[frame.first] = 2;
				stack.pop_back();
				continue;
			}
			const auto edgeIndex = outgoing[frame.first][frame.second++];
			const auto target = built.edges[edgeIndex].to;
			if (state[target] == 1) {
				backEdge[edgeIndex] = 1;
			} else if (state[target] == 0) {
				state[target] = 1;
				stack.push_back({ target, 0 });
			}
		}
	}

	std::vector<int> rank(nodeCount, 0);
	std::vector<std::size_t> pending(nodeCount, 0);
	for (std::size_t edgeIndex = 0; edgeIndex < built.edges.size(); ++edgeIndex) {
		if (!backEdge[edgeIndex]) ++pending[built.edges[edgeIndex].to];
	}
	std::vector<std::size_t> ready;
	for (std::size_t node = 0; node < nodeCount; ++node) {
		if (pending[node] == 0) ready.push_back(node);
	}
	for (std::size_t head = 0; head < ready.size(); ++head) {
		const auto node = ready[head];
		for (const auto edgeIndex : outgoing[node]) {
			if (backEdge[edgeIndex]) continue;
			const auto target = built.edges[edgeIndex].to;
			rank[target] = std::max(rank[target], rank[node] + 1);
			if (--pending[target] == 0) ready.push_back(target);
		}
	}

	// -------------------------------------------------- layered entries
	std::vector<LayoutEntry> entries;
	entries.reserve(nodeCount + built.edges.size());
	std::vector<std::size_t> entryOfNode(nodeCount, kNoIndex);
	const bool horizontal = direction == Direction::LeftRight || direction == Direction::RightLeft;
	for (std::size_t node = 0; node < nodeCount; ++node) {
		LayoutEntry entry;
		entry.node = node;
		entry.rank = rank[node];
		entry.breadth = horizontal ? built.nodes[node].height : built.nodes[node].width;
		entry.depth = horizontal ? built.nodes[node].width : built.nodes[node].height;
		entries.push_back(entry);
		entryOfNode[node] = entries.size() - 1;
	}

	// Waypoints for edges that skip ranks, so ordering and routing both see them.
	std::vector<std::vector<std::size_t>> waypoints(built.edges.size());
	for (std::size_t edgeIndex = 0; edgeIndex < built.edges.size(); ++edgeIndex) {
		const auto& edge = built.edges[edgeIndex];
		for (int level = rank[edge.from] + 1; level < rank[edge.to]; ++level) {
			LayoutEntry entry;
			entry.rank = level;
			entry.breadth = 1;
			entry.depth = 1;
			entries.push_back(entry);
			waypoints[edgeIndex].push_back(entries.size() - 1);
		}
	}

	int maximumRank = 0;
	for (const auto& entry : entries) maximumRank = std::max(maximumRank, entry.rank);
	std::vector<std::vector<std::size_t>> layers(static_cast<std::size_t>(maximumRank) + 1);
	for (std::size_t index = 0; index < entries.size(); ++index) {
		layers[static_cast<std::size_t>(entries[index].rank)].push_back(index);
	}

	// Adjacency between layered entries, following the waypoint chains.
	std::vector<std::vector<std::size_t>> up(entries.size());
	std::vector<std::vector<std::size_t>> down(entries.size());
	const auto link = [&up, &down](std::size_t a, std::size_t b) {
		down[a].push_back(b);
		up[b].push_back(a);
	};
	for (std::size_t edgeIndex = 0; edgeIndex < built.edges.size(); ++edgeIndex) {
		const auto& edge = built.edges[edgeIndex];
		auto previous = entryOfNode[edge.from];
		for (const auto waypoint : waypoints[edgeIndex]) {
			link(previous, waypoint);
			previous = waypoint;
		}
		const auto target = entryOfNode[edge.to];
		if (rank[edge.to] > rank[edge.from]) link(previous, target);
		else link(target, previous);
	}

	// ------------------------------------------------------------ ordering
	// Barycentre sweeps, alternating direction, exactly as dagre's order phase.
	std::vector<double> order(entries.size(), 0.0);
	for (auto& layer : layers) {
		for (std::size_t position = 0; position < layer.size(); ++position) {
			order[layer[position]] = static_cast<double>(position);
		}
	}
	for (int sweep = 0; sweep < 8; ++sweep) {
		const bool downward = (sweep % 2) == 0;
		for (std::size_t layerIndex = 0; layerIndex < layers.size(); ++layerIndex) {
			const auto index = downward ? layerIndex : layers.size() - 1 - layerIndex;
			auto& layer = layers[index];
			std::vector<double> barycenter(layer.size(), 0.0);
			for (std::size_t position = 0; position < layer.size(); ++position) {
				const auto& neighbours = downward ? up[layer[position]] : down[layer[position]];
				if (neighbours.empty()) {
					barycenter[position] = order[layer[position]];
					continue;
				}
				double total = 0.0;
				for (const auto neighbour : neighbours) total += order[neighbour];
				barycenter[position] = total / static_cast<double>(neighbours.size());
			}
			std::vector<std::size_t> permutation(layer.size());
			for (std::size_t position = 0; position < layer.size(); ++position) permutation[position] = position;
			std::stable_sort(permutation.begin(), permutation.end(),
				[&barycenter](std::size_t a, std::size_t b) { return barycenter[a] < barycenter[b]; });
			std::vector<std::size_t> reordered(layer.size());
			for (std::size_t position = 0; position < layer.size(); ++position) {
				reordered[position] = layer[permutation[position]];
			}
			layer = std::move(reordered);
			for (std::size_t position = 0; position < layer.size(); ++position) {
				order[layer[position]] = static_cast<double>(position);
			}
		}
	}

	// -------------------------------------------------------- coordinates
	int depthCursor = 0;
	for (auto& layer : layers) {
		int thickest = 0;
		for (const auto index : layer) thickest = std::max(thickest, entries[index].depth);
		for (const auto index : layer) {
			// Centre a short node inside a rank made tall by a taller sibling.
			entries[index].offset = depthCursor + (thickest - entries[index].depth) / 2;
		}
		depthCursor += thickest + metrics.rankSeparation;
	}
	const auto totalDepth = std::max(0, depthCursor - metrics.rankSeparation);

	const auto pack = [&entries, &metrics](std::vector<std::size_t>& layer) {
		double cursor = 0.0;
		for (const auto index : layer) {
			const auto half = entries[index].breadth / 2.0;
			cursor = std::max(cursor + half, entries[index].position);
			entries[index].position = cursor;
			cursor += half + metrics.nodeSeparation;
		}
	};
	for (auto& layer : layers) {
		for (const auto index : layer) entries[index].position = 0.0;
		pack(layer);
	}
	// Pull each entry toward its neighbours, then re-pack so the order survives.
	for (int iteration = 0; iteration < 6; ++iteration) {
		const bool downward = (iteration % 2) == 0;
		for (std::size_t layerIndex = 0; layerIndex < layers.size(); ++layerIndex) {
			const auto index = downward ? layerIndex : layers.size() - 1 - layerIndex;
			auto& layer = layers[index];
			for (const auto entryIndex : layer) {
				const auto& neighbours = downward ? up[entryIndex] : down[entryIndex];
				if (neighbours.empty()) continue;
				double total = 0.0;
				for (const auto neighbour : neighbours) total += entries[neighbour].position;
				entries[entryIndex].position = total / static_cast<double>(neighbours.size());
			}
			std::stable_sort(layer.begin(), layer.end(),
				[&entries](std::size_t a, std::size_t b) { return entries[a].position < entries[b].position; });
			pack(layer);
		}
	}

	double minimumPosition = std::numeric_limits<double>::max();
	double maximumPosition = std::numeric_limits<double>::lowest();
	for (const auto& entry : entries) {
		minimumPosition = std::min(minimumPosition, entry.position - entry.breadth / 2.0);
		maximumPosition = std::max(maximumPosition, entry.position + entry.breadth / 2.0);
	}
	if (minimumPosition > maximumPosition) { minimumPosition = 0.0; maximumPosition = 0.0; }
	for (auto& entry : entries) entry.position -= minimumPosition;
	const auto totalBreadth = static_cast<int>(maximumPosition - minimumPosition + 0.5);

	// The layout is computed along abstract breadth/depth axes; only here does it
	// become a direction.
	const auto placeX = [&](const LayoutEntry& entry, int extentAcross, int extentAlong) {
		switch (direction) {
		case Direction::TopDown:
		case Direction::BottomUp:
			return static_cast<int>(entry.position + 0.5) - extentAcross / 2;
		case Direction::LeftRight:
			return entry.offset;
		case Direction::RightLeft:
			return totalDepth - entry.offset - extentAlong;
		}
		return 0;
	};
	const auto placeY = [&](const LayoutEntry& entry, int extentAcross, int extentAlong) {
		switch (direction) {
		case Direction::TopDown:
			return entry.offset;
		case Direction::BottomUp:
			return totalDepth - entry.offset - extentAlong;
		case Direction::LeftRight:
		case Direction::RightLeft:
			return static_cast<int>(entry.position + 0.5) - extentAcross / 2;
		}
		return 0;
	};
	for (const auto& entry : entries) {
		if (entry.node == kNoIndex) continue;
		auto& node = built.nodes[entry.node];
		node.x = placeX(entry, entry.breadth, entry.depth);
		node.y = placeY(entry, entry.breadth, entry.depth);
	}

	built.width = horizontal ? totalDepth : totalBreadth;
	built.height = horizontal ? totalBreadth : totalDepth;

	// ------------------------------------------------------------- routing
	const auto exitAnchor = [&](const DiagramNode& node) {
		switch (direction) {
		case Direction::TopDown: return DiagramPoint{ node.x + node.width / 2, node.y + node.height };
		case Direction::BottomUp: return DiagramPoint{ node.x + node.width / 2, node.y };
		case Direction::LeftRight: return DiagramPoint{ node.x + node.width, node.y + node.height / 2 };
		case Direction::RightLeft: return DiagramPoint{ node.x, node.y + node.height / 2 };
		}
		return DiagramPoint{};
	};
	const auto entryAnchor = [&](const DiagramNode& node) {
		switch (direction) {
		case Direction::TopDown: return DiagramPoint{ node.x + node.width / 2, node.y };
		case Direction::BottomUp: return DiagramPoint{ node.x + node.width / 2, node.y + node.height };
		case Direction::LeftRight: return DiagramPoint{ node.x, node.y + node.height / 2 };
		case Direction::RightLeft: return DiagramPoint{ node.x + node.width, node.y + node.height / 2 };
		}
		return DiagramPoint{};
	};
	for (std::size_t edgeIndex = 0; edgeIndex < built.edges.size(); ++edgeIndex) {
		auto& edge = built.edges[edgeIndex];
		const auto& fromNode = built.nodes[edge.from];
		const auto& toNode = built.nodes[edge.to];
		const bool forward = rank[edge.to] > rank[edge.from];
		edge.points.clear();
		edge.points.push_back(forward ? exitAnchor(fromNode) : entryAnchor(fromNode));
		for (const auto waypoint : waypoints[edgeIndex]) {
			const auto& entry = entries[waypoint];
			edge.points.push_back({ placeX(entry, entry.breadth, entry.depth),
				placeY(entry, entry.breadth, entry.depth) });
		}
		edge.points.push_back(forward ? entryAnchor(toNode) : exitAnchor(toNode));
		const auto middle = edge.points.size() / 2;
		const auto& a = edge.points[middle - 1];
		const auto& b = edge.points[middle];
		edge.labelCenter = { (a.x + b.x) / 2, (a.y + b.y) / 2 };
	}

	*diagram = std::move(built);
	return BuildOutcome::Supported;
}

} // namespace markdown::mermaid
