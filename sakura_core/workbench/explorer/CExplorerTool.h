/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/IWorkbenchTool.h"

#include <Windows.h>

#include <functional>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::explorer {

//! A filesystem entry returned by one non-recursive directory enumeration.
struct ExplorerEntry {
	std::wstring name;
	std::wstring path;
	bool isDirectory = false;
	bool isReparsePoint = false;
};

//! The worker always reaches one of these states.  Stopped is terminal.
enum class ExplorerWorkerState : unsigned char {
	Idle,
	Running,
	CancelRequested,
	Stopped,
};

//! Theme tokens consumed by the native Explorer projection. The construction
//! values are bootstrap fallbacks; production injection maps each field from a
//! VS Code Side Bar semantic role in CViewContainerPages.
struct ExplorerPalette {
	COLORREF background = RGB(0x20, 0x23, 0x2A);
	COLORREF text = RGB(0xE8, 0xEB, 0xF0);
	COLORREF secondaryText = RGB(0xA8, 0xAE, 0xB8);
	COLORREF border = RGB(0x38, 0x3E, 0x49);
	COLORREF focus = RGB(0xEB, 0x6A, 0x9A);
	COLORREF inactiveSelection = RGB(0x38, 0x3E, 0x49);
	COLORREF hover = RGB(0x2A, 0x2E, 0x36);
	COLORREF selectionText = RGB(0xFF, 0xFF, 0xFF);
	COLORREF button = RGB(0xEB, 0x6A, 0x9A);
	COLORREF buttonHover = RGB(0xF2, 0x83, 0xAD);
	COLORREF buttonText = RGB(0xFF, 0xFF, 0xFF);
	COLORREF scrollbarThumb = RGB(0x38, 0x3E, 0x49);
	COLORREF scrollbarThumbHover = RGB(0x8B, 0x91, 0x9B);
	COLORREF scrollbarTrackHover = RGB(0x2A, 0x2E, 0x36);
};

//! Matches VS Code's editor-opening intent: a selection previews, while an
//! explicit activation pins the editor and prevents the next preview replacing it.
enum class ExplorerFileActivationKind : unsigned char {
	Preview,
	Pinned,
};

//! Process-local projection of VS Code's editor-group preview rules.  The
//! current native editor process owns one input; OpenNewEditor composes another
//! process in the same tab group, while ReplaceCurrentPreview reuses this one.
enum class ExplorerEditorActivationAction : unsigned char {
	ActivateCurrent,
	ReplaceCurrentPreview,
	OpenNewEditor,
};

//! The empty Explorer welcome variants exposed by VS Code's ViewWelcome.
enum class ExplorerWelcomeState : unsigned char {
	NoFolder,
	NoFolderWithEditors,
	EmptyWorkspace,
	//! A workspace has real folders, but this native projection has no multi-root
	//! tree model. Keep the unsupported boundary explicit rather than claiming no
	//! folder is open.
	WorkspaceWithFoldersUnsupported,
};

struct ExplorerEditorActivationPlan final {
	ExplorerEditorActivationAction action = ExplorerEditorActivationAction::OpenNewEditor;
	bool nextEditorIsPreview = false;
};

[[nodiscard]] constexpr ExplorerEditorActivationPlan PlanExplorerEditorActivation(
	bool currentEditorIsPreview, bool sameResource, ExplorerFileActivationKind kind) noexcept
{
	if (sameResource) {
		return {
			.action = ExplorerEditorActivationAction::ActivateCurrent,
			.nextEditorIsPreview = kind == ExplorerFileActivationKind::Preview
				&& currentEditorIsPreview,
		};
	}
	if (kind == ExplorerFileActivationKind::Preview && currentEditorIsPreview) {
		return {
			.action = ExplorerEditorActivationAction::ReplaceCurrentPreview,
			.nextEditorIsPreview = true,
		};
	}
	return {
		.action = ExplorerEditorActivationAction::OpenNewEditor,
		.nextEditorIsPreview = kind == ExplorerFileActivationKind::Preview,
	};
}

//! Left workbench Explorer backed by a Win32 TreeView with native scrollbars
//! suppressed and a VS Code-shaped overlay vertical scrollbar owned here.
//!
//! SetRoot only adds a root item. Child directories are enumerated on the one worker
//! thread when their item is expanded; refreshes restore expansion by filesystem path,
//! files activate on single click or Enter, and reparse points are displayed as leaves.
class CExplorerTool final : public IWorkbenchTool {
public:
	using FileActivationCallback = std::function<void(
		std::wstring_view path, ExplorerFileActivationKind kind)>;
	//! Dispatches one stable command with its serialized argument list.  The
	//! return value reports whether a registered executor handled the command.
	using CommandCallback = std::function<bool(
		std::string_view commandId, std::string_view argumentsJson)>;
	//! Resolves a command ID to the registry's title for menu rendering.  An
	//! empty result means the command is not registered; the context menu then
	//! fails closed instead of rendering a partial menu.
	using MenuTitleResolver = std::function<std::wstring(std::string_view commandId)>;
	//! Commits an inline rename: the entry at `path` takes the entered name.
	using RenameCommitCallback = std::function<void(
		std::wstring_view path, std::wstring_view newName)>;
	//! Commits an inline create of a file or folder under `parentDirectory`.
	using CreateCommitCallback = std::function<void(
		std::wstring_view parentDirectory, std::wstring_view name, bool directory)>;

	CExplorerTool();
	~CExplorerTool() override;
	CExplorerTool(const CExplorerTool&) = delete;
	CExplorerTool& operator=(const CExplorerTool&) = delete;

	bool Create(HWND parent) override;
	void Layout(const RECT& contentRect, unsigned int dpi) override;
	void Activate() override;
	void Deactivate() override;
	bool PreTranslateMessage(MSG& message) override;
	void Close() override;

	//! Replaces the sole, window-local root. The same path is a no-op; an empty value clears the tree.
	void SetRoot(std::wstring root);
	[[nodiscard]] const std::wstring& GetRoot() const noexcept;
	//! Selects the VS Code-compatible empty-state message and action set.
	//! The state is only visible while no filesystem root is projected.
	void SetWelcomeState(ExplorerWelcomeState state);
	[[nodiscard]] ExplorerWelcomeState GetWelcomeState() const noexcept;
	void SetFileActivationCallback(FileActivationCallback callback);
	void SetCommandCallback(CommandCallback callback);
	void SetMenuTitleResolver(MenuTitleResolver resolver);
	void SetRenameCommitCallback(RenameCommitCallback callback);
	void SetCreateCommitCallback(CreateCommitCallback callback);
	//! Starts an inline label edit on the entry at `path`, as VS Code's
	//! `renameFile` does.  The workspace root is not renameable.  The eventual
	//! commit arrives through the rename-commit callback; the tree itself never
	//! applies the label optimistically.
	bool BeginRenameEntry(std::wstring_view path);
	//! Starts an inline create under `parentDirectory` with a temporary row, as
	//! VS Code's `explorer.newFile`/`explorer.newFolder` do.  The commit arrives
	//! through the create-commit callback; the temporary row is removed and the
	//! filesystem watcher renders the real outcome.
	bool BeginCreateEntry(std::wstring_view parentDirectory, bool directory);
	//! Starts a create operation in the selected directory, or in the selected
	//! file's parent. This is the operand resolution used by VS Code's Explorer
	//! ViewTitle New File/New Folder actions.
	bool CreateEntryFromSelection(bool directory);
	//! Re-enumerates every expanded directory without changing the current root
	//! or replacing live TreeView items.
	void Refresh();
	void RefreshStrings();
	//! Collapses the Explorer's root and descendants and clears the persisted
	//! expansion set used by watcher-driven reconciliation.
	void CollapseAllFolders();
	void SetPalette(ExplorerPalette palette);
	[[nodiscard]] ExplorerPalette GetPalette() const noexcept;
	[[nodiscard]] ExplorerWorkerState GetWorkerState() const noexcept;
	[[nodiscard]] HWND GetHwnd() const noexcept;

	//! Pure helpers used by the directory worker and unit tests.
	[[nodiscard]] static std::vector<ExplorerEntry> SortEntries(std::vector<ExplorerEntry> entries);
	//! Returns the VS Code-style workspace label while keeping the canonical path private.
	[[nodiscard]] static std::wstring WorkspaceDisplayName(std::wstring_view root);
	//! UI result acceptance guard. A result from a cancelled/replaced root is stale.
	[[nodiscard]] static bool IsCurrentGeneration(std::uint64_t current, std::uint64_t candidate) noexcept;
	[[nodiscard]] static bool IsReparsePoint(DWORD attributes) noexcept;
	[[nodiscard]] static bool CanExpand(const ExplorerEntry& entry) noexcept;

	static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace workbench::explorer
