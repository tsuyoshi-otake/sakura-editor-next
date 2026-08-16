#include "StdAfx.h"
#include "terminal/window/TerminalPaneLayout.h"

#include <algorithm>
#include <cstdint>
#include <limits>

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

	std::int64_t paneWidth = width;
	const auto tabsDividerWidth = std::max(1, Dip(1, input.dpi));
	if( input.showTabs && width >= static_cast<std::int64_t>(terminalMinimum) + listMinimum + tabsDividerWidth ) {
		const auto listWidth = std::min<std::int64_t>(preferredList, width - terminalMinimum - tabsDividerWidth);
		const auto listLeft = static_cast<std::int64_t>(input.content.right) - listWidth;
		result.tabsBounds = { static_cast<LONG>(listLeft), input.content.top, input.content.right, input.content.bottom };
		const auto dividerWidth = std::min<std::int64_t>(tabsDividerWidth, listWidth);
		result.tabsDivider = { static_cast<LONG>(listLeft - dividerWidth), input.content.top,
			static_cast<LONG>(listLeft), input.content.bottom };
		paneWidth = listLeft - dividerWidth - input.content.left;
	}
	result.panesBounds = { input.content.left, input.content.top, static_cast<LONG>(input.content.left + paneWidth), input.content.bottom };

	const auto paneCount = input.paneCount;
	const auto dividerCount = paneCount > 1 ? paneCount - 1 : 0;
	const bool vertical = input.orientation == TerminalPaneOrientation::Vertical;
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
	std::int64_t cursor = vertical ? input.content.top : input.content.left;
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

} // namespace terminal
