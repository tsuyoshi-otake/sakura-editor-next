/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "update/UpdateTypes.h"
#include "update/UpdateVersion.h"

//! Pure parsing of this fork's GitHub Releases feed. No HTTP, no Win32, no
//! clock: the bytes come in, an `Update` or a typed refusal comes out.
namespace update {

//! Where the feed lives. `owner`/`repository` come from the generated
//! `GIT_REMOTE_ORIGIN_URL`, so a fork that renames itself keeps checking its own
//! releases rather than a hard-coded repository's.
struct GitHubRepositoryRef final {
	std::wstring owner;
	std::wstring repository;

	[[nodiscard]] bool operator==(const GitHubRepositoryRef&) const = default;
};

//! Accepts the spellings `git remote` produces for a GitHub HTTPS or SSH
//! remote: `https://github.com/<owner>/<repo>[.git]`, `git@github.com:<owner>/<repo>[.git]`,
//! and `ssh://git@github.com/<owner>/<repo>[.git]`. Anything else — another
//! host, a local path — has no releases feed and is refused rather than guessed
//! at.
[[nodiscard]] std::optional<GitHubRepositoryRef> ParseGitHubRemoteUrl(std::wstring_view remoteUrl);

//! `https://api.github.com/repos/<owner>/<repo>/releases/latest`.
//!
//! This endpoint is the whole "stable releases only" decision, enforced by the
//! server: GitHub documents it as returning the most recent release that is
//! neither a draft nor a prerelease. The parser below still re-checks both
//! flags, so the rule holds even if the caller points it at the list endpoint.
[[nodiscard]] std::wstring BuildLatestReleaseUrl(const GitHubRepositoryRef& repository);

//! `sakura_install3-1-0-7221-x64.exe`, which is
//! `OutputBaseFilename=sakura_install{#MyAppVerH}-{#MyArchitecture}` from
//! `installer/sakura-common.iss` evaluated for one build.
[[nodiscard]] std::wstring BuildUpdateAssetName(const UpdateVersion& version, std::wstring_view architecture);

enum class EUpdateFeedOutcome : std::uint8_t {
	UpdateAvailable,
	//! No eligible release, or the newest eligible release is not newer than the
	//! running build. Both are "you are up to date" to the user.
	NoUpdateAvailable,
	//! A newer eligible release exists but carries no installer for this
	//! architecture. Explicitly not `UpdateAvailable` with a guessed asset.
	NoEligibleAsset,
	InvalidResponse,
	ResponseTooLarge,
};

struct UpdateFeedResult final {
	EUpdateFeedOutcome outcome = EUpdateFeedOutcome::InvalidResponse;
	//! Present only for `UpdateAvailable`.
	std::optional<Update> update;
	std::wstring diagnostic;

	[[nodiscard]] bool operator==(const UpdateFeedResult&) const = default;
};

struct UpdateFeedRequest final {
	UpdateVersion currentVersion;
	std::wstring architecture = L"x64";
};

//! Parses a `releases/latest` object, or a `releases` array from which the
//! newest eligible entry is taken. Bounded by `JsoncDocument`'s own limits;
//! oversized input is `ResponseTooLarge` rather than a truncated parse.
[[nodiscard]] UpdateFeedResult ParseGitHubReleaseFeed(std::string_view utf8, const UpdateFeedRequest& request);

} // namespace update
