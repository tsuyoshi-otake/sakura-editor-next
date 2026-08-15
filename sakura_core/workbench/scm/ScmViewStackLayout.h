/*! @file
 * @brief Pure geometry and capability state for the native Source Control View stack.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <algorithm>
#include <cstdint>

namespace workbench::scm {

//! The native SCM host has no history provider yet.  Keeping this a typed state
//! prevents the Graph scaffold from being mistaken for a functional Git graph.
enum class EScmGraphPresentationStatus : std::uint8_t {
	Unsupported,
};

//! Presentation contract for the `workbench.scm.history` frame.
//!
//! A future history-provider snapshot may add a supported state here.  Until it
//! does, the frame owns neither graph rows nor commands and is never interactive.
struct ScmGraphPresentation final {
	EScmGraphPresentationStatus status{ EScmGraphPresentationStatus::Unsupported };

	[[nodiscard]] constexpr bool IsInteractive() const noexcept
	{
		switch (status) {
		case EScmGraphPresentationStatus::Unsupported: return false;
		}
		return false;
	}

	//! A Graph View belongs to a Source Control provider.  The native host has no
	//! history-provider capability yet, but it can show the requested frame only
	//! while an actual repository provider is present.
	[[nodiscard]] constexpr bool ShouldRenderFrameForProvider(bool repositoryAvailable) const noexcept
	{
		return repositoryAvailable;
	}

	[[nodiscard]] constexpr bool operator==(const ScmGraphPresentation&) const = default;
};

//! One vertical span in a View stack.  Empty spans are valid when the host is
//! shorter than its chrome; consumers must then simply avoid drawing them.
struct ScmVerticalBounds final {
	int top{};
	int bottom{};

	[[nodiscard]] constexpr bool Empty() const noexcept { return bottom <= top; }
	[[nodiscard]] constexpr bool operator==(const ScmVerticalBounds&) const = default;
};

//! Scaled native measurements fed into the pure View-stack projection.
struct ScmViewStackMeasurements final {
	int clientTop{};
	int clientBottom{};
	int viewHeaderHeight{};
	int repositoryRowHeight{};
	int inputOuterMargin{};
	int inputHeight{};
	int graphBodyHeight{};
	bool repositoriesVisible{};
	bool inputVisible{};
	bool graphVisible{};

	[[nodiscard]] constexpr bool operator==(const ScmViewStackMeasurements&) const = default;
};

//! The three sibling SCM views projected into the single native SCM HWND.
struct ScmViewStackLayout final {
	ScmVerticalBounds repositoriesHeader;
	ScmVerticalBounds repositoryRow;
	ScmVerticalBounds changesHeader;
	ScmVerticalBounds input;
	ScmVerticalBounds changesBody;
	ScmVerticalBounds graphHeader;
	ScmVerticalBounds graphBody;

	[[nodiscard]] constexpr bool operator==(const ScmViewStackLayout&) const = default;
};

//! Builds the non-overlapping vertical layout for Repositories, Changes, and
//! Graph.  The Graph frame reserves its own lower area before the Changes list
//! / welcome body is handed to a child HWND, so the two can never overpaint.
[[nodiscard]] constexpr ScmViewStackLayout BuildScmViewStackLayout(
	ScmViewStackMeasurements measurements) noexcept
{
	const int clientTop = measurements.clientTop;
	const int clientBottom = std::max(clientTop, measurements.clientBottom);
	const int headerHeight = std::max(0, measurements.viewHeaderHeight);
	const int repositoryRowHeight = std::max(0, measurements.repositoryRowHeight);
	const int inputOuterMargin = std::max(0, measurements.inputOuterMargin);
	const int inputHeight = std::max(0, measurements.inputHeight);
	const int graphBodyHeight = std::max(0, measurements.graphBodyHeight);

	int cursor = clientTop;
	const auto consume = [&cursor, clientBottom](int height) constexpr {
		const int top = cursor;
		cursor = std::min(clientBottom, cursor + std::max(0, height));
		return ScmVerticalBounds{ top, cursor };
	};

	ScmViewStackLayout layout;
	if (measurements.repositoriesVisible) {
		layout.repositoriesHeader = consume(headerHeight);
		layout.repositoryRow = consume(repositoryRowHeight);
	}
	layout.changesHeader = consume(headerHeight);
	if (measurements.inputVisible) {
		(void)consume(inputOuterMargin);
		layout.input = consume(inputHeight);
		(void)consume(inputOuterMargin);
	} else {
		layout.input = { cursor, cursor };
	}

	const int graphReserve = measurements.graphVisible
		? std::min(std::max(0, clientBottom - cursor), headerHeight + graphBodyHeight)
		: 0;
	const int graphTop = clientBottom - graphReserve;
	layout.changesBody = { cursor, graphTop };
	if (measurements.graphVisible) {
		layout.graphHeader = { graphTop, std::min(clientBottom, graphTop + headerHeight) };
		layout.graphBody = { layout.graphHeader.bottom, clientBottom };
	}
	return layout;
}

} // namespace workbench::scm
