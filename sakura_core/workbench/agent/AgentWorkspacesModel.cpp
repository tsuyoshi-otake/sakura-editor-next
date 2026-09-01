/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/agent/AgentWorkspacesModel.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <vector>

namespace workbench::agent {
namespace {

constexpr std::size_t kMaximumPhysicalFallbackRecords = 128;

std::wstring Utf8ToWide(std::string_view value)
{
	if (value.empty()) return {};
	const int length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		value.data(), static_cast<int>(value.size()), nullptr, 0);
	if (length <= 0) return {};
	std::wstring result(static_cast<std::size_t>(length), L'\0');
	if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		value.data(), static_cast<int>(value.size()), result.data(), length) != length) {
		return {};
	}
	return result;
}

std::wstring DisplayName(const worktree::GitWorktreeRecord& record)
{
	const std::filesystem::path path(record.path);
	auto name = path.filename().wstring();
	if (name.empty()) name = path.root_name().wstring();
	return name.empty() ? record.path : name;
}

std::wstring BranchLabel(const worktree::GitWorktreeRecord& record)
{
	if (record.bare) return L"Bare";
	if (record.detached || !record.branch) return L"Detached";
	constexpr std::string_view prefix = "refs/heads/";
	const std::string_view branch = *record.branch;
	return Utf8ToWide(branch.starts_with(prefix) ? branch.substr(prefix.size()) : branch);
}

std::wstring HeadLabel(std::string_view head)
{
	constexpr std::size_t abbreviatedLength = 7;
	return Utf8ToWide(head.substr(0, std::min(head.size(), abbreviatedLength)));
}

} // namespace

bool IsWorktreeIdentityAtOrBelow(const std::wstring_view candidateIdentity,
	const std::wstring_view worktreeIdentity) noexcept
{
	if (candidateIdentity.empty() || worktreeIdentity.empty()) return false;
	if (candidateIdentity == worktreeIdentity) return true;
	if (!candidateIdentity.starts_with(worktreeIdentity)) return false;
	if (worktreeIdentity.back() == L'\\') return true;
	return candidateIdentity.size() > worktreeIdentity.size()
		&& candidateIdentity[worktreeIdentity.size()] == L'\\';
}

std::optional<std::wstring> ResolvePhysicalDirectoryIdentity(
	const std::wstring_view path) noexcept
{
	if (path.empty() || path.size() > 32766) return std::nullopt;
	try {
		const std::wstring owned(path);
		const HANDLE handle = ::CreateFileW(owned.c_str(), FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (handle == INVALID_HANDLE_VALUE) return std::nullopt;
		struct HandleCloser final {
			HANDLE value;
			~HandleCloser() { if (value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
		} closer{ handle };
		DWORD required = ::GetFinalPathNameByHandleW(handle, nullptr, 0,
			FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
		if (required == 0 || required > 32767) return std::nullopt;
		std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1U, L'\0');
		const DWORD written = ::GetFinalPathNameByHandleW(handle, buffer.data(),
			static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
		if (written == 0 || written >= buffer.size()) return std::nullopt;
		std::wstring result(buffer.data(), written);
		std::ranges::replace(result, L'/', L'\\');
		constexpr std::wstring_view uncPrefix = L"\\\\?\\UNC\\";
		constexpr std::wstring_view devicePrefix = L"\\\\?\\";
		if (result.starts_with(uncPrefix)) {
			result = L"\\\\" + result.substr(uncPrefix.size());
		} else if (result.starts_with(devicePrefix)) {
			result.erase(0, devicePrefix.size());
		}
		std::wstring normalized(result.size(), L'\0');
		const int lowered = ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
			result.data(), static_cast<int>(result.size()), normalized.data(),
			static_cast<int>(normalized.size()), nullptr, nullptr, 0);
		if (lowered != static_cast<int>(normalized.size())) return std::nullopt;
		result = std::move(normalized);
		while (result.size() > 4 && result.back() == L'\\') result.pop_back();
		return result;
	} catch (...) {
		return std::nullopt;
	}
}

AgentWorkspacesProjectionResult ProjectAgentWorkspaces(
	const std::span<const worktree::GitWorktreeRecord> records,
	const std::wstring_view workspacePath,
	const std::wstring_view selectedIdentity,
	const AgentDirectoryIdentityResolver& identityResolver)
{
	AgentWorkspacesProjectionResult result;
	if (workspacePath.empty()) {
		result.status = EAgentWorkspacesProjectionStatus::NoWorkspace;
		return result;
	}
	if (!identityResolver) {
		result.status = EAgentWorkspacesProjectionStatus::InvalidWorkspacePath;
		return result;
	}
	const auto physicalWorkspace = identityResolver(workspacePath);
	if (!physicalWorkspace) {
		result.status = EAgentWorkspacesProjectionStatus::InvalidWorkspacePath;
		return result;
	}

	std::optional<std::size_t> currentIndex;
	std::size_t currentSpecificity = 0;
	bool ambiguous = false;
	auto consider = [&](const std::size_t index, const std::wstring_view physicalWorktree) {
		if (!IsWorktreeIdentityAtOrBelow(*physicalWorkspace, physicalWorktree)) return;
		if (!currentIndex || physicalWorktree.size() > currentSpecificity) {
			currentIndex = index;
			currentSpecificity = physicalWorktree.size();
			ambiguous = false;
		} else if (physicalWorktree.size() == currentSpecificity
			&& records[index].identity != records[*currentIndex].identity) {
			ambiguous = true;
		}
	};
	// Git normally reports the final DOS path it registered. This fast path is
	// O(N) string work and keeps directory handles off the UI path.
	for (std::size_t index = 0; index < records.size(); ++index) {
		consider(index, records[index].identity);
	}
	// A junction, symlink, or SUBST alias may make the registered lexical path
	// differ from the opened folder's final path. Resolve a bounded fallback set;
	// a repository with more records fails closed instead of doing unbounded I/O.
	if (!currentIndex && records.size() <= kMaximumPhysicalFallbackRecords) {
		for (std::size_t index = 0; index < records.size(); ++index) {
			const auto physicalWorktree = identityResolver(records[index].path);
			if (physicalWorktree) consider(index, *physicalWorktree);
		}
	}
	if (ambiguous) {
		result.status = EAgentWorkspacesProjectionStatus::AmbiguousCurrentWorktree;
		return result;
	}
	if (!currentIndex) {
		result.status = EAgentWorkspacesProjectionStatus::CurrentWorktreeUnavailable;
		return result;
	}

	result.rows.reserve(records.size());
	for (std::size_t index = 0; index < records.size(); ++index) {
		const auto& record = records[index];
		result.rows.push_back({
			.path = record.path,
			.identity = record.identity,
			.name = DisplayName(record),
			.branch = BranchLabel(record),
			.head = HeadLabel(record.head),
			.windowState = currentIndex && *currentIndex == index
				? EAgentWorktreeWindowState::ThisWindow
				: EAgentWorktreeWindowState::OpenInNewWindow,
			.detached = record.detached,
			.bare = record.bare,
			.locked = record.locked,
			.prunable = record.prunable,
		});
	}
	result.currentIndex = currentIndex;
	if (!selectedIdentity.empty()) {
		const auto selected = std::ranges::find(result.rows, selectedIdentity,
			&AgentWorktreeRow::identity);
		if (selected != result.rows.end()) {
			result.selectedIndex = static_cast<std::size_t>(selected - result.rows.begin());
		}
	}
	if (!result.selectedIndex && currentIndex) result.selectedIndex = currentIndex;
	if (!result.selectedIndex && !result.rows.empty()) result.selectedIndex = 0;
	result.status = EAgentWorkspacesProjectionStatus::Succeeded;
	return result;
}

} // namespace workbench::agent
