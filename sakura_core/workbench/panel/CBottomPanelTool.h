/*! @file
	@brief Bottom panel tabs for Terminal, Problems, and Output projections
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/window/CTerminalTool.h"
#include "theme/CThemeService.h"
#include "workbench/IWorkbenchTool.h"
#include "workbench/layout/WorkbenchIds.h"
#include "workbench/win32/ProblemsOutputPanelProjection.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace workbench::viewcontainer {
class IViewContainerPageHostService;
}

namespace workbench::panel {

//! Non-owning shorthand for the repository's single canonical VS Code
//! ViewContainer-ID authority. In particular, upstream Terminal is `terminal`;
//! the Panel must not invent a physical-Part-qualified replacement ID.
namespace containerIds = layout::ids::viewContainer;

enum class EBottomPanelContainerSupport : std::uint8_t {
	Supported,
	Unsupported,
	Invalid,
};

//! Ports and Debug Console are known upstream containers, but this fork has no
//! production page authority for either one. Unknown IDs are invalid rather
//! than aliases for Terminal.
[[nodiscard]] constexpr EBottomPanelContainerSupport ClassifyBottomPanelContainer(
	const std::string_view containerId) noexcept
{
	if (containerId == containerIds::Problems || containerId == containerIds::Output
		|| containerId == containerIds::Terminal) {
		return EBottomPanelContainerSupport::Supported;
	}
	if (containerId == containerIds::Ports || containerId == containerIds::DebugConsole) {
		return EBottomPanelContainerSupport::Unsupported;
	}
	return EBottomPanelContainerSupport::Invalid;
}

//! Panel view containers follow VS Code's left-to-right order. VS Code also
//! contributes Ports and Debug Console to this Part; both are omitted here.
//! Neither has a native view projection, and neither the Remote Development
//! authority that Ports forwards through nor the DAP adapter transport that a
//! Debug Console reads exists in this fork. A tab that can never be selected is
//! chrome that promises a surface, so the container is omitted rather than
//! rendered inert. The divergence and its reason are recorded in
//! workbench/CLAUDE.md; the pure Ports and Debug Console models are untouched.
enum class BottomPanelTab { Problems, Output, Terminal };

//! Typed I06 seam for moving the selected page into or out of the physical
//! Panel host. Neither transition owns the page model or TerminalInstance.
enum class EBottomPanelPageAttachStatus : std::uint8_t {
	Attached,
	AlreadyAttached,
	Failed,
	Closed,
};

enum class EBottomPanelPageDetachStatus : std::uint8_t {
	Detached,
	AlreadyDetached,
	Failed,
	Closed,
};

struct BottomPanelVerticalLayout final {
	int headerHeight = 0;
	int contentTop = 0;
	int contentHeight = 0;
	int outputSelectorHeight = 0;
};

struct BottomPanelPageLayout final {
	//! Wrapper coordinates are relative to the physical Panel host.
	RECT wrapperBounds{};
	//! Page-content coordinates are relative to the reparented wrapper.
	RECT contentBounds{};
};

//! Normalizes the panel's vertical geometry before it reaches child HWNDs.
//! During live resize the Panel can be shorter than its preferred header; no
//! child may receive an inverted rectangle or start below the client bottom.
[[nodiscard]] constexpr BottomPanelVerticalLayout CalculateBottomPanelVerticalLayout(
	int availableHeight, int preferredHeaderHeight, int preferredOutputSelectorHeight) noexcept
{
	availableHeight = (std::max)(0, availableHeight);
	preferredHeaderHeight = (std::max)(0, preferredHeaderHeight);
	preferredOutputSelectorHeight = (std::max)(0, preferredOutputSelectorHeight);
	const int headerHeight = (std::min)(availableHeight, preferredHeaderHeight);
	const int contentHeight = availableHeight - headerHeight;
	return {
		headerHeight,
		headerHeight,
		contentHeight,
		(std::min)(contentHeight, preferredOutputSelectorHeight),
	};
}

[[nodiscard]] constexpr BottomPanelPageLayout CalculateBottomPanelPageLayout(
	int availableWidth, const BottomPanelVerticalLayout& vertical) noexcept
{
	availableWidth = (std::max)(0, availableWidth);
	return {
		.wrapperBounds = { 0, vertical.contentTop, availableWidth,
			vertical.contentTop + vertical.contentHeight },
		.contentBounds = { 0, 0, availableWidth, vertical.contentHeight },
	};
}

class CBottomPanelTool final : public IWorkbenchTool {
public:
	using ProblemActivationCallback = std::function<void(const win32::ProblemsPanelEntry&)>;
	//! Commits a user-originated Output channel selection to the owning model.
	using OutputChannelSelectionCallback = std::function<bool(const std::string& channelId)>;
	//! Commits a user-originated ViewContainer selection to the owning model.
	//! The callback receives one canonical ID and is never called by Apply.
	using ContainerSelectionCallback = std::function<bool(std::string_view containerId)>;
	//! I06 compatibility conversion for the current CEditWnd composition. New
	//! callers use ContainerSelectionCallback and stable IDs.
	using TabSelectionCallback = std::function<bool(BottomPanelTab tab)>;

	//! Common actions owned by the physical VS Code Panel Part, not by a panel view.
	struct PanelActions {
		std::function<void()> closePanel;
		std::function<void()> toggleMaximize;
		std::function<bool()> isMaximized;
	};

	explicit CBottomPanelTool(terminal::TerminalTabManagerDependencies terminalDependencies = {},
		std::shared_ptr<viewcontainer::IViewContainerPageHostService> sharedPages = {});
	~CBottomPanelTool() override;
	CBottomPanelTool(const CBottomPanelTool&) = delete;
	CBottomPanelTool& operator=(const CBottomPanelTool&) = delete;

	bool Create(HWND parent) override;
	void Layout(const RECT& contentRect, unsigned int dpi) override;
	void Activate() override;
	void Deactivate() override;
	bool PreTranslateMessage(MSG& message) override;
	void Close() override;

	[[nodiscard]] terminal::CTerminalTool* Terminal() noexcept;
	void SetPalette(const theme::ThemePalette& palette);
	//! Applies an already-committed value projection without querying any producer or callback.
	void SetProblemsSnapshot(win32::ProblemsPanelSnapshot snapshot);
	//! Applies an already-committed value projection without querying any producer or callback.
	void SetOutputSnapshot(win32::OutputPanelSnapshot snapshot);
	void SetProblemActivationCallback(ProblemActivationCallback callback);
	void SetOutputChannelSelectionCallback(OutputChannelSelectionCallback callback);
	void SetContainerSelectionCallback(ContainerSelectionCallback callback);
	void SetTabSelectionCallback(TabSelectionCallback callback);
	//! Sets the common Panel Part chrome actions. The callbacks are invoked only for
	//! user input; committed visibility/extent state still arrives through the model.
	void SetPanelActions(PanelActions actions);
	void Refresh();
	//! Refreshes localized panel chrome without replacing committed snapshots.
	void RefreshStrings();
	//! Applies already-committed model state. This never calls the selection callback.
	[[nodiscard]] bool ApplyActiveContainer(std::string_view containerId);
	//! Capability is derived from either a specialized Panel adapter or a registered
	//! retained page descriptor that explicitly includes the Panel location.
	[[nodiscard]] bool SupportsContainer(std::string_view containerId) const noexcept;
	//! Sends one user request to the stable-ID owner. Apply, attachment, activation,
	//! and focus remain separate even when the callback accepts the request.
	[[nodiscard]] bool RequestContainerSelection(std::string_view containerId) noexcept;
	[[nodiscard]] std::string_view ActiveContainerId() const noexcept;
	//! Attaches/detaches only the selected page projection. These operations never
	//! stop a service, close a terminal session, or change selection/focus.
	[[nodiscard]] EBottomPanelPageAttachStatus AttachActivePage() noexcept;
	[[nodiscard]] EBottomPanelPageDetachStatus DetachActivePage() noexcept;
	[[nodiscard]] std::optional<std::string_view> AttachedContainerId() const noexcept;
	//! Compatibility conversions retained until CEditWnd migrates in I06.
	void SetActiveTab(BottomPanelTab tab);
	//! Sends a user request to the owner. With a callback installed, native state is
	//! changed only when the next committed model snapshot is projected back.
	[[nodiscard]] bool RequestTabSelection(BottomPanelTab tab) noexcept;
	void ShowProblems();
	void ShowOutput();
	[[nodiscard]] BottomPanelTab ActiveTab() const noexcept;
	//! Sends a user selection request to the owner. With a callback installed, the
	//! cached/native channel selection waits for the committed output snapshot.
	[[nodiscard]] bool RequestOutputChannelSelection(const std::string& channelId) noexcept;
	//! The panel's cached selection; it follows an accepted activeChannelId when available.
	[[nodiscard]] std::optional<std::string> SelectedOutputChannelId() const;

	static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace workbench::panel
