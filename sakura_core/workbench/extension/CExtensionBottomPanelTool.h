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

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace workbench::extension {

//! Panel view containers follow VS Code's left-to-right order. Ports and Debug
//! Console are visible as explicit, disabled boundaries until their native view
//! projections are implemented; they never select a fake placeholder surface.
enum class ExtensionBottomPanelTab { Problems, Output, Terminal, Ports, DebugConsole };

class CExtensionBottomPanelTool final : public IWorkbenchTool {
public:
	using ProblemActivationCallback = std::function<void(const win32::ProblemsPanelEntry&)>;
	//! Commits a user-originated Output channel selection to the owning model.
	using OutputChannelSelectionCallback = std::function<bool(const std::string& channelId)>;
	//! Commits a user-originated tab selection to the owning model. Return false
	//! to veto it; the value has no HWND or layout/model dependency.
	using TabSelectionCallback = std::function<bool(ExtensionBottomPanelTab tab)>;

	//! Common actions owned by the physical VS Code Panel Part, not by a panel view.
	struct PanelActions {
		std::function<void()> closePanel;
		std::function<void()> toggleMaximize;
		std::function<bool()> isMaximized;
	};

	CExtensionBottomPanelTool();
	~CExtensionBottomPanelTool() override;
	CExtensionBottomPanelTool(const CExtensionBottomPanelTool&) = delete;
	CExtensionBottomPanelTool& operator=(const CExtensionBottomPanelTool&) = delete;

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
	//! Applies already-committed model state. This never calls the selection callback.
	void SetActiveTab(ExtensionBottomPanelTab tab);
	//! Sends a user request to the owner. With a callback installed, native state is
	//! changed only when the next committed model snapshot is projected back.
	[[nodiscard]] bool RequestTabSelection(ExtensionBottomPanelTab tab) noexcept;
	void ShowProblems();
	void ShowOutput();
	[[nodiscard]] ExtensionBottomPanelTab ActiveTab() const noexcept;
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

} // namespace workbench::extension
