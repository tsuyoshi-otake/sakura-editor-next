/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "senp/SenpManagementService.h"

// Every embedded built-in is installed by default (#337), so startup brings a
// built-in the profile lacks into a fresh profile and into one an update
// reaches alike: both arrive here with no installed copy and no tombstone.
TEST(SenpBuiltInStartup, InstallsEveryEmbeddedBuiltInTheProfileLacks)
{
	for (const auto id : senp::EmbeddedBuiltInExtensionIds()) {
		EXPECT_TRUE(senp::StartupInstallsBuiltIn(id, false, false)) << std::wstring(id);
		EXPECT_TRUE(senp::StartupInstallsBuiltIn(id, false, true)) << std::wstring(id);
	}
}

// The GitHub packages are named so that moving them back to offered fails here
// rather than only in the loop above.
TEST(SenpBuiltInStartup, InstallsTheGitHubBuiltInsWithoutAGitHubConnection)
{
	EXPECT_TRUE(senp::StartupInstallsBuiltIn(L"sakura-github-pull-requests", false, false));
	EXPECT_TRUE(senp::StartupInstallsBuiltIn(L"sakura-github-actions", false, false));
}

// An explicit uninstall survives product updates; only an explicit reinstall
// clears its tombstone.
TEST(SenpBuiltInStartup, HonorsAnUninstallTombstoneForEveryEmbeddedBuiltIn)
{
	for (const auto id : senp::EmbeddedBuiltInExtensionIds()) {
		EXPECT_FALSE(senp::StartupInstallsBuiltIn(id, true, false)) << std::wstring(id);
		EXPECT_FALSE(senp::StartupInstallsBuiltIn(id, true, true)) << std::wstring(id);
	}
}

TEST(SenpBuiltInStartup, NeverInstallsAnIdTheExecutableDoesNotEmbed)
{
	EXPECT_FALSE(senp::StartupInstallsBuiltIn(L"sample.extension", false, false));
	EXPECT_FALSE(senp::StartupInstallsBuiltIn(L"sample.extension", false, true));
	EXPECT_FALSE(senp::StartupInstallsBuiltIn(L"", false, true));
}
