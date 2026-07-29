/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/CWorkspaceContext.h"

#include <algorithm>
#include <utility>

namespace workbench {
namespace {

[[nodiscard]] bool IsSeparator(wchar_t character) noexcept
{
	return character == L'\\' || character == L'/';
}

[[nodiscard]] std::wstring StripTrailingSeparators(std::wstring path)
{
	while (path.size() > 3 && !path.empty() && IsSeparator(path.back())) path.pop_back();
	return path;
}

} // namespace

CWorkspaceContext::CWorkspaceContext(std::wstring currentDirectory)
	: m_currentDirectory(std::move(currentDirectory))
{
}

void CWorkspaceContext::SetCurrentDirectory(std::wstring currentDirectory)
{
	m_currentDirectory = std::move(currentDirectory);
}

void CWorkspaceContext::SetExplicitRoot(std::wstring root)
{
	m_explicitRoot = StripTrailingSeparators(std::move(root));
}

void CWorkspaceContext::ClearExplicitRoot()
{
	m_explicitRoot.clear();
}

void CWorkspaceContext::SetSelectedFile(std::wstring file)
{
	m_selectedFile = std::move(file);
}

void CWorkspaceContext::ClearSelectedFile()
{
	m_selectedFile.clear();
}

std::wstring CWorkspaceContext::GetRoot() const
{
	return ResolveRoot(m_explicitRoot, m_selectedFile, m_currentDirectory);
}

std::wstring CWorkspaceContext::GetNewTerminalWorkingDirectory() const
{
	return GetRoot();
}

std::wstring CWorkspaceContext::ResolveRoot(const std::wstring& explicitRoot,
	const std::wstring& activeFile, const std::wstring& currentDirectory)
{
	if (!explicitRoot.empty()) return StripTrailingSeparators(explicitRoot);
	if (const auto parent = ParentDirectory(activeFile); !parent.empty()) return parent;
	return StripTrailingSeparators(currentDirectory);
}

std::wstring CWorkspaceContext::ParentDirectory(const std::wstring& filePath)
{
	if (filePath.empty()) return {};
	const auto lastSeparator = filePath.find_last_of(L"\\/");
	if (lastSeparator == std::wstring::npos) return {};
	if (lastSeparator == 2 && filePath.size() >= 3 && filePath[1] == L':') return filePath.substr(0, 3);
	if (lastSeparator == 0) return filePath.substr(0, 1);
	return StripTrailingSeparators(filePath.substr(0, lastSeparator));
}

std::uint64_t CExplorerRefreshCoordinator::RequestEnumeration() noexcept
{
	++m_generation;
	if (m_generation == 0) ++m_generation;
	m_refreshPending = false;
	m_dueAt = 0;
	return m_generation;
}

void CExplorerRefreshCoordinator::Cancel() noexcept
{
	(void)RequestEnumeration();
}

bool CExplorerRefreshCoordinator::IsCurrent(std::uint64_t generation) const noexcept
{
	return generation != 0 && generation == m_generation;
}

bool CExplorerRefreshCoordinator::TryAcquireWorker(std::uint64_t generation) noexcept
{
	if (!IsCurrent(generation) || m_workerGeneration != 0) return false;
	m_workerGeneration = generation;
	return true;
}

void CExplorerRefreshCoordinator::FinishWorker(std::uint64_t generation) noexcept
{
	if (m_workerGeneration == generation) m_workerGeneration = 0;
}

void CExplorerRefreshCoordinator::NotifyDirectoryChange(std::uint64_t nowMilliseconds) noexcept
{
	m_refreshPending = true;
	m_dueAt = nowMilliseconds + kDebounceMilliseconds;
}

std::optional<std::uint64_t> CExplorerRefreshCoordinator::TakeDueRefresh(std::uint64_t nowMilliseconds) noexcept
{
	if (!m_refreshPending || nowMilliseconds < m_dueAt || m_workerGeneration != 0) return std::nullopt;
	m_refreshPending = false;
	return RequestEnumeration();
}

bool CExplorerRefreshCoordinator::ShouldRecurseIntoEntry(bool isDirectory, bool isReparsePoint) noexcept
{
	return isDirectory && !isReparsePoint;
}

} // namespace workbench
