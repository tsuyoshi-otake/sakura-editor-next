/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "senp/github/GhRepositorySelection.h"
#include "workbench/worktree/GitWorktreePorcelainParser.h"

#include <algorithm>
#include <cwctype>
#include <limits>
#include <string_view>
#include <utility>

namespace senp::github {
namespace {

constexpr std::size_t kMaximumRoots = 64, kMaximumRemotes = 64;
constexpr std::size_t kMaximumGitOutput = 256u * 1024u;

bool EqualInsensitive(std::wstring_view left, std::wstring_view right) noexcept
{
	return left.size() == right.size() && ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
		right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::optional<std::wstring> Utf8(std::string_view bytes)
{
	if (bytes.empty() || bytes.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return std::nullopt;
	const int count = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
		static_cast<int>(bytes.size()), nullptr, 0);
	if (count <= 0) return std::nullopt;
	std::wstring value(static_cast<std::size_t>(count), L'\0');
	if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
		static_cast<int>(bytes.size()), value.data(), count) != count) return std::nullopt;
	return value;
}

GhLocalRepositoryStatus Map(workbench::scm::EGitExecutionStatus status) noexcept
{
	using workbench::scm::EGitExecutionStatus;
	switch (status) {
	case EGitExecutionStatus::GitUnavailable: return GhLocalRepositoryStatus::GitUnavailable;
	case EGitExecutionStatus::TimedOut: return GhLocalRepositoryStatus::TimedOut;
	case EGitExecutionStatus::Cancelled: return GhLocalRepositoryStatus::Cancelled;
	case EGitExecutionStatus::OutputLimitExceeded: return GhLocalRepositoryStatus::OutputLimitExceeded;
	case EGitExecutionStatus::InvalidRequest: return GhLocalRepositoryStatus::InvalidRoot;
	default: return GhLocalRepositoryStatus::Failed;
	}
}

bool IsRemoteName(std::wstring_view value) noexcept
{
	if (value.empty() || value.size() > 128 || value.front() == L'.' || value.back() == L'.') return false;
	return std::ranges::all_of(value, [](wchar_t c) {
		return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')
			|| c == L'-' || c == L'_' || c == L'.';
	});
}

bool IsRepoPart(std::wstring_view value) noexcept
{
	return !value.empty() && value.size() <= 100 && value != L"." && value != L".."
		&& std::ranges::all_of(value, [](wchar_t c) {
			return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')
				|| c == L'-' || c == L'_' || c == L'.';
		});
}

bool IsHost(std::wstring_view value) noexcept
{
	if (value.empty() || value.size() > 253 || value.front() == L'.' || value.back() == L'.') return false;
	return std::ranges::all_of(value, [](wchar_t c) {
		return (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'.';
	});
}

std::wstring Lower(std::wstring value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
	return value;
}

GhRemoteCandidate ParseRemote(std::wstring name, std::wstring url)
{
	if (!IsRemoteName(name) || url.empty() || url.size() > 2048) {
		return { std::move(name), std::move(url), GhRemoteKind::Malformed };
	}
	std::wstring host, path;
	bool sshAlias = false;
	if (url.starts_with(L"https://")) {
		const auto begin = std::wstring_view(L"https://").size();
		const auto slash = url.find(L'/', begin);
		if (slash == std::wstring::npos || url.find_first_of(L"?#@", begin) < slash) {
			return { std::move(name), std::move(url), GhRemoteKind::Malformed };
		}
		host = Lower(url.substr(begin, slash - begin));
		path = url.substr(slash + 1);
	} else if (url.starts_with(L"ssh://")) {
		const auto begin = std::wstring_view(L"ssh://").size();
		const auto slash = url.find(L'/', begin);
		if (slash == std::wstring::npos) return { std::move(name), std::move(url), GhRemoteKind::Malformed };
		auto authority = url.substr(begin, slash - begin);
		const auto at = authority.rfind(L'@');
		if (at != std::wstring::npos) authority.erase(0, at + 1);
		if (authority.find(L':') != std::wstring::npos) return { std::move(name), std::move(url), GhRemoteKind::Malformed };
		host = Lower(std::move(authority));
		path = url.substr(slash + 1);
		sshAlias = true;
	} else {
		const auto colon = url.find(L':');
		const auto at = url.rfind(L'@', colon);
		if (colon == std::wstring::npos || at == std::wstring::npos || at >= colon) {
			return { std::move(name), std::move(url), GhRemoteKind::NonGitHub };
		}
		host = Lower(url.substr(at + 1, colon - at - 1));
		path = url.substr(colon + 1);
		sshAlias = true;
	}
	if (!IsHost(host) || path.find_first_of(L"?#\\") != std::wstring::npos) {
		return { std::move(name), std::move(url), GhRemoteKind::Malformed };
	}
	if (path.ends_with(L".git")) path.resize(path.size() - 4);
	const auto slash = path.find(L'/');
	if (slash == std::wstring::npos || path.find(L'/', slash + 1) != std::wstring::npos) {
		return { std::move(name), std::move(url), GhRemoteKind::Malformed };
	}
	auto owner = path.substr(0, slash), repository = path.substr(slash + 1);
	if (!IsRepoPart(owner) || !IsRepoPart(repository)) {
		return { std::move(name), std::move(url), GhRemoteKind::Malformed };
	}
	if (sshAlias && !EqualInsensitive(host, L"github.com")) {
		return { std::move(name), std::move(url), GhRemoteKind::UnresolvedSshAlias };
	}
	return { std::move(name), std::move(url), GhRemoteKind::GitHub,
		std::move(host), std::move(owner), std::move(repository) };
}

std::optional<std::vector<GhRemoteCandidate>> ParseRemotes(const std::vector<std::uint8_t>& bytes)
{
	if (bytes.size() > kMaximumGitOutput) return std::nullopt;
	const auto text = Utf8(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
	if (!text && !bytes.empty()) return std::nullopt;
	std::vector<GhRemoteCandidate> result;
	std::size_t offset{};
	while (text && offset < text->size()) {
		const auto end = text->find(L'\n', offset);
		auto line = std::wstring_view(*text).substr(offset,
			(end == std::wstring::npos ? text->size() : end) - offset);
		if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
		offset = end == std::wstring::npos ? text->size() : end + 1;
		if (line.empty()) continue;
		const auto tab = line.find(L'\t');
		const auto typeBegin = line.rfind(L" (");
		if (tab == std::wstring::npos || typeBegin == std::wstring::npos || typeBegin <= tab + 1
			|| line.back() != L')') return std::nullopt;
		const auto name = line.substr(0, tab), url = line.substr(tab + 1, typeBegin - tab - 1);
		const auto type = line.substr(typeBegin + 2, line.size() - typeBegin - 3);
		if (type != L"fetch" && type != L"push") return std::nullopt;
		if (type == L"push") continue;
		if (result.size() >= kMaximumRemotes) return std::nullopt;
		if (std::ranges::any_of(result, [name](const GhRemoteCandidate& item) { return item.Name() == name; })) {
			return std::nullopt;
		}
		result.push_back(ParseRemote(std::wstring(name), std::wstring(url)));
	}
	return result;
}

bool ContainsRoot(const GhWorkspaceRepository& repository, std::wstring_view identity) noexcept
{
	return std::ranges::find(repository.WorkspaceRootIdentities(), identity)
		!= repository.WorkspaceRootIdentities().end();
}

} // namespace

GhLocalRepositoryRead::GhLocalRepositoryRead(GhLocalRepositoryStatus status, std::wstring repositoryPath,
	std::wstring repositoryIdentity, std::vector<std::uint8_t> remoteOutput) : m_status(status),
	m_repositoryPath(std::move(repositoryPath)), m_repositoryIdentity(std::move(repositoryIdentity)),
	m_remoteOutput(std::move(remoteOutput)) {}

GhLocalRepositoryRead CWindowsGhLocalRepositoryPlatform::Inspect(
	std::wstring_view workspaceRoot, HANDLE stop) const
{
	workbench::scm::GitExecutionRequest root;
	root.workingDirectory = std::wstring(workspaceRoot);
	root.arguments = { L"rev-parse", L"--path-format=absolute", L"--show-toplevel" };
	root.policy = workbench::scm::EGitRequestPolicy::PassiveRepositoryRead;
	root.timeoutMilliseconds = 15000;
	root.maximumOutputBytes = 64u * 1024u;
	const auto rootResult = workbench::scm::RunGit(root, stop);
	if (!rootResult.Succeeded()) {
		const auto status = rootResult.status == workbench::scm::EGitExecutionStatus::Failed
			? GhLocalRepositoryStatus::NotRepository : Map(rootResult.status);
		return { status, {}, {}, {} };
	}
	auto pathBytes = rootResult.standardOutput;
	while (!pathBytes.empty() && (pathBytes.back() == '\n' || pathBytes.back() == '\r')) pathBytes.pop_back();
	const auto path = Utf8(std::string_view(reinterpret_cast<const char*>(pathBytes.data()), pathBytes.size()));
	if (!path) return { GhLocalRepositoryStatus::InvalidOutput, {}, {}, {} };
	const auto normalized = workbench::worktree::NormalizeWindowsWorktreePath(*path);
	if (!normalized) return { GhLocalRepositoryStatus::InvalidOutput, {}, {}, {} };

	workbench::scm::GitExecutionRequest remotes;
	remotes.workingDirectory = normalized->first;
	remotes.arguments = { L"remote", L"--verbose" };
	remotes.policy = workbench::scm::EGitRequestPolicy::PassiveRepositoryRead;
	remotes.timeoutMilliseconds = 15000;
	remotes.maximumOutputBytes = kMaximumGitOutput;
	const auto remoteResult = workbench::scm::RunGit(remotes, stop);
	if (!remoteResult.Succeeded()) return { Map(remoteResult.status), {}, {}, {} };
	return { GhLocalRepositoryStatus::Succeeded, normalized->first, normalized->second,
		remoteResult.standardOutput };
}

GhRemoteCandidate::GhRemoteCandidate(std::wstring name, std::wstring fetchUrl, GhRemoteKind kind,
	std::wstring hostname, std::wstring owner, std::wstring repository) : m_name(std::move(name)),
	m_fetchUrl(std::move(fetchUrl)), m_kind(kind), m_hostname(std::move(hostname)),
	m_owner(std::move(owner)), m_repository(std::move(repository)) {}

GhRemoteCandidate::GhRemoteCandidate(std::wstring name, std::wstring fetchUrl, GhRemoteKind kind) :
	GhRemoteCandidate(std::move(name), std::move(fetchUrl), kind, {}, {}, {}) {}

GhWorkspaceRepository::GhWorkspaceRepository(std::wstring repositoryPath, std::wstring repositoryIdentity,
	std::vector<std::wstring> workspaceRootIdentities, std::vector<GhRemoteCandidate> remotes) :
	m_repositoryPath(std::move(repositoryPath)), m_repositoryIdentity(std::move(repositoryIdentity)),
	m_workspaceRootIdentities(std::move(workspaceRootIdentities)), m_remotes(std::move(remotes)) {}

GhRepositorySnapshot::GhRepositorySnapshot(GhRepositorySnapshotStatus status,
	std::uint64_t workspaceGeneration, std::uint64_t workspaceRevision,
	std::vector<GhWorkspaceRepository> repositories, std::size_t unsupportedRoots,
	std::size_t failedRoots) : m_status(status), m_workspaceGeneration(workspaceGeneration),
	m_workspaceRevision(workspaceRevision), m_repositories(std::move(repositories)),
	m_unsupportedRoots(unsupportedRoots), m_failedRoots(failedRoots) {}

GhSelectedRepository::GhSelectedRepository(std::wstring repositoryIdentity, std::wstring remoteName,
	std::wstring hostname, std::wstring owner, std::wstring repository) :
	m_repositoryIdentity(std::move(repositoryIdentity)), m_remoteName(std::move(remoteName)),
	m_hostname(std::move(hostname)), m_owner(std::move(owner)), m_repository(std::move(repository)) {}

GhRepositorySelection::GhRepositorySelection(GhRepositorySelectionStatus status,
	std::optional<GhSelectedRepository> selected) : m_status(status), m_selected(std::move(selected)) {}

GhRepositorySelection::GhRepositorySelection(GhRepositorySelectionStatus status) : m_status(status) {}

CGhRepositorySelection::CGhRepositorySelection(std::shared_ptr<const IGhLocalRepositoryPlatform> platform) :
	m_platform(std::move(platform)) {}

GhRepositorySelection CGhRepositorySelection::Select(const GhRepositorySnapshot& snapshot,
	std::uint64_t expectedWorkspaceGeneration, std::uint64_t expectedWorkspaceRevision) const
{
	return Select(snapshot, expectedWorkspaceGeneration, expectedWorkspaceRevision, std::nullopt, std::nullopt);
}

GhRepositorySelection CGhRepositorySelection::Select(const GhRepositorySnapshot& snapshot,
	std::uint64_t expectedWorkspaceGeneration, std::uint64_t expectedWorkspaceRevision,
	std::optional<std::wstring_view> repositoryIdentity) const
{
	return Select(snapshot, expectedWorkspaceGeneration, expectedWorkspaceRevision,
		repositoryIdentity, std::nullopt);
}

GhRepositorySnapshot CGhRepositorySelection::Capture(
	const config::WorkspaceContextSnapshot& workspace, HANDLE stop) const
try {
	if (workspace.kind == config::EWorkspaceKind::Empty || workspace.folders.empty()) {
		return { GhRepositorySnapshotStatus::EmptyWorkspace, workspace.generation, workspace.revision, {}, 0, 0 };
	}
	if (!m_platform || workspace.folders.size() > kMaximumRoots) {
		return { GhRepositorySnapshotStatus::Failed, workspace.generation, workspace.revision, {}, 0, workspace.folders.size() };
	}
	std::vector<GhWorkspaceRepository> repositories;
	std::vector<std::wstring> inspectedRoots;
	std::size_t unsupported{}, failed{};
	for (const auto& folder : workspace.folders) {
		const auto windows = folder.uri.ToWindowsPath();
		if (!windows.value) { ++unsupported; continue; }
		const auto normalizedRoot = workbench::worktree::NormalizeWindowsWorktreePath(*windows.value);
		if (!normalizedRoot) { ++unsupported; continue; }
		if (std::ranges::find(inspectedRoots, normalizedRoot->second) != inspectedRoots.end()) continue;
		inspectedRoots.push_back(normalizedRoot->second);
		const auto inspected = m_platform->Inspect(normalizedRoot->first, stop);
		if (inspected.Status() == GhLocalRepositoryStatus::Cancelled) {
			return { GhRepositorySnapshotStatus::Cancelled, workspace.generation, workspace.revision, {}, unsupported, failed };
		}
		if (inspected.Status() == GhLocalRepositoryStatus::NotRepository) continue;
		if (inspected.Status() != GhLocalRepositoryStatus::Succeeded) { ++failed; continue; }
		const auto normalizedRepository = workbench::worktree::NormalizeWindowsWorktreePath(
			inspected.RepositoryPath());
		if (!normalizedRepository || normalizedRepository->second != inspected.RepositoryIdentity()) {
			++failed;
			continue;
		}
		const auto remotes = ParseRemotes(inspected.RemoteOutput());
		if (!remotes) { ++failed; continue; }
		auto existing = std::ranges::find_if(repositories, [&inspected](const GhWorkspaceRepository& item) {
			return item.RepositoryIdentity() == inspected.RepositoryIdentity();
		});
		if (existing == repositories.end()) {
			repositories.emplace_back(normalizedRepository->first, normalizedRepository->second,
				std::vector<std::wstring>{ normalizedRoot->second }, std::move(*remotes));
		} else {
			auto roots = existing->WorkspaceRootIdentities();
			roots.push_back(normalizedRoot->second);
			*existing = GhWorkspaceRepository(existing->RepositoryPath(), existing->RepositoryIdentity(),
				std::move(roots), existing->Remotes());
		}
	}
	GhRepositorySnapshotStatus status = GhRepositorySnapshotStatus::Succeeded;
	if (repositories.empty()) status = failed != 0 ? GhRepositorySnapshotStatus::Failed
		: unsupported == workspace.folders.size() ? GhRepositorySnapshotStatus::NoFileRoots
		: GhRepositorySnapshotStatus::NoRepository;
	else if (failed != 0 || unsupported != 0) status = GhRepositorySnapshotStatus::PartialFailure;
	return { status, workspace.generation, workspace.revision, std::move(repositories), unsupported, failed };
} catch (...) {
	return { GhRepositorySnapshotStatus::Failed, workspace.generation, workspace.revision, {}, 0, workspace.folders.size() };
}

GhRepositorySelection CGhRepositorySelection::Select(const GhRepositorySnapshot& snapshot,
	std::uint64_t expectedWorkspaceGeneration, std::uint64_t expectedWorkspaceRevision,
	std::optional<std::wstring_view> repositoryIdentity, std::optional<std::wstring_view> remoteName) const
try {
	if (expectedWorkspaceGeneration == 0 || expectedWorkspaceGeneration != snapshot.WorkspaceGeneration()
		|| expectedWorkspaceRevision != snapshot.WorkspaceRevision()) {
		return { GhRepositorySelectionStatus::StaleSnapshot };
	}
	if (snapshot.Status() == GhRepositorySnapshotStatus::EmptyWorkspace
		|| snapshot.Status() == GhRepositorySnapshotStatus::NoFileRoots) {
		return { GhRepositorySelectionStatus::EmptyWorkspace };
	}
	if (snapshot.Repositories().empty()) return { GhRepositorySelectionStatus::NoRepository };
	const GhWorkspaceRepository* repository{};
	if (repositoryIdentity) {
		const auto found = std::ranges::find_if(snapshot.Repositories(), [repositoryIdentity](const auto& item) {
			return item.RepositoryIdentity() == *repositoryIdentity || ContainsRoot(item, *repositoryIdentity);
		});
		if (found == snapshot.Repositories().end()) return { GhRepositorySelectionStatus::SelectedRootRemoved };
		repository = &*found;
	} else if (snapshot.Repositories().size() != 1) {
		return { GhRepositorySelectionStatus::MultipleRoots };
	} else repository = &snapshot.Repositories().front();
	if (repository->Remotes().empty()) return { GhRepositorySelectionStatus::NoRemotes };
	const GhRemoteCandidate* remote{};
	if (remoteName) {
		const auto found = std::ranges::find_if(repository->Remotes(), [remoteName](const auto& item) {
			return item.Name() == *remoteName;
		});
		if (found == repository->Remotes().end()) return { GhRepositorySelectionStatus::SelectedRemoteRemoved };
		remote = &*found;
		if (remote->Kind() == GhRemoteKind::UnresolvedSshAlias) {
			return { GhRepositorySelectionStatus::RequiresSshAliasResolution };
		}
		if (remote->Kind() != GhRemoteKind::GitHub) return { GhRepositorySelectionStatus::NoGitHubRemote };
	} else {
		const auto unresolved = std::ranges::count_if(repository->Remotes(), [](const auto& item) {
			return item.Kind() == GhRemoteKind::UnresolvedSshAlias;
		});
		std::vector<const GhRemoteCandidate*> github;
		for (const auto& item : repository->Remotes()) if (item.Kind() == GhRemoteKind::GitHub) github.push_back(&item);
		if (unresolved != 0) return { GhRepositorySelectionStatus::RequiresSshAliasResolution };
		if (github.empty()) return { GhRepositorySelectionStatus::NoGitHubRemote };
		const auto first = github.front();
		const bool distinct = std::ranges::any_of(github, [first](const auto* item) {
			return !EqualInsensitive(item->Hostname(), first->Hostname())
				|| item->Owner() != first->Owner() || item->Repository() != first->Repository();
		});
		if (distinct) return { GhRepositorySelectionStatus::MultipleRepositories };
		remote = first;
	}
	return { GhRepositorySelectionStatus::Selected, GhSelectedRepository(repository->RepositoryIdentity(),
		remote->Name(), remote->Hostname(), remote->Owner(), remote->Repository()) };
} catch (...) {
	return { GhRepositorySelectionStatus::InvalidRequest };
}

} // namespace senp::github
