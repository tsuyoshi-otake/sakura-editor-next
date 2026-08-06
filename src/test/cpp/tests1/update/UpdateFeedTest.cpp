/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include <gtest/gtest.h>

#include <sakura/serialization/JsoncDocument.h>

#include <string>

#include "update/UpdateFeed.h"

namespace {

using update::BuildLatestReleaseUrl;
using update::BuildUpdateAssetName;
using update::EUpdateFeedOutcome;
using update::GitHubRepositoryRef;
using update::ParseGitHubReleaseFeed;
using update::ParseGitHubRemoteUrl;
using update::UpdateFeedRequest;
using update::UpdateVersion;

//! The build every feed below is compared against.
constexpr UpdateVersion kCurrent{ 3, 1, 0, 7221 };

UpdateFeedRequest Request()
{
	return UpdateFeedRequest{ kCurrent, L"x64" };
}

//! One `releases/latest` object carrying a newer build with a complete asset.
constexpr std::string_view kNewerRelease = R"json({
	"tag_name": "v3.1.0-build.7300",
	"draft": false,
	"prerelease": false,
	"html_url": "https://github.com/tsuyoshi-otake/sakura-editor-next/releases/tag/v3.1.0-build.7300",
	"body": "What changed.",
	"assets": [
		{
			"name": "sakura_install3-1-0-7300-arm64.exe",
			"state": "uploaded",
			"browser_download_url": "https://example.invalid/arm64.exe",
			"size": 111
		},
		{
			"name": "sakura_install3-1-0-7300-x64.exe",
			"state": "uploaded",
			"browser_download_url": "https://github.com/tsuyoshi-otake/sakura-editor-next/releases/download/v3.1.0-build.7300/sakura_install3-1-0-7300-x64.exe",
			"size": 4218368,
			"digest": "sha256:8D45E72D30A2294A4D8AE5B9C82C1DF424AF2FB1C2DEEB36264611D6AC5F5109"
		}
	]
})json";

} // namespace

TEST(UpdateFeed, ReadsTheOwnerAndRepositoryFromEveryRemoteSpellingGitProduces)
{
	const GitHubRepositoryRef expected{ L"tsuyoshi-otake", L"sakura-editor-next" };
	EXPECT_EQ(expected, ParseGitHubRemoteUrl(L"https://github.com/tsuyoshi-otake/sakura-editor-next"));
	EXPECT_EQ(expected, ParseGitHubRemoteUrl(L"https://github.com/tsuyoshi-otake/sakura-editor-next.git"));
	EXPECT_EQ(expected, ParseGitHubRemoteUrl(L"git@github.com:tsuyoshi-otake/sakura-editor-next.git"));
	EXPECT_EQ(expected, ParseGitHubRemoteUrl(L"ssh://git@github.com/tsuyoshi-otake/sakura-editor-next.git"));
	// `git config` output keeps its newline, and the host comparison is case-insensitive.
	EXPECT_EQ(expected, ParseGitHubRemoteUrl(L"  https://GitHub.com/tsuyoshi-otake/sakura-editor-next.git\r\n"));
}

TEST(UpdateFeed, RefusesARemoteThatHasNoGitHubReleasesFeedRatherThanGuessingOne)
{
	EXPECT_FALSE(ParseGitHubRemoteUrl(L"").has_value());
	EXPECT_FALSE(ParseGitHubRemoteUrl(L"https://gitlab.com/owner/repo.git").has_value());
	EXPECT_FALSE(ParseGitHubRemoteUrl(L"https://github.com.evil.invalid/owner/repo").has_value());
	EXPECT_FALSE(ParseGitHubRemoteUrl(L"C:\\Codes\\sakura-editor-next").has_value());
	EXPECT_FALSE(ParseGitHubRemoteUrl(L"https://github.com/owner").has_value());
	EXPECT_FALSE(ParseGitHubRemoteUrl(L"https://github.com/owner/repo/extra").has_value());
	EXPECT_FALSE(ParseGitHubRemoteUrl(L"https://github.com/owner/..").has_value());
	EXPECT_FALSE(ParseGitHubRemoteUrl(L"https://github.com/ow ner/repo").has_value());
}

TEST(UpdateFeed, BuildsTheStableOnlyEndpointAndThePackagesOwnAssetName)
{
	EXPECT_EQ(
		L"https://api.github.com/repos/tsuyoshi-otake/sakura-editor-next/releases/latest",
		BuildLatestReleaseUrl({ L"tsuyoshi-otake", L"sakura-editor-next" }));
	// An unsafe segment yields no URL at all rather than a spliced one.
	EXPECT_EQ(L"", BuildLatestReleaseUrl({ L"owner/../evil", L"repo" }));
	EXPECT_EQ(L"", BuildLatestReleaseUrl({ L"", L"repo" }));

	EXPECT_EQ(L"sakura_install3-1-0-7221-x64.exe", BuildUpdateAssetName(kCurrent, L"x64"));
	EXPECT_EQ(L"sakura_install3-1-0-7221-arm64.exe", BuildUpdateAssetName(kCurrent, L"arm64"));
}

TEST(UpdateFeed, AcceptsANewerStableReleaseWithAMatchingInstallerAsset)
{
	const auto result = ParseGitHubReleaseFeed(kNewerRelease, Request());
	ASSERT_EQ(EUpdateFeedOutcome::UpdateAvailable, result.outcome);
	ASSERT_TRUE(result.update.has_value());
	EXPECT_EQ((UpdateVersion{ 3, 1, 0, 7300 }), result.update->version);
	EXPECT_EQ(L"v3.1.0-build.7300", result.update->tagName);
	EXPECT_EQ(L"sakura_install3-1-0-7300-x64.exe", result.update->assetName);
	EXPECT_EQ(4218368u, result.update->sizeBytes);
	EXPECT_EQ(
		L"https://github.com/tsuyoshi-otake/sakura-editor-next/releases/download/v3.1.0-build.7300/sakura_install3-1-0-7300-x64.exe",
		result.update->downloadUrl);
	EXPECT_EQ(
		L"https://github.com/tsuyoshi-otake/sakura-editor-next/releases/tag/v3.1.0-build.7300",
		result.update->releaseUrl);
	EXPECT_EQ(L"What changed.", result.update->releaseNotes);
	ASSERT_TRUE(result.update->sha256.has_value());
	// The digest is normalized to lowercase hex so the verifier compares like with like.
	EXPECT_EQ(L"8d45e72d30a2294a4d8ae5b9c82c1df424af2fb1c2deeb36264611d6ac5f5109", *result.update->sha256);
	EXPECT_TRUE(result.diagnostic.empty());
}

TEST(UpdateFeed, ExcludesPrereleasesAndDraftsEvenWhenTheyAreTheOnlyReleases)
{
	constexpr std::string_view prerelease = R"json({
		"tag_name": "v3.1.0-build.7300",
		"prerelease": true,
		"assets": [ { "name": "sakura_install3-1-0-7300-x64.exe", "browser_download_url": "https://example.invalid/a.exe", "size": 10 } ]
	})json";
	constexpr std::string_view draft = R"json({
		"tag_name": "v3.1.0-build.7300",
		"draft": true,
		"assets": [ { "name": "sakura_install3-1-0-7300-x64.exe", "browser_download_url": "https://example.invalid/a.exe", "size": 10 } ]
	})json";

	// This is the fail-closed behaviour that keeps the Update button hidden until a
	// stable release exists; it is deliberately not "no stable one, so take this".
	for (const auto& feed : { prerelease, draft }) {
		const auto result = ParseGitHubReleaseFeed(feed, Request());
		EXPECT_EQ(EUpdateFeedOutcome::NoUpdateAvailable, result.outcome);
		EXPECT_FALSE(result.update.has_value());
		EXPECT_EQ(L"No stable release has been published yet.", result.diagnostic);
	}
}

TEST(UpdateFeed, TakesTheNewestEligibleEntryFromAListingArray)
{
	constexpr std::string_view listing = R"json([
		{ "tag_name": "v3.1.0-build.7250", "assets": [ { "name": "sakura_install3-1-0-7250-x64.exe", "browser_download_url": "https://example.invalid/older.exe", "size": 10 } ] },
		{ "tag_name": "v9.9.9-build.7400", "prerelease": true, "assets": [] },
		{ "tag_name": "v3.1.0-build.7310", "assets": [ { "name": "sakura_install3-1-0-7310-x64.exe", "browser_download_url": "https://example.invalid/newest.exe", "size": 20 } ] },
		{ "tag_name": "not-a-release-tag", "assets": [] }
	])json";

	const auto result = ParseGitHubReleaseFeed(listing, Request());
	ASSERT_EQ(EUpdateFeedOutcome::UpdateAvailable, result.outcome);
	ASSERT_TRUE(result.update.has_value());
	EXPECT_EQ(7310u, result.update->version.revision);
	EXPECT_EQ(L"https://example.invalid/newest.exe", result.update->downloadUrl);
}

TEST(UpdateFeed, ReportsAnUpToDateInstallationForAReleaseThatIsNotNewer)
{
	constexpr std::string_view sameBuild = R"json({
		"tag_name": "v3.1.0-build.7221",
		"assets": [ { "name": "sakura_install3-1-0-7221-x64.exe", "browser_download_url": "https://example.invalid/a.exe", "size": 10 } ]
	})json";
	const auto result = ParseGitHubReleaseFeed(sameBuild, Request());
	EXPECT_EQ(EUpdateFeedOutcome::NoUpdateAvailable, result.outcome);
	EXPECT_EQ(L"This is the latest version of Sakura Editor NEXT.", result.diagnostic);
}

TEST(UpdateFeed, RefusesToSubstituteAnotherAssetWhenThisArchitecturesInstallerIsMissing)
{
	constexpr std::string_view otherArchitectureOnly = R"json({
		"tag_name": "v3.1.0-build.7300",
		"assets": [ { "name": "sakura_install3-1-0-7300-arm64.exe", "browser_download_url": "https://example.invalid/arm64.exe", "size": 10 } ]
	})json";
	const auto result = ParseGitHubReleaseFeed(otherArchitectureOnly, Request());
	EXPECT_EQ(EUpdateFeedOutcome::NoEligibleAsset, result.outcome);
	EXPECT_FALSE(result.update.has_value());
	EXPECT_EQ(
		L"The release v3.1.0-build.7300 carries no sakura_install3-1-0-7300-x64.exe.",
		result.diagnostic);

	constexpr std::string_view noAssetsMember = R"json({ "tag_name": "v3.1.0-build.7300" })json";
	const auto missing = ParseGitHubReleaseFeed(noAssetsMember, Request());
	EXPECT_EQ(EUpdateFeedOutcome::NoEligibleAsset, missing.outcome);
	EXPECT_EQ(L"The release v3.1.0-build.7300 lists no assets.", missing.diagnostic);
}

TEST(UpdateFeed, RefusesAnAssetThatIsUnfinishedUnsizedOrNotServedOverHttps)
{
	constexpr std::string_view uploading = R"json({
		"tag_name": "v3.1.0-build.7300",
		"assets": [ { "name": "sakura_install3-1-0-7300-x64.exe", "state": "open", "browser_download_url": "https://example.invalid/a.exe", "size": 10 } ]
	})json";
	const auto pending = ParseGitHubReleaseFeed(uploading, Request());
	EXPECT_EQ(EUpdateFeedOutcome::NoEligibleAsset, pending.outcome);
	EXPECT_EQ(
		L"The installer sakura_install3-1-0-7300-x64.exe has not finished uploading.",
		pending.diagnostic);

	constexpr std::string_view plainHttp = R"json({
		"tag_name": "v3.1.0-build.7300",
		"assets": [ { "name": "sakura_install3-1-0-7300-x64.exe", "browser_download_url": "http://example.invalid/a.exe", "size": 10 } ]
	})json";
	const auto insecure = ParseGitHubReleaseFeed(plainHttp, Request());
	EXPECT_EQ(EUpdateFeedOutcome::NoEligibleAsset, insecure.outcome);
	EXPECT_EQ(
		L"The installer sakura_install3-1-0-7300-x64.exe has no HTTPS download address.",
		insecure.diagnostic);

	constexpr std::string_view zeroSize = R"json({
		"tag_name": "v3.1.0-build.7300",
		"assets": [ { "name": "sakura_install3-1-0-7300-x64.exe", "browser_download_url": "https://example.invalid/a.exe", "size": 0 } ]
	})json";
	const auto unsized = ParseGitHubReleaseFeed(zeroSize, Request());
	EXPECT_EQ(EUpdateFeedOutcome::NoEligibleAsset, unsized.outcome);
	EXPECT_EQ(L"The installer sakura_install3-1-0-7300-x64.exe reports no size.", unsized.diagnostic);
}

TEST(UpdateFeed, AcceptsAReleaseWhoseAssetCarriesNoUsableDigestButKeepsTheDigestAbsent)
{
	constexpr std::string_view noDigest = R"json({
		"tag_name": "v3.1.0-build.7300",
		"assets": [ { "name": "sakura_install3-1-0-7300-x64.exe", "browser_download_url": "https://example.invalid/a.exe", "size": 10 } ]
	})json";
	const auto absent = ParseGitHubReleaseFeed(noDigest, Request());
	ASSERT_EQ(EUpdateFeedOutcome::UpdateAvailable, absent.outcome);
	ASSERT_TRUE(absent.update.has_value());
	EXPECT_FALSE(absent.update->sha256.has_value());

	// A digest this verifier cannot compare must not be reported as one it can:
	// another algorithm, a truncated hash, and a non-hex body all yield no digest.
	constexpr std::string_view unusableDigests = R"json([
		{ "tag_name": "v3.1.0-build.7301", "assets": [ { "name": "sakura_install3-1-0-7301-x64.exe", "browser_download_url": "https://example.invalid/a.exe", "size": 10, "digest": "sha512:8d45e72d30a2294a4d8ae5b9c82c1df424af2fb1c2deeb36264611d6ac5f5109" } ] }
	])json";
	const auto wrongAlgorithm = ParseGitHubReleaseFeed(unusableDigests, Request());
	ASSERT_EQ(EUpdateFeedOutcome::UpdateAvailable, wrongAlgorithm.outcome);
	EXPECT_FALSE(wrongAlgorithm.update->sha256.has_value());

	constexpr std::string_view shortDigest = R"json({
		"tag_name": "v3.1.0-build.7302",
		"assets": [ { "name": "sakura_install3-1-0-7302-x64.exe", "browser_download_url": "https://example.invalid/a.exe", "size": 10, "digest": "sha256:8d45e7" } ]
	})json";
	const auto truncated = ParseGitHubReleaseFeed(shortDigest, Request());
	ASSERT_EQ(EUpdateFeedOutcome::UpdateAvailable, truncated.outcome);
	EXPECT_FALSE(truncated.update->sha256.has_value());
}

TEST(UpdateFeed, ReportsAMalformedListingInsteadOfPartiallyTrustingIt)
{
	const auto broken = ParseGitHubReleaseFeed("{ \"tag_name\": ", Request());
	EXPECT_EQ(EUpdateFeedOutcome::InvalidResponse, broken.outcome);
	EXPECT_EQ(L"The release listing could not be read.", broken.diagnostic);

	const auto notAnObject = ParseGitHubReleaseFeed("\"v3.1.0-build.7300\"", Request());
	EXPECT_EQ(EUpdateFeedOutcome::InvalidResponse, notAnObject.outcome);
	EXPECT_EQ(L"The release listing was not in the expected shape.", notAnObject.diagnostic);

	const auto wrongTypes = ParseGitHubReleaseFeed(
		R"json({ "tag_name": "v3.1.0-build.7300", "prerelease": "yes" })json", Request());
	EXPECT_EQ(EUpdateFeedOutcome::InvalidResponse, wrongTypes.outcome);
	EXPECT_EQ(L"The release listing was not in the expected shape.", wrongTypes.diagnostic);

	const auto nonObjectEntry = ParseGitHubReleaseFeed(R"json([ 42 ])json", Request());
	EXPECT_EQ(EUpdateFeedOutcome::InvalidResponse, nonObjectEntry.outcome);
}

TEST(UpdateFeed, RefusesAnOversizedListingBeforeParsingIt)
{
	const std::string oversized(platform::serialization::CJsoncDocument::kMaximumInputBytes + 1, ' ');
	const auto result = ParseGitHubReleaseFeed(oversized, Request());
	EXPECT_EQ(EUpdateFeedOutcome::ResponseTooLarge, result.outcome);
	EXPECT_EQ(L"The release listing was larger than this editor will parse.", result.diagnostic);
}
