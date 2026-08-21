#include "StdAfx.h"
#include "terminal/window/TerminalPaneLayout.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>

namespace terminal {
namespace {

int Dip( const int dip, const unsigned int dpi ) noexcept
{
	const auto effectiveDpi = dpi == 0 ? 96u : dpi;
	const auto scaled = (static_cast<std::int64_t>(dip) * effectiveDpi + 48) / 96;
	return static_cast<int>(std::clamp<std::int64_t>(scaled, 0, std::numeric_limits<int>::max()));
}

bool IsValidContent( const RECT& rect ) noexcept
{
	return rect.right >= rect.left && rect.bottom >= rect.top;
}

std::int64_t ProportionalExtent( std::int64_t total, std::int64_t numerator, std::int64_t denominator ) noexcept
{
	if( total <= 0 || numerator <= 0 || denominator <= 0 ) return 0;
	const auto quotient = total / denominator;
	const auto remainder = total % denominator;
	const auto whole = quotient * numerator;
	if( remainder <= std::numeric_limits<std::int64_t>::max() / numerator ) {
		return whole + remainder * numerator / denominator;
	}
	return whole + static_cast<std::int64_t>(static_cast<long double>(remainder) * numerator / denominator);
}

} // namespace

TerminalPaneLayoutResult CalculateTerminalPaneLayout( const TerminalPaneLayoutInput& input ) noexcept
{
	TerminalPaneLayoutResult result;
	result.orientation = input.orientation;
	if( input.paneCount == 0 || !IsValidContent(input.content) ) return result;

	const auto width = static_cast<std::int64_t>(input.content.right) - input.content.left;
	const auto height = static_cast<std::int64_t>(input.content.bottom) - input.content.top;
	// VS Code's normal terminal tabs list defaults to 120 px and may shrink to
	// 80 px.  The list stays on the right in the default Panel layout for both
	// horizontal and vertical pane orientations.
	const auto listMinimum = Dip(80, input.dpi);
	const auto terminalMinimum = Dip(80, input.dpi);
	const auto preferredList = Dip(120, input.dpi);
	const auto dividerDefault = Dip(4, input.dpi);

	std::int64_t paneLeft = input.content.left;
	std::int64_t paneRight = input.content.right;
	const auto tabsDividerWidth = std::max(1, Dip(1, input.dpi));
	if( input.showTabs && width >= static_cast<std::int64_t>(terminalMinimum) + listMinimum + tabsDividerWidth ) {
		const auto listWidth = std::min<std::int64_t>(preferredList, width - terminalMinimum - tabsDividerWidth);
		const auto dividerWidth = std::min<std::int64_t>(tabsDividerWidth, listWidth);
		if( input.tabsLocation == TerminalTabsLocation::Left ) {
			const auto listRight = static_cast<std::int64_t>(input.content.left) + listWidth;
			result.tabsBounds = { input.content.left, input.content.top,
				static_cast<LONG>(listRight), input.content.bottom };
			result.tabsDivider = { static_cast<LONG>(listRight), input.content.top,
				static_cast<LONG>(listRight + dividerWidth), input.content.bottom };
			paneLeft = listRight + dividerWidth;
		} else {
			const auto listLeft = static_cast<std::int64_t>(input.content.right) - listWidth;
			result.tabsBounds = { static_cast<LONG>(listLeft), input.content.top, input.content.right, input.content.bottom };
			result.tabsDivider = { static_cast<LONG>(listLeft - dividerWidth), input.content.top,
				static_cast<LONG>(listLeft), input.content.bottom };
			paneRight = listLeft - dividerWidth;
		}
	}
	result.panesBounds = { static_cast<LONG>(paneLeft), input.content.top,
		static_cast<LONG>(paneRight), input.content.bottom };

	const auto paneCount = input.paneCount;
	const auto dividerCount = paneCount > 1 ? paneCount - 1 : 0;
	const bool vertical = input.orientation == TerminalPaneOrientation::Vertical;
	const auto paneWidth = paneRight - paneLeft;
	const auto primaryExtent = vertical ? height : paneWidth;
	const auto totalDivider = std::min<std::int64_t>(primaryExtent, static_cast<std::int64_t>(dividerCount) * dividerDefault);
	const auto baseDivider = dividerCount == 0 ? 0 : totalDivider / static_cast<std::int64_t>(dividerCount);
	const auto extraDivider = dividerCount == 0 ? 0 : totalDivider % static_cast<std::int64_t>(dividerCount);
	const auto available = primaryExtent - totalDivider;

	bool weighted = input.paneWeights.size() >= paneCount;
	std::int64_t weightSum = 0;
	if( weighted ) {
		for( std::size_t i = 0; i < paneCount; ++i ) {
			if( input.paneWeights[i] <= 0 || weightSum > std::numeric_limits<std::int64_t>::max() - input.paneWeights[i] ) {
				weighted = false;
				break;
			}
			weightSum += input.paneWeights[i];
		}
		if( weightSum <= 0 ) weighted = false;
	}

	result.panes.reserve(paneCount);
	result.paneDividers.reserve(dividerCount);
	const auto minimumPane = static_cast<std::int64_t>(Dip(80, input.dpi));
	const bool canHonorMinimum = minimumPane > 0
		&& paneCount <= static_cast<std::size_t>(available / minimumPane);
	const auto basePane = canHonorMinimum ? minimumPane : 0;
	const auto distributable = available - basePane * static_cast<std::int64_t>(paneCount);
	std::int64_t cursor = vertical ? input.content.top : paneLeft;
	std::int64_t allocated = 0;
	std::int64_t allocatedWeight = 0;
	for( std::size_t i = 0; i < paneCount; ++i ) {
		std::int64_t paneExtent = distributable - allocated;
		if( i + 1 < paneCount ) {
			if( weighted ) {
				allocatedWeight += input.paneWeights[i];
				paneExtent = ProportionalExtent(distributable, allocatedWeight, weightSum) - allocated;
			} else {
				paneExtent = ProportionalExtent(distributable, static_cast<std::int64_t>(i + 1),
					static_cast<std::int64_t>(paneCount)) - allocated;
			}
		}
		paneExtent = std::max<std::int64_t>(0, paneExtent);
		const auto fullExtent = basePane + paneExtent;
		if( vertical ) {
			result.panes.push_back({ result.panesBounds.left, static_cast<LONG>(cursor),
				result.panesBounds.right, static_cast<LONG>(cursor + fullExtent) });
		} else {
			result.panes.push_back({ static_cast<LONG>(cursor), result.panesBounds.top,
				static_cast<LONG>(cursor + fullExtent), result.panesBounds.bottom });
		}
		cursor += fullExtent;
		allocated += paneExtent;
		if( i + 1 < paneCount ) {
			const auto divider = baseDivider + (static_cast<std::int64_t>(i) < extraDivider ? 1 : 0);
			if( vertical ) {
				result.paneDividers.push_back({ result.panesBounds.left, static_cast<LONG>(cursor),
					result.panesBounds.right, static_cast<LONG>(cursor + divider) });
			} else {
				result.paneDividers.push_back({ static_cast<LONG>(cursor), result.panesBounds.top,
					static_cast<LONG>(cursor + divider), result.panesBounds.bottom });
			}
			cursor += divider;
		}
	}
	return result;
}

namespace {

std::int64_t MinimumTreeExtent( std::span<const TerminalPaneLayoutTreeNode> nodes,
	std::size_t nodeIndex, bool vertical, unsigned int dpi ) noexcept
{
	const auto minimumPane = static_cast<std::int64_t>(Dip(80, dpi));
	if( nodeIndex >= nodes.size() ) return minimumPane;
	const auto& node = nodes[nodeIndex];
	if( node.children.empty() ) return minimumPane;

	const bool splitsAlongAxis = node.orientation
		== (vertical ? TerminalPaneOrientation::Vertical : TerminalPaneOrientation::Horizontal);
	std::int64_t extent = 0;
	for( const auto child : node.children ) {
		const auto childExtent = MinimumTreeExtent(nodes, child, vertical, dpi);
		if( splitsAlongAxis ) {
			extent = std::min<std::int64_t>(std::numeric_limits<std::int64_t>::max() - childExtent,
				extent) + childExtent;
		} else {
			extent = std::max(extent, childExtent);
		}
	}
	if( splitsAlongAxis && node.children.size() > 1 ) {
		const auto divider = static_cast<std::int64_t>(Dip(4, dpi));
		const auto dividerCount = static_cast<std::int64_t>(node.children.size() - 1);
		if( dividerCount > 0 && extent <= std::numeric_limits<std::int64_t>::max() - divider * dividerCount ) {
			extent += divider * dividerCount;
		} else {
			extent = std::numeric_limits<std::int64_t>::max();
		}
	}
	return extent;
}

} // namespace

TerminalPaneLayoutResult CalculateTerminalPaneTreeLayout( const TerminalPaneTreeLayoutInput& input ) noexcept
{
	TerminalPaneLayoutResult result;
	if( input.nodes.empty() || input.root >= input.nodes.size() || !IsValidContent(input.content) ) return result;

	const auto width = static_cast<std::int64_t>(input.content.right) - input.content.left;
	const auto listMinimum = Dip(80, input.dpi);
	const auto terminalMinimum = Dip(80, input.dpi);
	const auto preferredList = Dip(120, input.dpi);
	const auto tabsDividerWidth = std::max(1, Dip(1, input.dpi));
	std::int64_t paneLeft = input.content.left;
	std::int64_t paneRight = input.content.right;
	if( input.showTabs && width >= static_cast<std::int64_t>(terminalMinimum) + listMinimum + tabsDividerWidth ) {
		const auto listWidth = std::min<std::int64_t>(preferredList, width - terminalMinimum - tabsDividerWidth);
		const auto dividerWidth = std::min<std::int64_t>(tabsDividerWidth, listWidth);
		if( input.tabsLocation == TerminalTabsLocation::Left ) {
			const auto listRight = static_cast<std::int64_t>(input.content.left) + listWidth;
			result.tabsBounds = { input.content.left, input.content.top,
				static_cast<LONG>(listRight), input.content.bottom };
			result.tabsDivider = { static_cast<LONG>(listRight), input.content.top,
				static_cast<LONG>(listRight + dividerWidth), input.content.bottom };
			paneLeft = listRight + dividerWidth;
		} else {
			const auto listLeft = static_cast<std::int64_t>(input.content.right) - listWidth;
			result.tabsBounds = { static_cast<LONG>(listLeft), input.content.top, input.content.right, input.content.bottom };
			result.tabsDivider = { static_cast<LONG>(listLeft - dividerWidth), input.content.top,
				static_cast<LONG>(listLeft), input.content.bottom };
			paneRight = listLeft - dividerWidth;
		}
	}
	result.panesBounds = { static_cast<LONG>(paneLeft), input.content.top,
		static_cast<LONG>(paneRight), input.content.bottom };
	result.orientation = input.nodes[input.root].orientation;

	const auto dividerDefault = static_cast<std::int64_t>(Dip(4, input.dpi));
	const auto layoutNode = [&](auto&& self, std::size_t nodeIndex, const RECT& bounds) -> void {
		if( nodeIndex >= input.nodes.size() ) return;
		const auto& node = input.nodes[nodeIndex];
		if( node.children.empty() ) {
			result.panes.push_back(bounds);
			return;
		}
		if( node.children.size() == 1 ) {
			self(self, node.children.front(), bounds);
			return;
		}

		const bool vertical = node.orientation == TerminalPaneOrientation::Vertical;
		const auto primaryExtent = vertical
			? static_cast<std::int64_t>(bounds.bottom) - bounds.top
			: static_cast<std::int64_t>(bounds.right) - bounds.left;
		const auto dividerCount = static_cast<std::int64_t>(node.children.size() - 1);
		const auto totalDivider = std::min<std::int64_t>(primaryExtent, dividerCount * dividerDefault);
		const auto baseDivider = dividerCount == 0 ? 0 : totalDivider / dividerCount;
		const auto extraDivider = dividerCount == 0 ? 0 : totalDivider % dividerCount;
		const auto available = primaryExtent - totalDivider;

		bool weighted = node.weights.size() == node.children.size();
		std::int64_t weightSum = 0;
		if( weighted ) {
			for( const auto weight : node.weights ) {
				if( weight <= 0 || weightSum > std::numeric_limits<std::int64_t>::max() - weight ) {
					weighted = false;
					break;
				}
				weightSum += weight;
			}
			if( weightSum <= 0 ) weighted = false;
		}

		std::vector<std::int64_t> minimums;
		minimums.reserve(node.children.size());
		std::int64_t totalMinimum = 0;
		for( const auto child : node.children ) {
			const auto minimum = MinimumTreeExtent(input.nodes, child, vertical, input.dpi);
			minimums.push_back(minimum);
			if( totalMinimum <= std::numeric_limits<std::int64_t>::max() - minimum ) totalMinimum += minimum;
			else totalMinimum = std::numeric_limits<std::int64_t>::max();
		}
		const bool canHonorMinimum = totalMinimum <= available;
		const auto distributable = canHonorMinimum ? available - totalMinimum : available;
		const auto denominator = weighted ? weightSum : static_cast<std::int64_t>(node.children.size());
		std::vector<RECT> childBounds;
		childBounds.reserve(node.children.size());
		std::int64_t cursor = vertical ? bounds.top : bounds.left;
		std::int64_t allocated = 0;
		for( std::size_t index = 0; index < node.children.size(); ++index ) {
			std::int64_t variableExtent = distributable - allocated;
			if( index + 1 < node.children.size() ) {
				const auto numerator = weighted
					? std::accumulate(node.weights.begin(), node.weights.begin() + index + 1, std::int64_t{})
					: static_cast<std::int64_t>(index + 1);
				variableExtent = ProportionalExtent(distributable, numerator, denominator) - allocated;
			}
			variableExtent = std::max<std::int64_t>(0, variableExtent);
			const auto fullExtent = (canHonorMinimum ? minimums[index] : 0) + variableExtent;
			if( vertical ) {
				childBounds.push_back({ bounds.left, static_cast<LONG>(cursor), bounds.right,
					static_cast<LONG>(cursor + fullExtent) });
			} else {
				childBounds.push_back({ static_cast<LONG>(cursor), bounds.top,
					static_cast<LONG>(cursor + fullExtent), bounds.bottom });
			}
			cursor += fullExtent;
			allocated += variableExtent;
			if( index + 1 < node.children.size() ) {
				cursor += baseDivider + (static_cast<std::int64_t>(index) < extraDivider ? 1 : 0);
			}
		}

		for( std::size_t index = 0; index < node.children.size(); ++index ) {
			self(self, node.children[index], childBounds[index]);
			if( index + 1 >= node.children.size() ) continue;
			RECT divider{};
			if( vertical ) {
				divider = { bounds.left, childBounds[index].bottom, bounds.right, childBounds[index + 1].top };
			} else {
				divider = { childBounds[index].right, bounds.top, childBounds[index + 1].left, bounds.bottom };
			}
			result.paneDividers.push_back(divider);
			result.paneDividerNodes.push_back(nodeIndex);
			result.paneDividerChildren.push_back(index);
			result.paneDividerFirstPanes.push_back(childBounds[index]);
			result.paneDividerSecondPanes.push_back(childBounds[index + 1]);
		}
	};

	layoutNode(layoutNode, input.root, result.panesBounds);
	return result;
}

} // namespace terminal
