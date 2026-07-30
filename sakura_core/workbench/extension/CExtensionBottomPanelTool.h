/*! @file
	@brief Bottom panel tabs for Terminal, extension Problems, and extension Output
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "extension/CExtensionWorkbenchUi.h"
#include "terminal/window/CTerminalTool.h"
#include "theme/CThemeService.h"
#include "workbench/IWorkbenchTool.h"

#include <functional>
#include <memory>
#include <vector>

namespace workbench::extension {

enum class ExtensionBottomPanelTab { Terminal, Problems, Output };

class CExtensionBottomPanelTool final : public IWorkbenchTool {
public:
	using ProblemsProvider = std::function<std::vector<SExtensionProblem>()>;
	using OutputProvider = std::function<std::vector<SExtensionOutputChannel>()>;
	using ProblemActivationCallback = std::function<void(const SExtensionProblem&)>;

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
	void SetProblemsProvider(ProblemsProvider provider);
	void SetOutputProvider(OutputProvider provider);
	void SetProblemActivationCallback(ProblemActivationCallback callback);
	void Refresh();
	void ShowProblems();
	void ShowOutput();
	[[nodiscard]] ExtensionBottomPanelTab ActiveTab() const noexcept;

	static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace workbench::extension
