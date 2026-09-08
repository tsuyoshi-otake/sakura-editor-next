/*! @file
 * @brief Workspace repository snapshot and explicit GitHub remote selection.
 */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "config/WorkspaceContextTypes.h"
#include "workbench/scm/GitCommandRunner.h"

#include <Windows.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace senp::github {

enum class GhLocalRepositoryStatus : std::uint8_t {
	Succeeded,
	NotRepository,
	InvalidRoot,
	GitUnavailable,
	Failed,
	TimedOut,
	Cancelled,
	OutputLimitExceeded,
	InvalidOutput,
};

class GhLocalRepositoryRead final {
public:
	GhLocalRepositoryRead(GhLocalRepositoryStatus status, std::wstring repositoryPath,
		std::wstring repositoryIdentity, std::vector<std::uint8_t> remoteOutput);
	[[nodiscard]] GhLocalRepositoryStatus Status() const noexcept { return m_status; }
	[[nodiscard]] const std::wstring& RepositoryPath() const noexcept { return m_repositoryPath; }
	[[nodiscard]] const std::wstring& RepositoryIdentity() const noexcept { return m_repositoryIdentity; }
	[[nodiscard]] const std::vector<std::uint8_t>& RemoteOutput() const noexcept { return m_remoteOutput; }
private:
	GhLocalRepositoryStatus m_status{ GhLocalRepositoryStatus::InvalidRoot };
	std::wstring m_repositoryPath, m_repositoryIdentity;
	std::vector<std::uint8_t> m_remoteOutput;
};

//! Fixed passive Git operations; callers cannot supply argv or policy.
class IGhLocalRepositoryPlatform {
public:
	virtual ~IGhLocalRepositoryPlatform() = default;
	[[nodiscard]] virtual GhLocalRepositoryRead Inspect(
		std::wstring_view workspaceRoot, HANDLE stop) const = 0;
};

class CWindowsGhLocalRepositoryPlatform final : public IGhLocalRepositoryPlatform {
public:
	[[nodiscard]] GhLocalRepositoryRead Inspect(
		std::wstring_view workspaceRoot, HANDLE stop) const override;
};

enum class GhRemoteKind : std::uint8_t {
	GitHub,
	NonGitHub,
	UnresolvedSshAlias,
	Malformed,
};

class GhRemoteCandidate final {
public:
	GhRemoteCandidate(std::wstring name, std::wstring fetchUrl, GhRemoteKind kind,
		std::wstring hostname, std::wstring owner, std::wstring repository);
	GhRemoteCandidate(std::wstring name, std::wstring fetchUrl, GhRemoteKind kind);
	[[nodiscard]] const std::wstring& Name() const noexcept { return m_name; }
	[[nodiscard]] const std::wstring& FetchUrl() const noexcept { return m_fetchUrl; }
	[[nodiscard]] GhRemoteKind Kind() const noexcept { return m_kind; }
	[[nodiscard]] const std::wstring& Hostname() const noexcept { return m_hostname; }
	[[nodiscard]] const std::wstring& Owner() const noexcept { return m_owner; }
	[[nodiscard]] const std::wstring& Repository() const noexcept { return m_repository; }
private:
	std::wstring m_name, m_fetchUrl;
	GhRemoteKind m_kind{ GhRemoteKind::Malformed };
	std::wstring m_hostname, m_owner, m_repository;
};

class GhWorkspaceRepository final {
public:
	GhWorkspaceRepository(std::wstring repositoryPath, std::wstring repositoryIdentity,
		std::vector<std::wstring> workspaceRootIdentities, std::vector<GhRemoteCandidate> remotes);
	[[nodiscard]] const std::wstring& RepositoryPath() const noexcept { return m_repositoryPath; }
	[[nodiscard]] const std::wstring& RepositoryIdentity() const noexcept { return m_repositoryIdentity; }
	[[nodiscard]] const std::vector<std::wstring>& WorkspaceRootIdentities() const noexcept { return m_workspaceRootIdentities; }
	[[nodiscard]] const std::vector<GhRemoteCandidate>& Remotes() const noexcept { return m_remotes; }
private:
	std::wstring m_repositoryPath, m_repositoryIdentity;
	std::vector<std::wstring> m_workspaceRootIdentities;
	std::vector<GhRemoteCandidate> m_remotes;
};

enum class GhRepositorySnapshotStatus : std::uint8_t {
	Succeeded,
	EmptyWorkspace,
	NoFileRoots,
	NoRepository,
	PartialFailure,
	Failed,
	Cancelled,
};

class GhRepositorySnapshot final {
public:
	GhRepositorySnapshot(GhRepositorySnapshotStatus status, std::uint64_t workspaceGeneration,
		std::uint64_t workspaceRevision, std::vector<GhWorkspaceRepository> repositories,
		std::size_t unsupportedRoots, std::size_t failedRoots);
	[[nodiscard]] GhRepositorySnapshotStatus Status() const noexcept { return m_status; }
	[[nodiscard]] std::uint64_t WorkspaceGeneration() const noexcept { return m_workspaceGeneration; }
	[[nodiscard]] std::uint64_t WorkspaceRevision() const noexcept { return m_workspaceRevision; }
	[[nodiscard]] const std::vector<GhWorkspaceRepository>& Repositories() const noexcept { return m_repositories; }
	[[nodiscard]] std::size_t UnsupportedRoots() const noexcept { return m_unsupportedRoots; }
	[[nodiscard]] std::size_t FailedRoots() const noexcept { return m_failedRoots; }
private:
	GhRepositorySnapshotStatus m_status{ GhRepositorySnapshotStatus::Failed };
	std::uint64_t m_workspaceGeneration{}, m_workspaceRevision{};
	std::vector<GhWorkspaceRepository> m_repositories;
	std::size_t m_unsupportedRoots{}, m_failedRoots{};
};

enum class GhRepositorySelectionStatus : std::uint8_t {
	Selected,
	InvalidRequest,
	StaleSnapshot,
	EmptyWorkspace,
	MultipleRoots,
	NoRepository,
	NoRemotes,
	NoGitHubRemote,
	MultipleRepositories,
	RequiresSshAliasResolution,
	SelectedRootRemoved,
	SelectedRemoteRemoved,
};

class GhSelectedRepository final {
public:
	GhSelectedRepository(std::wstring repositoryIdentity, std::wstring remoteName,
		std::wstring hostname, std::wstring owner, std::wstring repository);
	[[nodiscard]] const std::wstring& RepositoryIdentity() const noexcept { return m_repositoryIdentity; }
	[[nodiscard]] const std::wstring& RemoteName() const noexcept { return m_remoteName; }
	[[nodiscard]] const std::wstring& Hostname() const noexcept { return m_hostname; }
	[[nodiscard]] const std::wstring& Owner() const noexcept { return m_owner; }
	[[nodiscard]] const std::wstring& Repository() const noexcept { return m_repository; }
private:
	std::wstring m_repositoryIdentity, m_remoteName, m_hostname, m_owner, m_repository;
};

class GhRepositorySelection final {
public:
	GhRepositorySelection(GhRepositorySelectionStatus status,
		std::optional<GhSelectedRepository> selected);
	GhRepositorySelection(GhRepositorySelectionStatus status);
	[[nodiscard]] GhRepositorySelectionStatus Status() const noexcept { return m_status; }
	[[nodiscard]] const std::optional<GhSelectedRepository>& Selected() const noexcept { return m_selected; }
private:
	GhRepositorySelectionStatus m_status{ GhRepositorySelectionStatus::InvalidRequest };
	std::optional<GhSelectedRepository> m_selected;
};

class CGhRepositorySelection final {
public:
	explicit CGhRepositorySelection(std::shared_ptr<const IGhLocalRepositoryPlatform> platform);
	[[nodiscard]] GhRepositorySnapshot Capture(
		const config::WorkspaceContextSnapshot& workspace, HANDLE stop) const;
	[[nodiscard]] GhRepositorySelection Select(const GhRepositorySnapshot& snapshot,
		std::uint64_t expectedWorkspaceGeneration, std::uint64_t expectedWorkspaceRevision,
		std::optional<std::wstring_view> repositoryIdentity,
		std::optional<std::wstring_view> remoteName) const;
	[[nodiscard]] GhRepositorySelection Select(const GhRepositorySnapshot& snapshot,
		std::uint64_t expectedWorkspaceGeneration, std::uint64_t expectedWorkspaceRevision,
		std::optional<std::wstring_view> repositoryIdentity) const;
	[[nodiscard]] GhRepositorySelection Select(const GhRepositorySnapshot& snapshot,
		std::uint64_t expectedWorkspaceGeneration, std::uint64_t expectedWorkspaceRevision) const;
private:
	std::shared_ptr<const IGhLocalRepositoryPlatform> m_platform;
};

} // namespace senp::github
