/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/scm/GitHubRepositoryStatus.h"

using namespace workbench::scm;

namespace {

[[nodiscard]] GitHubRepositoryLabelText Words()
{
	return { L"Public", L"Private", L"Internal" };
}

[[nodiscard]] GitHubRepositoryStatus StatusOf(const EGitHubRepositoryVisibility visibility)
{
	return { GitHubRepositoryRef{ "octo", "hello" }, visibility };
}

} // namespace

TEST(GitHubRepositoryStatus, ParsesEveryGitHubRemoteForm)
{
	const GitHubRepositoryRef expected{ "octo", "hello" };
	for (const char* url : {
			 "https://github.com/octo/hello", "https://github.com/octo/hello.git",
			 "https://github.com/octo/hello/", "https://user@github.com/octo/hello.git",
			 "HTTPS://GitHub.com/octo/hello.GIT", "ssh://git@github.com/octo/hello.git",
			 "git@github.com:octo/hello.git", "github.com:octo/hello", "  git@github.com:octo/hello.git\r\n" }) {
		const auto parsed = ParseGitHubRemoteUrl(url);
		ASSERT_TRUE(parsed.has_value()) << url;
		EXPECT_EQ(*parsed, expected) << url;
	}
}

TEST(GitHubRepositoryStatus, RejectsRemotesItCannotNameOnGitHub)
{
	for (const char* url : {
			 "", "https://gitlab.com/octo/hello.git", "git@gitlab.com:octo/hello.git",
			 "https://github.com.evil.example/octo/hello", "https://github.com/octo",
			 "https://github.com/octo/hello/extra", "https://github.com//hello",
			 "git@github.com:octo", "file:///C:/repos/hello", "C:\\repos\\hello",
			 "https://github.com/oc to/hello", "https://github.com/../hello", "ftp://github.com/octo/hello" }) {
		EXPECT_FALSE(ParseGitHubRemoteUrl(url).has_value()) << url;
	}
}

TEST(GitHubRepositoryStatus, ParsesVisibilityAndNeverGuesses)
{
	EXPECT_EQ(ParseGitHubVisibility("PUBLIC\r\n"), EGitHubRepositoryVisibility::Public);
	EXPECT_EQ(ParseGitHubVisibility("private"), EGitHubRepositoryVisibility::Private);
	EXPECT_EQ(ParseGitHubVisibility(" INTERNAL \r\n"), EGitHubRepositoryVisibility::Internal);
	EXPECT_EQ(ParseGitHubVisibility(""), EGitHubRepositoryVisibility::Unknown);
	EXPECT_EQ(ParseGitHubVisibility("PUBLICLY"), EGitHubRepositoryVisibility::Unknown);
	EXPECT_EQ(ParseGitHubVisibility("gh: not logged in"), EGitHubRepositoryVisibility::Unknown);
}

TEST(GitHubRepositoryStatus, LabelUsesLockOnlyForRestrictedRepositories)
{
	EXPECT_EQ(BuildGitHubRepositoryLabel(StatusOf(EGitHubRepositoryVisibility::Public), Words()), L"$(repo) octo/hello  Public");
	EXPECT_EQ(BuildGitHubRepositoryLabel(StatusOf(EGitHubRepositoryVisibility::Private), Words()), L"$(lock) octo/hello  Private");
	EXPECT_EQ(BuildGitHubRepositoryLabel(StatusOf(EGitHubRepositoryVisibility::Internal), Words()), L"$(lock) octo/hello  Internal");
}

TEST(GitHubRepositoryStatus, UnknownVisibilityShowsTheNameAlone)
{
	const auto status = StatusOf(EGitHubRepositoryVisibility::Unknown);
	EXPECT_EQ(BuildGitHubRepositoryLabel(status, Words()), L"$(repo) octo/hello");
	EXPECT_EQ(BuildGitHubRepositoryTooltip(status, Words()), L"github.com/octo/hello");
}

TEST(GitHubRepositoryStatus, TooltipNamesHostAndVisibility)
{
	EXPECT_EQ(BuildGitHubRepositoryTooltip(StatusOf(EGitHubRepositoryVisibility::Private), Words()),
		L"github.com/octo/hello (Private)");
}

TEST(GitHubRepositoryStatus, NoRepositoryYieldsNoText)
{
	const GitHubRepositoryStatus none;
	EXPECT_TRUE(BuildGitHubRepositoryLabel(none, Words()).empty());
	EXPECT_TRUE(BuildGitHubRepositoryTooltip(none, Words()).empty());
}
