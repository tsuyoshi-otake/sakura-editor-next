/*! @file
	@brief Tests for workbench::extension::ComputeInstallButtonState (Issue #23 gap 3: update detection)
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/extension/ExtensionInstallButtonState.h"

#include <optional>
#include <string>

namespace {
using workbench::extension::ComputeInstallButtonState;
using workbench::extension::InstallButtonAction;
} // namespace

TEST(ExtensionsViewInstallButtonState, NoExtensionShownYieldsDisabledInstall)
{
	const auto state = ComputeInstallButtonState(false, std::nullopt, L"1.0.0", true);
	EXPECT_EQ(state.action, InstallButtonAction::NoExtension);
	EXPECT_EQ(state.label, L"Install");
	EXPECT_FALSE(state.enabled);
}

TEST(ExtensionsViewInstallButtonState, NotInstalledYieldsEnabledInstallWhenCallbackPresent)
{
	const auto state = ComputeInstallButtonState(true, std::nullopt, L"1.0.0", true);
	EXPECT_EQ(state.action, InstallButtonAction::Install);
	EXPECT_EQ(state.label, L"Install");
	EXPECT_TRUE(state.enabled);
}

TEST(ExtensionsViewInstallButtonState, NotInstalledStaysDisabledWithoutInstallCallback)
{
	const auto state = ComputeInstallButtonState(true, std::nullopt, L"1.0.0", false);
	EXPECT_EQ(state.action, InstallButtonAction::Install);
	EXPECT_FALSE(state.enabled);
}

TEST(ExtensionsViewInstallButtonState, OlderInstalledVersionYieldsEnabledUpdate)
{
	const auto state = ComputeInstallButtonState(true, std::optional<std::wstring>(L"0.9.0"), L"1.0.0", true);
	EXPECT_EQ(state.action, InstallButtonAction::Update);
	EXPECT_EQ(state.label, L"Update");
	EXPECT_TRUE(state.enabled);
}

TEST(ExtensionsViewInstallButtonState, MatchingInstalledVersionYieldsDisabledInstalled)
{
	const auto state = ComputeInstallButtonState(true, std::optional<std::wstring>(L"1.0.0"), L"1.0.0", true);
	EXPECT_EQ(state.action, InstallButtonAction::UpToDate);
	EXPECT_EQ(state.label, L"Installed");
	// No uninstall callback exists on this surface, so a current install must
	// not leave the button enabled for an action it cannot actually perform.
	EXPECT_FALSE(state.enabled);
}

TEST(ExtensionsViewInstallButtonState, EmptyCurrentVersionNeverCountsAsUpToDate)
{
	const auto state = ComputeInstallButtonState(true, std::optional<std::wstring>(L""), L"", true);
	EXPECT_EQ(state.action, InstallButtonAction::Update);
	EXPECT_TRUE(state.enabled);
}
