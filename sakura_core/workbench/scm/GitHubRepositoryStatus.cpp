/*! @file
 * @brief The GitHub repository the workspace's `origin` points at, and whether it is public.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/scm/GitHubRepositoryStatus.h"

#include "platform/process/BoundedProcessRunner.h"
#include "platform/process/WindowsExecutableResolver.h"
#include "workbench/scm/GitCommandRunner.h"

#include <sakura/security/AuthenticodeSignatureVerifier.h>

#include <algorithm>
#include <cctype>
#include <vector>

namespace workbench::scm {
namespace {

constexpr std::string_view kGitHubHost = "github.com";
constexpr std::uint32_t kGitRemoteTimeoutMilliseconds = 5000;
constexpr std::size_t kGitRemoteOutputBytes = 8u * 1024u;
constexpr std::uint32_t kGhVisibilityTimeoutMilliseconds = 8000;
constexpr std::size_t kGhVisibilityOutputBytes = 1024;
constexpr std::size_t kGhVisibilityErrorBytes = 64u * 1024u;

[[nodiscard]] std::string Trim(std::string_view text)
{
	const auto isSpace = [](const unsigned char c) { return std::isspace(c) != 0; };
	while (!text.empty() && isSpace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
	while (!text.empty() && isSpace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
	return std::string(text);
}

[[nodiscard]] bool EqualsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept
{
	return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](char a, char b) {
		return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
	});
}

[[nodiscard]] bool IsNameChar(const char c) noexcept
{
	const auto u = static_cast<unsigned char>(c);
	return std::isalnum(u) != 0 || c == '-' || c == '_' || c == '.';
}

//! `owner/repo[.git][/]` with exactly two valid segments.
[[nodiscard]] std::optional<GitHubRepositoryRef> ParseOwnerAndRepo(std::string_view path)
{
	while (!path.empty() && path.front() == '/') path.remove_prefix(1);
	while (!path.empty() && path.back() == '/') path.remove_suffix(1);
	const auto slash = path.find('/');
	if (slash == std::string_view::npos) return std::nullopt;
	std::string_view owner = path.substr(0, slash);
	std::string_view repo = path.substr(slash + 1);
	if (repo.find('/') != std::string_view::npos) return std::nullopt;
	if (repo.size() > 4 && EqualsIgnoreCase(repo.substr(repo.size() - 4), ".git")) repo.remove_suffix(4);
	const auto valid = [](std::string_view name) {
		return !name.empty() && name != "." && name != ".." && std::ranges::all_of(name, IsNameChar);
	};
	if (!valid(owner) || !valid(repo)) return std::nullopt;
	return GitHubRepositoryRef{ std::string(owner), std::string(repo) };
}

[[nodiscard]] std::wstring Widen(std::string_view ascii)
{
	return std::wstring(ascii.begin(), ascii.end());
}

[[nodiscard]] const std::wstring& VisibilityWord(
	const EGitHubRepositoryVisibility visibility, const GitHubRepositoryLabelText& text) noexcept
{
	static const std::wstring kEmpty;
	switch (visibility) {
	case EGitHubRepositoryVisibility::Public: return text.publicText;
	case EGitHubRepositoryVisibility::Private: return text.privateText;
	case EGitHubRepositoryVisibility::Internal: return text.internalText;
	case EGitHubRepositoryVisibility::Unknown: break;
	}
	return kEmpty;
}

[[nodiscard]] std::optional<std::string> ReadOriginUrl(std::wstring_view workspaceRoot, HANDLE stop)
{
	GitExecutionRequest request;
	request.workingDirectory = std::wstring(workspaceRoot);
	request.arguments = { L"remote", L"get-url", L"origin" };
	request.policy = EGitRequestPolicy::PassiveRepositoryRead;
	request.timeoutMilliseconds = kGitRemoteTimeoutMilliseconds;
	request.maximumOutputBytes = kGitRemoteOutputBytes;
	const auto result = RunGit(request, stop);
	if (!result.Succeeded()) return std::nullopt;
	return Trim(std::string_view(
		reinterpret_cast<const char*>(result.standardOutput.data()), result.standardOutput.size()));
}

} // namespace

EGitHubRepositoryVisibility QueryGitHubRepositoryVisibility(const GitHubRepositoryRef& repository,
	std::wstring_view workspaceRoot, HANDLE stop)
{
	const auto executable = platform::ResolveWindowsExecutable(L"gh.exe");
	// A PATH-planted gh.exe must not run under the editor's identity; only the
	// GitHub-signed binary is launched, exactly as the account query does.
	if (!executable || !platform::security::VerifyGitHubCliTrust(*executable)) {
		return EGitHubRepositoryVisibility::Unknown;
	}
	if (!platform::IsAbsoluteWindowsPath(workspaceRoot)) return EGitHubRepositoryVisibility::Unknown;

	platform::process::BoundedProcessRequest request(*executable, std::wstring(workspaceRoot),
		{ L"repo", L"view", Widen(std::string(kGitHubHost) + "/" + repository.owner + "/" + repository.repo),
			L"--json", L"visibility", L"--jq", L".visibility" });
	request.SetEnvironmentOverrides({
		{ L"GH_FORCE_TTY", L"0" },
		{ L"GH_NO_UPDATE_NOTIFIER", L"1" },
		{ L"GH_PAGER", L"cat" },
		{ L"GH_PROMPT_DISABLED", L"1" },
		{ L"NO_COLOR", L"1" },
	});
	request.SetEnvironmentRemovals({ L"GH_DEBUG" });
	request.SetTimeoutMilliseconds(kGhVisibilityTimeoutMilliseconds);
	request.SetMaximumStandardOutputBytes(kGhVisibilityOutputBytes);
	request.SetMaximumStandardErrorBytes(kGhVisibilityErrorBytes);

	const auto result = platform::process::RunBoundedProcess(request, stop);
	if (!result.Succeeded() || result.ExitCode() != 0) return EGitHubRepositoryVisibility::Unknown;
	return ParseGitHubVisibility(std::string_view(
		reinterpret_cast<const char*>(result.StandardOutput().data()), result.StandardOutput().size()));
}

std::optional<GitHubRepositoryRef> ParseGitHubRemoteUrl(std::string_view url)
{
	const std::string trimmed = Trim(url);
	std::string_view rest = trimmed;

	// scp-like: [user@]github.com:owner/repo[.git]
	if (rest.find("://") == std::string_view::npos) {
		const auto colon = rest.find(':');
		if (colon == std::string_view::npos) return std::nullopt;
		std::string_view host = rest.substr(0, colon);
		if (const auto at = host.rfind('@'); at != std::string_view::npos) host.remove_prefix(at + 1);
		if (!EqualsIgnoreCase(host, kGitHubHost)) return std::nullopt;
		return ParseOwnerAndRepo(rest.substr(colon + 1));
	}

	const auto scheme = rest.substr(0, rest.find("://"));
	if (!EqualsIgnoreCase(scheme, "https") && !EqualsIgnoreCase(scheme, "ssh")) return std::nullopt;
	rest.remove_prefix(scheme.size() + 3);
	const auto slash = rest.find('/');
	if (slash == std::string_view::npos) return std::nullopt;
	std::string_view authority = rest.substr(0, slash);
	if (const auto at = authority.rfind('@'); at != std::string_view::npos) authority.remove_prefix(at + 1);
	if (!EqualsIgnoreCase(authority, kGitHubHost)) return std::nullopt;
	return ParseOwnerAndRepo(rest.substr(slash + 1));
}

EGitHubRepositoryVisibility ParseGitHubVisibility(std::string_view standardOutput) noexcept
{
	try {
		const std::string word = Trim(standardOutput);
		if (EqualsIgnoreCase(word, "PUBLIC")) return EGitHubRepositoryVisibility::Public;
		if (EqualsIgnoreCase(word, "PRIVATE")) return EGitHubRepositoryVisibility::Private;
		if (EqualsIgnoreCase(word, "INTERNAL")) return EGitHubRepositoryVisibility::Internal;
	}
	catch (...) {
	}
	return EGitHubRepositoryVisibility::Unknown;
}

std::wstring BuildGitHubRepositoryLabel(
	const GitHubRepositoryStatus& status, const GitHubRepositoryLabelText& text)
{
	if (!status.repository) return {};
	const bool restricted = status.visibility == EGitHubRepositoryVisibility::Private
		|| status.visibility == EGitHubRepositoryVisibility::Internal;
	std::wstring label = restricted ? L"$(lock) " : L"$(repo) ";
	label += Widen(status.repository->owner + "/" + status.repository->repo);
	if (const auto& word = VisibilityWord(status.visibility, text); !word.empty()) {
		label += L"  ";
		label += word;
	}
	return label;
}

std::wstring BuildGitHubRepositoryTooltip(
	const GitHubRepositoryStatus& status, const GitHubRepositoryLabelText& text)
{
	if (!status.repository) return {};
	std::wstring tooltip = Widen(std::string(kGitHubHost) + "/" + status.repository->owner + "/" + status.repository->repo);
	if (const auto& word = VisibilityWord(status.visibility, text); !word.empty()) {
		tooltip += L" (";
		tooltip += word;
		tooltip += L")";
	}
	return tooltip;
}

std::optional<GitHubRepositoryRef> QueryGitHubRepositoryRef(const std::wstring_view workspaceRoot, const HANDLE stop)
{
	if (workspaceRoot.empty()) return std::nullopt;
	const auto url = ReadOriginUrl(workspaceRoot, stop);
	if (!url) return std::nullopt;
	return ParseGitHubRemoteUrl(*url);
}

} // namespace workbench::scm
