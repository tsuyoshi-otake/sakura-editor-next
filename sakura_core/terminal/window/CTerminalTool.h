/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/window/TerminalTabManager.h"
#include "workbench/IWorkbenchTool.h"
#include "theme/CThemeService.h"

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
};

//! Bottom-panel terminal tool with up to two viewports and multiple session tabs.
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
	void SetPalette( const theme::ThemePalette& palette );
	void SetPanelActions( TerminalPanelActions actions );
	//! Materializes the renderer and starts exactly one initial session without
	//! moving keyboard focus. Used when a persisted-visible panel is restored.
	[[nodiscard]] bool EnsureSessionStarted();
	[[nodiscard]] std::optional<std::uint64_t> AddTerminal();
	void RedetectPowerShell();
	[[nodiscard]] bool SelectTerminal( std::uint64_t tabId );
	[[nodiscard]] bool RestartTerminal( std::uint64_t tabId );
	[[nodiscard]] bool DeleteTerminal( std::uint64_t tabId );
	[[nodiscard]] bool SplitTerminalRight();
	[[nodiscard]] bool CloseTerminalSplit();
	[[nodiscard]] bool HasTerminalSplit() const noexcept;
	[[nodiscard]] std::vector<TerminalTabSnapshot> Tabs() const;
	[[nodiscard]] std::optional<std::uint64_t> ActiveTerminalId() const noexcept;
	[[nodiscard]] std::size_t TabCount() const noexcept;
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
