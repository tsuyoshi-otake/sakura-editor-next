/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include "workbench/WorkbenchBootstrapContext.h"

namespace {

using config::EWorkspaceKind;
using config::WorkspaceFolderDescriptor;
using platform::profiles::ProfileBootstrapSnapshot;
using platform::profiles::ResolveProfileBootstrapSnapshot;
using platform::profiles::ResolveUserDataProfileBootstrap;
using platform::profiles::UserDataProfileBootstrapRequest;
using platform::profiles::UserDataProfileBootstrapSnapshot;
using platform::profiles::UserDataProfileRegistry;
using platform::profiles::UserDataProfileResourceRootMode;
using platform::uri::Uri;
using workbench::EWorkbenchBootstrapStatus;
using workbench::ResolveWorkbenchBootstrapContext;
using workbench::WorkbenchBootstrapRequest;

constexpr char kProfileId[] = "0123456789abcdef0123456789abcdef";
constexpr char kOtherProfileId[] = "fedcba9876543210fedcba9876543210";

ProfileBootstrapSnapshot Profile()
{
	auto resolved = ResolveProfileBootstrapSnapshot(kProfileId, 5, L"C:\\Profiles\\Sakura");
	EXPECT_TRUE(resolved.Resolved());
	return std::move(*resolved.snapshot);
}

UserDataProfileBootstrapSnapshot UserDataProfile(std::string authorityId = kProfileId, std::uint64_t generation = 5)
{
	UserDataProfileRegistry registry;
	UserDataProfileBootstrapRequest request {
		{ std::move(authorityId), generation }, L"C:\\Profiles\\Sakura", {},
		UserDataProfileResourceRootMode::LegacyControlRootForDefault,
	};
	auto resolved = ResolveUserDataProfileBootstrap(request, registry);
	EXPECT_TRUE(resolved.Resolved());
	return std::move(*resolved.snapshot);
}

Uri Parse(const wchar_t* text)
{
	auto parsed = Uri::Parse(text);
	EXPECT_TRUE(parsed);
	return std::move(*parsed.value);
}

WorkbenchBootstrapRequest Request()
{
	return { Profile(), UserDataProfile(), L"window-alpha", std::nullopt, std::nullopt, {}, std::nullopt, std::nullopt };
}

TEST(WorkbenchBootstrapContext, EmptyLaunchCarriesAnExplicitEmptyWorkspace)
{
	auto result = ResolveWorkbenchBootstrapContext(Request());
	ASSERT_TRUE(result.Resolved());
	EXPECT_EQ(EWorkspaceKind::Empty, result.context->Workspace().kind);
	EXPECT_TRUE(result.context->Workspace().folders.empty());
	EXPECT_FALSE(result.context->Workspace().workspaceConfigUri);
	EXPECT_EQ(L"empty:12:window-alpha", result.context->Workspace().workspaceIdentityKey);
}

TEST(WorkbenchBootstrapContext, CopiesAndValidatesSeparatedProfileResources)
{
	auto result = ResolveWorkbenchBootstrapContext(Request());
	ASSERT_TRUE(result.Resolved());
	const auto& resources = result.context->ControlProfile().Resources();
	EXPECT_EQ(L"file:///C:/Profiles/Sakura/settings.json", resources.Settings().ToString());
	EXPECT_EQ(L"file:///C:/Profiles/Sakura/tasks.json", resources.Tasks().ToString());
	EXPECT_EQ(L"file:///C:/Profiles/Sakura/keybindings.json", resources.Keybindings().ToString());
	EXPECT_EQ(L"file:///C:/Profiles/Sakura/snippets", resources.Snippets().ToString());
	EXPECT_EQ(L"file:///C:/Profiles/Sakura/extensions.json", resources.ExtensionsManifest().ToString());
	EXPECT_EQ(L"file:///C:/Profiles/Sakura/extensions", resources.ExtensionsInstallHome().ToString());
	EXPECT_EQ(result.context->ControlProfile().ProfileId(), result.context->UserDataProfile().ControlAuthority().authorityId);
	EXPECT_EQ(result.context->ControlProfile().AuthorityGeneration(), result.context->UserDataProfile().ControlAuthority().generation);
	EXPECT_EQ(L"file:///C:/Profiles/Sakura/settings.json", result.context->UserDataProfile().Resources().Settings().ToString());
	EXPECT_EQ(result.context->ControlProfile().Resources().Settings().ToString(), result.context->Profile().Resources().Settings().ToString());
}

TEST(WorkbenchBootstrapContext, RejectsASelectedUserDataProfilePinnedToAnotherControlAuthority)
{
	auto request = Request();
	request.userDataProfile = UserDataProfile(kOtherProfileId, 5);

	const auto result = ResolveWorkbenchBootstrapContext(std::move(request));

	EXPECT_EQ(EWorkbenchBootstrapStatus::InvalidUserDataProfileSnapshot, result.status);
	EXPECT_FALSE(result.Resolved());
	EXPECT_FALSE(result.context);
}

TEST(WorkbenchBootstrapContext, UserDataResourceRootsStaySeparatedFromTheControlProfileWhenNamedProfileIsSelected)
{
	UserDataProfileRegistry registry;
	ASSERT_EQ(platform::profiles::UserDataProfileOperationStatus::Applied,
		registry.Create({ L"named-profile-01", L"Named profile", platform::profiles::UserDataProfileKind::Normal, {}, {} }).status);
	UserDataProfileBootstrapRequest selection {
		{ kProfileId, 5 }, L"C:\\Profiles\\Sakura", { L"named-profile-01", std::nullopt, std::nullopt },
		UserDataProfileResourceRootMode::ProfileIdNamespace,
	};
	auto userData = ResolveUserDataProfileBootstrap(selection, registry);
	ASSERT_TRUE(userData.Resolved());

	auto request = Request();
	request.userDataProfile = std::move(*userData.snapshot);
	const auto result = ResolveWorkbenchBootstrapContext(std::move(request));

	ASSERT_TRUE(result.Resolved());
	EXPECT_EQ(L"named-profile-01", result.context->UserDataProfile().SelectedProfileId());
	EXPECT_EQ(L"file:///C:/Profiles/Sakura/user-data-profiles/named-profile-01/settings.json",
		result.context->UserDataProfile().Resources().Settings().ToString());
	EXPECT_EQ(L"file:///C:/Profiles/Sakura/settings.json", result.context->ControlProfile().Resources().Settings().ToString());
}

TEST(WorkbenchBootstrapContext, ExplicitFolderIsTheOnlyFolderWorkspaceShape)
{
	auto request = Request();
	request.explicitFolderUri = Parse(L"file://localhost/C:/Repo");
	auto result = ResolveWorkbenchBootstrapContext(std::move(request));
	ASSERT_TRUE(result.Resolved());
	EXPECT_EQ(EWorkspaceKind::Folder, result.context->Workspace().kind);
	ASSERT_EQ(1U, result.context->Workspace().folders.size());
	EXPECT_EQ(L"file:///C:/Repo", result.context->Workspace().folders.front().uri.ToString());
	EXPECT_EQ(L"Repo", result.context->Workspace().folders.front().displayName);
}

TEST(WorkbenchBootstrapContext, ExplicitWorkspaceKeepsItsMultiRootDescriptors)
{
	auto request = Request();
	request.explicitWorkspaceConfigUri = Parse(L"file:///C:/Work/demo.code-workspace");
	request.workspaceFolders = { { Parse(L"file:///C:/One"), L"one" }, { Parse(L"file:///C:/Two"), L"two" } };
	auto result = ResolveWorkbenchBootstrapContext(std::move(request));
	ASSERT_TRUE(result.Resolved());
	EXPECT_EQ(EWorkspaceKind::Workspace, result.context->Workspace().kind);
	ASSERT_TRUE(result.context->Workspace().workspaceConfigUri);
	EXPECT_EQ(2U, result.context->Workspace().folders.size());
}

TEST(WorkbenchBootstrapContext, OpenedFileNeverEstablishesWorkspace)
{
	auto request = Request();
	request.initialDocumentUri = Parse(L"file:///C:/Outside/readme.md");
	auto result = ResolveWorkbenchBootstrapContext(std::move(request));
	ASSERT_TRUE(result.Resolved());
	EXPECT_EQ(EWorkspaceKind::Empty, result.context->Workspace().kind);
	ASSERT_TRUE(result.context->InitialDocumentUri());
	EXPECT_EQ(L"file:///C:/Outside/readme.md", result.context->InitialDocumentUri()->ToString());
}

TEST(WorkbenchBootstrapContext, TerminalFallbackIsSeparateFromWorkspaceAndDocument)
{
	auto request = Request();
	request.initialDocumentUri = Parse(L"file:///C:/Outside/readme.md");
	request.terminalLaunchDirectoryUri = Parse(L"file:///C:/Launch");
	auto result = ResolveWorkbenchBootstrapContext(std::move(request));
	ASSERT_TRUE(result.Resolved());
	EXPECT_EQ(EWorkspaceKind::Empty, result.context->Workspace().kind);
	ASSERT_TRUE(result.context->TerminalLaunchDirectoryUri());
	EXPECT_EQ(L"file:///C:/Launch", result.context->TerminalLaunchDirectoryUri()->ToString());
}

TEST(WorkbenchBootstrapContext, RejectsInvalidShapesAndDuplicateWorkspaceFolders)
{
	struct Case {
		WorkbenchBootstrapRequest request;
		EWorkbenchBootstrapStatus expected;
	};
	std::vector<Case> cases;
	{
		auto request = Request(); request.windowInstanceIdentity.clear();
		cases.push_back({ std::move(request), EWorkbenchBootstrapStatus::InvalidWindowInstanceIdentity });
	}
	{
		auto request = Request(); request.explicitFolderUri = Parse(L"file:///C:/One"); request.explicitWorkspaceConfigUri = Parse(L"file:///C:/demo.code-workspace");
		cases.push_back({ std::move(request), EWorkbenchBootstrapStatus::InvalidWorkspaceShape });
	}
	{
		auto request = Request(); request.workspaceFolders.push_back({ Parse(L"file:///C:/One"), L"one" });
		cases.push_back({ std::move(request), EWorkbenchBootstrapStatus::InvalidWorkspaceShape });
	}
	{
		auto request = Request(); request.explicitFolderUri = Parse(L"file:///C:/One?bad");
		cases.push_back({ std::move(request), EWorkbenchBootstrapStatus::InvalidFolderUri });
	}
	{
		auto request = Request(); request.explicitWorkspaceConfigUri = Parse(L"file:///C:/demo.code-workspace#bad");
		cases.push_back({ std::move(request), EWorkbenchBootstrapStatus::InvalidWorkspaceConfigUri });
	}
	{
		auto request = Request(); request.explicitWorkspaceConfigUri = Parse(L"file:///C:/demo.code-workspace"); request.workspaceFolders = { { Parse(L"file:///C:/One"), L"one" }, { Parse(L"file://localhost/c:/one"), L"alias" } };
		cases.push_back({ std::move(request), EWorkbenchBootstrapStatus::DuplicateWorkspaceFolderIdentity });
	}
	{
		auto request = Request(); request.explicitWorkspaceConfigUri = Parse(L"file:///C:/demo.code-workspace"); request.workspaceFolders = { { Parse(L"file:///C:/One"), L"" } };
		cases.push_back({ std::move(request), EWorkbenchBootstrapStatus::InvalidFolderDisplayName });
	}
	{
		auto request = Request(); request.initialDocumentUri = Parse(L"untitled:");
		cases.push_back({ std::move(request), EWorkbenchBootstrapStatus::InvalidInitialDocumentUri });
	}
	{
		auto request = Request(); request.terminalLaunchDirectoryUri = Parse(L"file:///C:/Launch?bad");
		cases.push_back({ std::move(request), EWorkbenchBootstrapStatus::InvalidTerminalLaunchDirectoryUri });
	}
	for (auto& testCase : cases) {
		auto result = ResolveWorkbenchBootstrapContext(std::move(testCase.request));
		EXPECT_EQ(testCase.expected, result.status);
		EXPECT_FALSE(result.Resolved());
		EXPECT_FALSE(result.context);
	}
}

TEST(WorkbenchBootstrapContext, InvalidProfileResolverResultCannotBecomeABootstrapContext)
{
	auto profile = ResolveProfileBootstrapSnapshot("invalid", 1, L"C:\\Profiles\\Sakura");
	EXPECT_EQ(platform::profiles::ProfileBootstrapSnapshotStatus::InvalidProfileId, profile.status);
	EXPECT_FALSE(profile.snapshot);
}

} // namespace
