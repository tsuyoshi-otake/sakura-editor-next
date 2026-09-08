/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include "senp/github/GhRepositorySelection.h"

#include <map>
#include <tuple>

namespace {
using namespace senp::github;

std::vector<std::uint8_t> Bytes(std::string_view value)
{
	return { value.begin(), value.end() };
}

platform::uri::Uri FileUri(std::wstring_view path)
{
	auto parsed = platform::uri::Uri::FromWindowsPath(path);
	EXPECT_TRUE(parsed);
	return std::move(*parsed.value);
}

config::WorkspaceContextSnapshot Workspace(
	std::initializer_list<std::wstring_view> paths, std::uint64_t generation = 3, std::uint64_t revision = 7)
{
	config::WorkspaceContextSnapshot result;
	result.kind = paths.size() == 1 ? config::EWorkspaceKind::Folder : config::EWorkspaceKind::Workspace;
	result.generation = generation;
	result.revision = revision;
	for (const auto path : paths) result.folders.push_back({ FileUri(path), std::wstring(path) });
	return result;
}

class Platform final : public IGhLocalRepositoryPlatform {
public:
	void Add(std::wstring root, std::wstring repositoryPath, std::wstring repositoryIdentity,
		std::string remotes)
	{
		m_entries.emplace(std::move(root), std::tuple{ GhLocalRepositoryStatus::Succeeded,
			std::move(repositoryPath), std::move(repositoryIdentity), std::move(remotes) });
	}
	void AddStatus(std::wstring root, GhLocalRepositoryStatus status)
	{
		m_entries.emplace(std::move(root), std::tuple{ status, std::wstring{}, std::wstring{}, std::string{} });
	}
	GhLocalRepositoryRead Inspect(std::wstring_view root, HANDLE) const override
	{
		const auto found = m_entries.find(std::wstring(root));
		if (found == m_entries.end()) return { GhLocalRepositoryStatus::NotRepository, {}, {}, {} };
		return { std::get<0>(found->second), std::get<1>(found->second), std::get<2>(found->second),
			Bytes(std::get<3>(found->second)) };
	}
private:
	std::map<std::wstring, std::tuple<GhLocalRepositoryStatus, std::wstring, std::wstring, std::string>> m_entries;
};

std::shared_ptr<Platform> StandardPlatform()
{
	auto platform = std::make_shared<Platform>();
	platform->Add(L"C:\\Work\\Fork", L"C:\\Work\\Fork", L"c:\\work\\fork",
		"origin\thttps://github.com/me/project.git (fetch)\n"
		"origin\thttps://github.com/me/project.git (push)\n");
	return platform;
}

} // namespace

TEST(GhRepositorySelection, CapturesSingleRootAndSelectsItsOnlyGitHubRepository)
{
	auto platform = StandardPlatform();
	CGhRepositorySelection service(platform);
	const auto snapshot = service.Capture(Workspace({ L"C:\\Work\\Fork" }), nullptr);
	ASSERT_EQ(GhRepositorySnapshotStatus::Succeeded, snapshot.Status());
	ASSERT_EQ(1U, snapshot.Repositories().size());
	ASSERT_EQ(1U, snapshot.Repositories().front().Remotes().size());
	const auto selected = service.Select(snapshot, 3, 7);
	ASSERT_EQ(GhRepositorySelectionStatus::Selected, selected.Status());
	ASSERT_TRUE(selected.Selected());
	EXPECT_EQ(L"origin", selected.Selected()->RemoteName());
	EXPECT_EQ(L"github.com", selected.Selected()->Hostname());
	EXPECT_EQ(L"me", selected.Selected()->Owner());
	EXPECT_EQ(L"project", selected.Selected()->Repository());
}

TEST(GhRepositorySelection, KeepsForkAndUpstreamAsAnExplicitAmbiguousChoice)
{
	auto platform = std::make_shared<Platform>();
	platform->Add(L"C:\\Work\\Fork", L"C:\\Work\\Fork", L"c:\\work\\fork",
		"origin\thttps://github.com/me/project.git (fetch)\n"
		"upstream\thttps://github.com/team/project.git (fetch)\n");
	CGhRepositorySelection service(platform);
	const auto snapshot = service.Capture(Workspace({ L"C:\\Work\\Fork" }), nullptr);
	EXPECT_EQ(GhRepositorySelectionStatus::MultipleRepositories, service.Select(snapshot, 3, 7).Status());
	const auto upstream = service.Select(snapshot, 3, 7, std::nullopt, L"upstream");
	ASSERT_EQ(GhRepositorySelectionStatus::Selected, upstream.Status());
	EXPECT_EQ(L"team", upstream.Selected()->Owner());
	const auto origin = service.Select(snapshot, 3, 7, std::nullopt, L"origin");
	ASSERT_EQ(GhRepositorySelectionStatus::Selected, origin.Status());
	EXPECT_EQ(L"me", origin.Selected()->Owner());
}

TEST(GhRepositorySelection, NeverGuessesTheTargetOfAnSshAlias)
{
	auto platform = std::make_shared<Platform>();
	platform->Add(L"C:\\Work\\Fork", L"C:\\Work\\Fork", L"c:\\work\\fork",
		"origin\tgit@work-github:me/project.git (fetch)\n");
	CGhRepositorySelection service(platform);
	const auto snapshot = service.Capture(Workspace({ L"C:\\Work\\Fork" }), nullptr);
	ASSERT_EQ(GhRemoteKind::UnresolvedSshAlias, snapshot.Repositories().front().Remotes().front().Kind());
	EXPECT_EQ(GhRepositorySelectionStatus::RequiresSshAliasResolution,
		service.Select(snapshot, 3, 7).Status());
	EXPECT_EQ(GhRepositorySelectionStatus::RequiresSshAliasResolution,
		service.Select(snapshot, 3, 7, std::nullopt, L"origin").Status());
}

TEST(GhRepositorySelection, AcceptsOnlyTheLiteralGitHubHostForScpStyleSsh)
{
	auto platform = std::make_shared<Platform>();
	platform->Add(L"C:\\Work\\Fork", L"C:\\Work\\Fork", L"c:\\work\\fork",
		"origin\tgit@github.com:me/project.git (fetch)\n");
	CGhRepositorySelection service(platform);
	const auto snapshot = service.Capture(Workspace({ L"C:\\Work\\Fork" }), nullptr);
	const auto selected = service.Select(snapshot, 3, 7);
	ASSERT_EQ(GhRepositorySelectionStatus::Selected, selected.Status());
	EXPECT_EQ(L"github.com", selected.Selected()->Hostname());
}

TEST(GhRepositorySelection, RequiresRootSelectionForDistinctMultiRootRepositories)
{
	auto platform = StandardPlatform();
	platform->Add(L"D:\\Other", L"D:\\Other", L"d:\\other",
		"origin\thttps://github.com/team/other (fetch)\n");
	CGhRepositorySelection service(platform);
	const auto snapshot = service.Capture(Workspace({ L"C:\\Work\\Fork", L"D:\\Other" }), nullptr);
	ASSERT_EQ(2U, snapshot.Repositories().size());
	EXPECT_EQ(GhRepositorySelectionStatus::MultipleRoots, service.Select(snapshot, 3, 7).Status());
	const auto selected = service.Select(snapshot, 3, 7, L"d:\\other");
	ASSERT_EQ(GhRepositorySelectionStatus::Selected, selected.Status());
	EXPECT_EQ(L"other", selected.Selected()->Repository());
}

TEST(GhRepositorySelection, CollapsesDuplicateWorkspaceRootsInsideOneRepository)
{
	auto platform = StandardPlatform();
	platform->Add(L"C:\\Work\\Fork\\child", L"C:\\Work\\Fork", L"c:\\work\\fork",
		"origin\thttps://github.com/me/project.git (fetch)\n");
	CGhRepositorySelection service(platform);
	const auto snapshot = service.Capture(Workspace({ L"C:\\Work\\Fork", L"C:\\Work\\Fork\\child",
		L"C:\\WORK\\FORK" }), nullptr);
	ASSERT_EQ(1U, snapshot.Repositories().size());
	EXPECT_EQ(2U, snapshot.Repositories().front().WorkspaceRootIdentities().size());
}

TEST(GhRepositorySelection, DistinguishesRemoteAndRootRemovalFromAStaleSnapshot)
{
	auto platform = StandardPlatform();
	CGhRepositorySelection service(platform);
	const auto snapshot = service.Capture(Workspace({ L"C:\\Work\\Fork" }), nullptr);
	EXPECT_EQ(GhRepositorySelectionStatus::SelectedRemoteRemoved,
		service.Select(snapshot, 3, 7, std::nullopt, L"deleted").Status());
	EXPECT_EQ(GhRepositorySelectionStatus::SelectedRootRemoved,
		service.Select(snapshot, 3, 7, L"d:\\removed").Status());
	EXPECT_EQ(GhRepositorySelectionStatus::StaleSnapshot,
		service.Select(snapshot, 3, 8).Status());
}

TEST(GhRepositorySelection, KeepsUnsupportedFailureAndNoRepositoryStatesDistinct)
{
	auto platform = std::make_shared<Platform>();
	platform->AddStatus(L"C:\\Plain", GhLocalRepositoryStatus::NotRepository);
	platform->AddStatus(L"D:\\Failed", GhLocalRepositoryStatus::TimedOut);
	CGhRepositorySelection service(platform);
	auto empty = config::WorkspaceContextSnapshot{};
	EXPECT_EQ(GhRepositorySnapshotStatus::EmptyWorkspace, service.Capture(empty, nullptr).Status());
	EXPECT_EQ(GhRepositorySnapshotStatus::NoRepository,
		service.Capture(Workspace({ L"C:\\Plain" }), nullptr).Status());
	EXPECT_EQ(GhRepositorySnapshotStatus::Failed,
		service.Capture(Workspace({ L"D:\\Failed" }), nullptr).Status());
	auto remote = platform::uri::Uri::Parse(L"vscode-remote://ssh-remote+host/work");
	ASSERT_TRUE(remote);
	auto unsupported = Workspace({ L"C:\\Plain" });
	unsupported.folders = { { std::move(*remote.value), L"remote" } };
	EXPECT_EQ(GhRepositorySnapshotStatus::NoFileRoots, service.Capture(unsupported, nullptr).Status());
}

TEST(GhRepositorySelection, RejectsMalformedRemoteOutputWithoutPublishingARepository)
{
	auto platform = std::make_shared<Platform>();
	platform->Add(L"C:\\Work\\Fork", L"C:\\Work\\Fork", L"c:\\work\\fork",
		"origin https://github.com/me/project (fetch)\n");
	CGhRepositorySelection service(platform);
	const auto snapshot = service.Capture(Workspace({ L"C:\\Work\\Fork" }), nullptr);
	EXPECT_EQ(GhRepositorySnapshotStatus::Failed, snapshot.Status());
	EXPECT_TRUE(snapshot.Repositories().empty());
}
