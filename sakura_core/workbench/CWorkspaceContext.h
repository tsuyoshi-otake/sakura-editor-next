/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace workbench {

//! Pure window-local workspace selection.  It is never stored in shared memory.
class CWorkspaceContext final {
public:
	CWorkspaceContext() = default;
	explicit CWorkspaceContext(std::wstring currentDirectory);

	void SetCurrentDirectory(std::wstring currentDirectory);
	void SetExplicitRoot(std::wstring root);
	void ClearExplicitRoot();
	void SetSelectedFile(std::wstring file);
	void ClearSelectedFile();

	[[nodiscard]] const std::wstring& GetSelectedFile() const noexcept { return m_selectedFile; }
	[[nodiscard]] const std::wstring& GetCurrentDirectory() const noexcept { return m_currentDirectory; }
	[[nodiscard]] std::wstring GetRoot() const;
	//! Capture this when a new terminal starts. Existing sessions deliberately retain their own CWD.
	[[nodiscard]] std::wstring GetNewTerminalWorkingDirectory() const;

	[[nodiscard]] static std::wstring ResolveRoot(const std::wstring& explicitRoot,
		const std::wstring& activeFile, const std::wstring& currentDirectory);
	[[nodiscard]] static std::wstring ParentDirectory(const std::wstring& filePath);

private:
	std::wstring m_explicitRoot;
	std::wstring m_selectedFile;
	std::wstring m_currentDirectory;
};

//! Limits an explorer to one active worker and coalesces file-system notifications.
class CExplorerRefreshCoordinator final {
public:
	static constexpr std::uint64_t kDebounceMilliseconds = 150;

	[[nodiscard]] std::uint64_t RequestEnumeration() noexcept;
	void Cancel() noexcept;
	[[nodiscard]] bool IsCurrent(std::uint64_t generation) const noexcept;
	[[nodiscard]] bool TryAcquireWorker(std::uint64_t generation) noexcept;
	void FinishWorker(std::uint64_t generation) noexcept;
	void NotifyDirectoryChange(std::uint64_t nowMilliseconds) noexcept;
	[[nodiscard]] std::optional<std::uint64_t> TakeDueRefresh(std::uint64_t nowMilliseconds) noexcept;
	//! Reparse points (junctions/symlinks) are leaf entries; no automatic recursive walk is allowed.
	[[nodiscard]] static bool ShouldRecurseIntoEntry(bool isDirectory, bool isReparsePoint) noexcept;

private:
	std::uint64_t m_generation = 0;
	std::uint64_t m_dueAt = 0;
	std::uint64_t m_workerGeneration = 0;
	bool m_refreshPending = false;
};

} // namespace workbench
