/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "theme/CThemeService.h"
#include "workbench/IWorkbenchTool.h"
#include "workbench/scm/GitScmModel.h"
#include "workbench/scm/SourceControlService.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace workbench::scm {

class CScmWorkbenchTool final : public IWorkbenchTool {
public:
	using FileActivationCallback = std::function<void(std::wstring_view)>;
	using StateChangedCallback = std::function<void(const GitScmState&)>;
	using CommandCallback = std::function<void(std::string_view, std::string_view)>;
	using InputChangedCallback = std::function<void(std::string_view, std::wstring_view, bool)>;

	CScmWorkbenchTool();
	~CScmWorkbenchTool() override;
	CScmWorkbenchTool(const CScmWorkbenchTool&) = delete;
	CScmWorkbenchTool& operator=(const CScmWorkbenchTool&) = delete;

	bool Create(HWND parent) override;
	void Layout(const RECT& contentRect, unsigned int dpi) override;
	void Activate() override;
	void Deactivate() override;
	bool PreTranslateMessage(MSG& message) override;
	void Close() override;

	void SetRoot(std::wstring root);
	void SetPalette(const theme::ThemePalette& palette);
	void SetFileActivationCallback(FileActivationCallback callback);
	void SetStateChangedCallback(StateChangedCallback callback);
	void SetCommandCallback(CommandCallback callback);
	void SetInputChangedCallback(InputChangedCallback callback);
	//! Borrow the runtime-owned SCM authority. The tool never stops or owns it.
	void SetSourceControlService(SourceControlService* service);
	//! Re-read the immutable provider snapshot on the next UI turn.
	void RefreshExtensionProviders();
	void SetVisible(bool visible);
	void Refresh();
	[[nodiscard]] const GitScmState& State() const noexcept;
	[[nodiscard]] HWND GetHwnd() const noexcept;

	static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace workbench::scm
