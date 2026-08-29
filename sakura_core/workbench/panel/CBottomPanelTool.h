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
#include "workbench/win32/ProblemsOutputPanelProjection.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace workbench::panel {

//! Panel view containers follow VS Code's left-to-right order. VS Code also
//! contributes Ports and Debug Console to this Part; both are omitted here.
//! Neither has a native view projection, and neither the Remote Development
//! authority that Ports forwards through nor the DAP adapter transport that a
//! Debug Console reads exists in this fork. A tab that can never be selected is
//! chrome that promises a surface, so the container is omitted rather than
//! rendered inert. The divergence and its reason are recorded in
//! workbench/CLAUDE.md; the pure Ports and Debug Console models are untouched.
enum class BottomPanelTab { Problems, Output, Terminal };

struct BottomPanelVerticalLayout final {
	int headerHeight = 0;
	int contentTop = 0;
	int contentHeight = 0;
	int outputSelectorHeight = 0;
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

class CBottomPanelTool final : public IWorkbenchTool {
public:
	using ProblemActivationCallback = std::function<void(const win32::ProblemsPanelEntry&)>;
	//! Commits a user-originated Output channel selection to the owning model.
	using OutputChannelSelectionCallback = std::function<bool(const std::string& channelId)>;
	//! Commits a user-originated tab selection to the owning model. Return false
	//! to veto it; the value has no HWND or layout/model dependency.
	using TabSelectionCallback = std::function<bool(BottomPanelTab tab)>;

	//! Common actions owned by the physical VS Code Panel Part, not by a panel view.
	struct PanelActions {
		std::function<void()> closePanel;
		std::function<void()> toggleMaximize;
		std::function<bool()> isMaximized;
	};

	explicit CBottomPanelTool(terminal::TerminalTabManagerDependencies terminalDependencies = {});
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
	void SetTabSelectionCallback(TabSelectionCallback callback);
	//! Sets the common Panel Part chrome actions. The callbacks are invoked only for
	//! user input; committed visibility/extent state still arrives through the model.
	void SetPanelActions(PanelActions actions);
	void Refresh();
	//! Refreshes localized panel chrome without replacing committed snapshots.
	void RefreshStrings();
	//! Applies already-committed model state. This never calls the selection callback.
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
