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

//! The native multiline EDIT formatting inset corresponding to VS Code's SCM
//! input content padding.
inline constexpr int kScmInputPaddingDip = 4;
inline constexpr int kScmInputMinimumHeightDip = 26;
inline constexpr int kScmInputBorderDip = 1;

//! Whether the Graph view has a history snapshot to render.  Keeping this a
//! typed state prevents "the history could not be read" from being rendered as
//! "this repository has no commits".
enum class EScmGraphPresentationStatus : std::uint8_t {
	//! No history has been read yet, or reading it failed.  The frame shows a
	//! message instead of an empty list, because an empty list would be a claim.
	Unavailable,
	Available,
};

//! Presentation contract for the `workbench.scm.history` frame.
struct ScmGraphPresentation final {
	EScmGraphPresentationStatus status{ EScmGraphPresentationStatus::Unavailable };

	[[nodiscard]] constexpr bool IsInteractive() const noexcept
	{
		switch (status) {
		case EScmGraphPresentationStatus::Unavailable: return false;
		case EScmGraphPresentationStatus::Available: return true;
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

//! Client-relative formatting rectangle for the real multiline SCM EDIT.  A
//! multiline control supports EM_SETRECTNP, so unlike Search and Quick Input it
//! keeps the full painted HWND and centers its first line with equal padding.
struct ScmInputFormattingBounds final {
	int left{};
	int top{};
	int right{};
	int bottom{};

	[[nodiscard]] constexpr bool operator==(const ScmInputFormattingBounds&) const = default;
};

//! Geometry for the painted SCM input frame, its native multiline EDIT child,
//! and the child's formatting rectangle.  The frame shares Search's 26-DIP
//! minimum while extra commit lines grow the control by the measured line height.
struct ScmInputGeometry final {
	int frameHeight{};
	ScmInputFormattingBounds editor{};
	ScmInputFormattingBounds formatting{};

	[[nodiscard]] constexpr bool operator==(const ScmInputGeometry&) const = default;
};

[[nodiscard]] constexpr int ScaleScmInputDip(int value, unsigned int dpi) noexcept
{
	const unsigned int effectiveDpi = dpi == 0 ? 96U : dpi;
	return std::max(0, (value * static_cast<int>(effectiveDpi) + 48) / 96);
}

[[nodiscard]] constexpr ScmInputGeometry BuildScmInputGeometry(
	int width,
	int lineHeight,
	int lineCount,
	unsigned int dpi) noexcept
{
	const int pad = ScaleScmInputDip(kScmInputPaddingDip, dpi);
	const int border = ScaleScmInputDip(kScmInputBorderDip, dpi);
	const int boundedWidth = std::max(0, width);
	const int contentHeight = std::max(1, lineHeight) * std::max(1, lineCount);
	const int frameHeight = std::max(
		ScaleScmInputDip(kScmInputMinimumHeightDip, dpi), contentHeight + 2 * pad);
	const int editorWidth = std::max(0, boundedWidth - 2 * border);
	const int editorHeight = std::max(0, frameHeight - 2 * border);
	const int verticalSlack = std::max(0, frameHeight - contentHeight);
	const int outerTop = verticalSlack / 2;
	const int outerBottom = verticalSlack - outerTop;
	const int horizontalFormattingInset = std::max(0, pad - border);
	const int formattingTop = std::max(0, outerTop - border);
	const int formattingBottomInset = std::max(0, outerBottom - border);
	return {
		.frameHeight = frameHeight,
		.editor = {
			.left = border,
			.top = border,
			.right = std::max(border, boundedWidth - border),
			.bottom = std::max(border, frameHeight - border),
		},
		.formatting = {
			.left = horizontalFormattingInset,
			.top = formattingTop,
			.right = std::max(horizontalFormattingInset,
				editorWidth - horizontalFormattingInset),
			.bottom = std::max(formattingTop,
				editorHeight - formattingBottomInset),
		},
	};
}

//! Scaled native measurements fed into the pure View-stack projection.
struct ScmViewStackMeasurements final {
	int clientTop{};
	int clientBottom{};
	int viewHeaderHeight{};
	int repositoryRowHeight{};
	int inputOuterMargin{};
	int inputHeight{};
	//! `.scm-editor > .scm-editor-action-button`: the Commit button rendered
	//! directly under the input, or zero when the provider contributes none.
	int actionButtonHeight{};
	int graphBodyHeight{};
	//! The sash's own hit height, split evenly across the boundary it straddles.
	int sashHeight{};
	//! The smallest Changes body the Graph is allowed to squeeze it to while it
	//! grows, so a drag can never leave the change list with no rows at all.
	int minimumBodyHeight{};
	bool repositoriesVisible{};
	//! A collapsed section keeps its header and gives up its whole body, which is
	//! upstream's own collapsed pane: the twistie is still there to reopen it.
	bool repositoriesCollapsed{};
	bool changesCollapsed{};
	bool graphCollapsed{};
	//! A sole Changes view is merged into its Source Control container upstream,
	//! so its own pane header is not drawn or allocated.
	bool changesHeaderVisible{ true };
	bool inputVisible{};
	bool actionButtonVisible{};
	bool graphVisible{};

	[[nodiscard]] constexpr bool operator==(const ScmViewStackMeasurements&) const = default;
};

//! The three sibling SCM views projected into the single native SCM HWND.
struct ScmViewStackLayout final {
	ScmVerticalBounds repositoriesHeader;
	ScmVerticalBounds repositoryRow;
	ScmVerticalBounds changesHeader;
	ScmVerticalBounds input;
	ScmVerticalBounds actionButton;
	ScmVerticalBounds changesBody;
	ScmVerticalBounds graphHeader;
	ScmVerticalBounds graphBody;
	//! The drag handle between the Changes body and the Graph section. It is an
	//! overlay, exactly as upstream's 4px `.monaco-sash` is: it consumes no space
	//! and sits across the boundary rather than beside it.
	ScmVerticalBounds sash;

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
	const int actionButtonHeight = std::max(0, measurements.actionButtonHeight);
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
		if (!measurements.repositoriesCollapsed) layout.repositoryRow = consume(repositoryRowHeight);
		else layout.repositoryRow = { cursor, cursor };
	}
	if (measurements.changesHeaderVisible) {
		layout.changesHeader = consume(headerHeight);
	} else {
		layout.changesHeader = { cursor, cursor };
	}
	// The commit box and its button belong to the Changes section, so a collapsed
	// section takes them with it rather than leaving an input above nothing.
	const bool changesOpen = !measurements.changesCollapsed;
	if (changesOpen && measurements.inputVisible) {
		(void)consume(inputOuterMargin);
		layout.input = consume(inputHeight);
		(void)consume(inputOuterMargin);
	} else {
		layout.input = { cursor, cursor };
	}
	if (changesOpen && measurements.actionButtonVisible) {
		layout.actionButton = consume(actionButtonHeight);
		(void)consume(inputOuterMargin);
	} else {
		layout.actionButton = { cursor, cursor };
	}

	const int available = std::max(0, clientBottom - cursor);
	int graphReserve = 0;
	if (measurements.graphVisible) {
		if (measurements.graphCollapsed) {
			graphReserve = std::min(available, headerHeight);
		} else if (!changesOpen) {
			// With the Changes section closed there is no body to protect, so the
			// Graph takes what is left instead of holding a gap open below itself.
			graphReserve = available;
		} else {
			const int minimumBody = std::max(0, measurements.minimumBodyHeight);
			const int largest = std::max(0, available - minimumBody);
			graphReserve = std::min(largest, headerHeight + graphBodyHeight);
			// The header alone still fits even where the body would not, because a
			// section with no header cannot be reopened.
			graphReserve = std::max(graphReserve, std::min(available, headerHeight));
		}
	}
	const int graphTop = clientBottom - graphReserve;
	layout.changesBody = { cursor, graphTop };
	if (measurements.graphVisible) {
		layout.graphHeader = { graphTop, std::min(clientBottom, graphTop + headerHeight) };
		layout.graphBody = measurements.graphCollapsed
			? ScmVerticalBounds{ layout.graphHeader.bottom, layout.graphHeader.bottom }
			: ScmVerticalBounds{ layout.graphHeader.bottom, clientBottom };
		// A collapsed Graph has no height to drag, and a hidden one has no
		// boundary at all, so neither offers a sash.
		if (!measurements.graphCollapsed && changesOpen) {
			const int sashHeight = std::max(0, measurements.sashHeight);
			layout.sash = { graphTop - sashHeight / 2, graphTop + (sashHeight - sashHeight / 2) };
		}
	}
	return layout;
}

} // namespace workbench::scm
