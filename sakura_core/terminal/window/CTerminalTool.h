/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/window/TerminalTabManager.h"
#include "TerminalTabPresentation.h"
#include "workbench/IWorkbenchTool.h"
#include "theme/CThemeService.h"
#include "terminal/window/TerminalPaneLayout.h"
#include "terminal/input/TerminalShortcutPreset.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace terminal {

//! Frame-owned actions exposed to the terminal chrome without coupling it to CEditWnd.
struct TerminalPanelActions {
	std::function<void()> closePanel;
	std::function<void()> toggleMaximize;
	std::function<bool()> isMaximized;
	//! When false, the physical Panel Part draws maximize/close in its own chrome.
	bool renderPanelActions = true;
	//! When false, the physical Panel Part supplies the one shared header row and
	//! this tool renders only the terminal content surface.
	bool renderHeader = true;
};

enum class TerminalWorkspaceResetOutcome : std::uint8_t {
	Cleared,
	Restarted,
	RestartFailed,
	Busy,
	Unavailable,
};

struct TerminalWorkspaceResetResult {
	TerminalWorkspaceResetOutcome outcome{ TerminalWorkspaceResetOutcome::Unavailable };
	std::size_t clearedTabCount{};
	bool closeDeadlineExceeded{};
	std::uint32_t errorCode{};
};

//! Bottom-panel terminal tool with recursive split groups and a right-side
//! terminal instance list. Pane count is unbounded.
class CTerminalTool final : public workbench::IWorkbenchTool {
public:
	explicit CTerminalTool( TerminalTabManagerDependencies dependencies = {} );
	~CTerminalTool() override;
	CTerminalTool( const CTerminalTool& ) = delete;
	CTerminalTool& operator=( const CTerminalTool& ) = delete;

	bool Create( HWND parent ) override;
	void Layout( const RECT& contentRect, unsigned int dpi ) override;
	void Activate() override;
	void Deactivate() override;
	bool PreTranslateMessage( MSG& message ) override;
	void Close() override;

	void SetWorkingDirectory( std::wstring workingDirectory );
	//! Rebinds the terminal authority to a new workspace. Existing sessions,
	//! split state, and queued input are never allowed to cross this boundary.
	//! When requested, one replacement tab is created without taking focus.
	[[nodiscard]] TerminalWorkspaceResetResult ResetForWorkspace(
		std::wstring workingDirectory, bool recreateSession );
	void SetPalette( const theme::ThemePalette& palette );
	void SetPanelActions( TerminalPanelActions actions );
	//! Sets the physical Panel Part that owns the terminal-specific header actions.
	//! A null host restores standalone-header invalidation.
	void SetPanelHeaderHost( HWND host );
	//! Paints the terminal-specific actions into a host-owned, single-row header.
	void PaintPanelHeader( HDC dc, const RECT& bounds, unsigned int dpi );
	//! Forwards mouse input from the physical Panel Part's header region.
	[[nodiscard]] bool HandlePanelHeaderMessage(
		UINT message, WPARAM wParam, LPARAM lParam, const RECT& bounds, unsigned int dpi );
	//! Materializes the renderer and starts exactly one initial session without
	//! moving keyboard focus. Used when a persisted-visible panel is restored.
	[[nodiscard]] bool EnsureSessionStarted();
	[[nodiscard]] std::optional<std::uint64_t> AddTerminal();
	void RedetectPowerShell();
	//! Selects the multiplexer keybinding preset without persisting it. Used when
	//! the window applies the effective setting.
	void SetShortcutPreset( TerminalShortcutPreset preset );
	[[nodiscard]] TerminalShortcutPreset ShortcutPreset() const noexcept;
	//! Applies one coherent terminal.integrated.tabs.* projection. This is plain
	//! presentation data; it never reaches TerminalTabManager or restarts PTYs.
	void SetTabPresentationSettings( TerminalTabPresentationSettings settings );
	//! Called when the user picks a preset from the terminal menu, so the window
	//! can persist the selection. Never called by SetShortcutPreset.
	void SetShortcutPresetSink( std::function<void(TerminalShortcutPreset)> sink );
	//! Runs one key through the active preset. Returns true when the preset owns
	//! the key, meaning it must not reach the shell. PreTranslateMessage uses this
	//! after reading the physical modifier state.
	[[nodiscard]] bool DispatchShortcutPresetKey( const TerminalPresetKey& key );
	[[nodiscard]] bool SelectTerminal( std::uint64_t tabId );
	[[nodiscard]] bool RestartTerminal( std::uint64_t tabId );
	[[nodiscard]] bool DeleteTerminal( std::uint64_t tabId );
	[[nodiscard]] bool SplitTerminalRight();
	[[nodiscard]] bool SplitTerminalDown();
	[[nodiscard]] bool CloseTerminalSplit();
	[[nodiscard]] bool HasTerminalSplit() const noexcept;
	[[nodiscard]] TerminalPaneOrientation ActivePaneOrientation() const noexcept;
	[[nodiscard]] std::vector<TerminalTabSnapshot> Tabs() const;
	[[nodiscard]] std::optional<std::uint64_t> ActiveTerminalId() const noexcept;
	//! Resolves the focused terminal's display title for shared panel chrome.
	//! Empty means that the caller should keep its stable "Terminal" label.
	[[nodiscard]] std::wstring ActiveTerminalTitle() const;
	[[nodiscard]] std::size_t TabCount() const noexcept;
	//! Number of native terminal viewports in the selected terminal group.
	[[nodiscard]] std::size_t VisiblePaneCount() const noexcept;
	//! The default VS Code singleTerminal policy hides this list for one instance.
	[[nodiscard]] bool HasTerminalTabsList() const noexcept;
	//! Empty when the tabs list is hidden or cannot fit beside an 80 DIP terminal.
	[[nodiscard]] RECT TerminalTabsBounds() const noexcept;
	[[nodiscard]] bool HasStartedAnySession() const noexcept;
	//! True after the native renderer HWND has been created.  Kept observable so
	//! the hidden-panel startup contract can be unit-tested without a desktop.
	[[nodiscard]] bool HasCreatedRenderer() const noexcept;
	[[nodiscard]] HWND GetHwnd() const noexcept;

	static LRESULT CALLBACK WindowProc( HWND window, UINT message, WPARAM wParam, LPARAM lParam );

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace terminal
