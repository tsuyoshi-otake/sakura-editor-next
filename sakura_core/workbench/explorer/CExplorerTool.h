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

//! A small, injectable palette until the workbench theme service owns these values.
struct ExplorerPalette {
	COLORREF panel = RGB(0x20, 0x23, 0x2A);
	COLORREF text = RGB(0xE8, 0xEB, 0xF0);
	COLORREF border = RGB(0x38, 0x3E, 0x49);
	COLORREF focus = RGB(0xEB, 0x6A, 0x9A);
};

//! Left workbench Explorer implemented with the standard Win32 TreeView control.
//!
//! SetRoot only adds a root item.  Child directories are enumerated on the one worker
//! thread when their item is expanded; reparse points are deliberately displayed as leaves.
class CExplorerTool final : public IWorkbenchTool {
public:
	using FileActivationCallback = std::function<void(std::wstring_view path)>;

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

	//! Replaces the sole, window-local root. An empty value clears the tree.
	void SetRoot(std::wstring root);
	[[nodiscard]] const std::wstring& GetRoot() const noexcept;
	void SetFileActivationCallback(FileActivationCallback callback);
	void SetPalette(ExplorerPalette palette);
	[[nodiscard]] ExplorerPalette GetPalette() const noexcept;
	[[nodiscard]] ExplorerWorkerState GetWorkerState() const noexcept;
	[[nodiscard]] HWND GetHwnd() const noexcept;

	//! Pure helpers used by the directory worker and unit tests.
	[[nodiscard]] static std::vector<ExplorerEntry> SortEntries(std::vector<ExplorerEntry> entries);
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
