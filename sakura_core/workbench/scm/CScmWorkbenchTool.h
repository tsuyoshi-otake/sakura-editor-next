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

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::scm {

class CScmWorkbenchTool final : public IWorkbenchTool {
public:
	using FileActivationCallback = std::function<void(std::wstring_view)>;
	/*!
		@brief 公開済みプロバイダーの statusBarCommands を通知する

		実 VS Code の SCMStatusBarController は `SourceControl.statusBarCommands`
		をそのままステータスバーへ出す。ここで `GitScmState` を渡してしまうと、
		ステータスバーだけが解析結果を直接読む 2 本目の描画経路になり、ビューと
		食い違い得る。空のリストは「表示すべき SCM 項目が無い」を意味する。
	*/
	using StatusBarCommandsCallback = std::function<void(const std::vector<ScmCommand>&)>;
	/*!
		@brief Runs a published `ScmCommand`: the command id and its JSON arguments.

		Returns whether the host **recognized** the command, not whether running
		it succeeded. A recognized command is terminal, exactly as the native
		command route requires; an unrecognized one lets the caller fall back,
		which is what keeps `git.openFile` opening the file while no diff editor
		exists to register it against.
	*/
	using CommandCallback = std::function<bool(std::string_view, std::string_view)>;

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
	//! Whether the current window has a single open workspace folder, i.e.
	//! `CWorkbenchRuntime`'s state is `Folder` (see that class's own state
	//! enum). This decides which of upstream's two mutually exclusive
	//! `viewsWelcome` empty states the Source Control view shows when no
	//! repository is open: `Initialize Repository` with a folder open,
	//! `Clone Repository` with none. Defaults to `false` until the composition
	//! root calls this; see `GitInitCloneCommands.h`'s `BuildGitScmWelcomeModel`
	//! and this directory's CLAUDE.md for the exact split.
	void SetHasOpenFolder(bool hasOpenFolder);
	void SetPalette(const theme::ThemePalette& palette);
	void SetFileActivationCallback(FileActivationCallback callback);
	void SetStatusBarCommandsCallback(StatusBarCommandsCallback callback);
	void SetCommandCallback(CommandCallback callback);
	//! Borrow the runtime-owned SCM authority. The tool never stops or owns it.
	void SetSourceControlService(SourceControlService* service);
	//! Re-read the immutable provider snapshot on the next UI turn.
	void RefreshExtensionProviders();
	void SetVisible(bool visible);
	void Refresh();
	[[nodiscard]] const GitScmState& State() const noexcept;
	//! Providers currently published, i.e. upstream's `gitOpenRepositoryCount`.
	//! The composition root projects this into the command context so
	//! `git.checkout` and friends are gated exactly as VS Code gates them.
	[[nodiscard]] std::size_t OpenRepositoryCount() const noexcept;
	//! The commit message the input box currently holds. `git.commit` reads it
	//! here rather than from `SourceControlService::Snapshot`, which deep-copies
	//! every provider and must not run on a command path.
	[[nodiscard]] std::wstring CommitMessage() const;
	//! Replace the commit message, control and published input-box state
	//! together. Upstream clears it after a successful commit
	//! (`repository.inputBox.value = ''`), which is the only caller here.
	void SetCommitMessage(std::wstring value);
	[[nodiscard]] HWND GetHwnd() const noexcept;

	static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
	//! Paints the commit box's placeholder, which `EM_SETCUEBANNER` cannot:
	//! that message is documented for single-line edit controls only, and
	//! upstream's box grows to `scm.inputMaxLineCount` lines.
	static LRESULT CALLBACK InputSubclassProc(HWND window, UINT message, WPARAM wParam,
		LPARAM lParam, UINT_PTR id, DWORD_PTR data);

	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace workbench::scm
