/*! @file
 * @brief The GitHub repository the workspace's `origin` points at, and whether it is public.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <Windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace workbench::scm {

//! `owner/repo` on github.com. Other hosts are not recognised: `gh` is asked for
//! github.com only, and a remote it cannot name is simply not shown.
struct GitHubRepositoryRef final {
	std::string owner;
	std::string repo;
	[[nodiscard]] bool operator==(const GitHubRepositoryRef&) const noexcept = default;
};

//! `Unknown` is the honest answer whenever `gh` is missing, unauthenticated,
//! slow, or says something unrecognised. It is never rendered as a guess.
enum class EGitHubRepositoryVisibility : std::uint8_t { Unknown, Public, Private, Internal };

//! What the status bar entry shows. `repository` empty means there is nothing to show.
struct GitHubRepositoryStatus final {
	std::optional<GitHubRepositoryRef> repository;
	EGitHubRepositoryVisibility visibility = EGitHubRepositoryVisibility::Unknown;
	[[nodiscard]] bool operator==(const GitHubRepositoryStatus&) const noexcept = default;
};

//!
//! @brief Parse a `git remote get-url` value that names a github.com repository.
//!
//! Accepts `https://[user@]github.com/o/r[.git][/]`, `ssh://[git@]github.com/o/r[.git]`
//! and the scp-like `git@github.com:o/r[.git]`. Anything else, including a path
//! with more or fewer than two segments, yields no value.
//!
[[nodiscard]] std::optional<GitHubRepositoryRef> ParseGitHubRemoteUrl(std::string_view url);

//! Parse `gh repo view --json visibility --jq .visibility` output (`PUBLIC`, `PRIVATE`, `INTERNAL`).
[[nodiscard]] EGitHubRepositoryVisibility ParseGitHubVisibility(std::string_view standardOutput) noexcept;

//! The label text for the status bar item, `$(codicon) owner/repo[  Visibility]`.
struct GitHubRepositoryLabelText final {
	std::wstring publicText;
	std::wstring privateText;
	std::wstring internalText;
};
[[nodiscard]] std::wstring BuildGitHubRepositoryLabel(
	const GitHubRepositoryStatus& status, const GitHubRepositoryLabelText& text);

//! Tooltip: `owner/repo` plus the visibility word when it is known.
[[nodiscard]] std::wstring BuildGitHubRepositoryTooltip(
	const GitHubRepositoryStatus& status, const GitHubRepositoryLabelText& text);

//! `git remote get-url origin` for one workspace root, as a github.com repository.
//! Blocks; call on a worker thread. Empty when there is no such remote.
[[nodiscard]] std::optional<GitHubRepositoryRef> QueryGitHubRepositoryRef(
	std::wstring_view workspaceRoot, HANDLE stop);

//!
//! @brief Ask `gh` whether the repository is public. Blocks; call on a worker thread.
//!
//! Runs only the signature-verified `gh.exe`. Missing, unsigned, unauthenticated,
//! slow, or unrecognised all answer `Unknown`; none is an error to the caller.
//!
[[nodiscard]] EGitHubRepositoryVisibility QueryGitHubRepositoryVisibility(
	const GitHubRepositoryRef& repository, std::wstring_view workspaceRoot, HANDLE stop);

} // namespace workbench::scm
