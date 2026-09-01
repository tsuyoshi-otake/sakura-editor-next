#include "pch.h"

#include "workbench/agent/AgentWorkspacesModel.h"

#include <map>

namespace workbench::agent {
namespace {

worktree::GitWorktreeRecord Record(std::wstring path, std::wstring identity,
	std::string branch = "refs/heads/main")
{
	return {
		.path = std::move(path),
		.identity = std::move(identity),
		.head = "0123456789abcdef",
		.branch = std::move(branch),
	};
}

TEST(AgentWorkspacesModel, SelectsTheLongestContainingWorktreeForAnOpenedSubdirectory)
{
	const std::vector records{
		Record(L"C:\\repo", L"c:\\repo"),
		Record(L"C:\\repo\\feature", L"c:\\repo\\feature", "refs/heads/feature"),
	};
	const AgentDirectoryIdentityResolver resolver = [](std::wstring_view path) {
		EXPECT_EQ(L"C:\\repo\\feature\\src", path);
		return std::optional(std::wstring(L"c:\\repo\\feature\\src"));
	};

	const auto result = ProjectAgentWorkspaces(records, L"C:\\repo\\feature\\src", {}, resolver);

	ASSERT_TRUE(result.Succeeded());
	ASSERT_EQ(1U, result.currentIndex);
	ASSERT_EQ(1U, result.selectedIndex);
	ASSERT_EQ(2U, result.rows.size());
	EXPECT_EQ(EAgentWorktreeWindowState::OpenInNewWindow, result.rows[0].windowState);
	EXPECT_EQ(EAgentWorktreeWindowState::ThisWindow, result.rows[1].windowState);
	EXPECT_EQ(L"feature", result.rows[1].name);
	EXPECT_EQ(L"feature", result.rows[1].branch);
	EXPECT_EQ(L"0123456", result.rows[1].head);
}

TEST(AgentWorkspacesModel, DoesNotTreatACommonTextPrefixAsAContainingWorktree)
{
	const std::vector records{
		Record(L"C:\\repo", L"c:\\repo"),
		Record(L"C:\\repo2", L"c:\\repo2"),
	};
	const AgentDirectoryIdentityResolver resolver = [](std::wstring_view path) {
		if (path == L"C:\\repo2\\src") return std::optional(std::wstring(L"c:\\repo2\\src"));
		return std::optional<std::wstring>{};
	};

	const auto result = ProjectAgentWorkspaces(records, L"C:\\repo2\\src", {}, resolver);

	ASSERT_TRUE(result.Succeeded());
	ASSERT_EQ(1U, result.currentIndex);
	EXPECT_EQ(EAgentWorktreeWindowState::OpenInNewWindow, result.rows[0].windowState);
	EXPECT_EQ(EAgentWorktreeWindowState::ThisWindow, result.rows[1].windowState);
}

TEST(AgentWorkspacesModel, ResolvesAPhysicalAliasWithABoundedFallback)
{
	const std::vector records{
		Record(L"C:\\registered", L"c:\\registered"),
		Record(L"C:\\other", L"c:\\other"),
	};
	std::size_t calls = 0;
	const AgentDirectoryIdentityResolver resolver = [&calls](std::wstring_view path) {
		++calls;
		if (path == L"S:\\src") return std::optional(std::wstring(L"c:\\physical\\repo\\src"));
		if (path == L"C:\\registered") return std::optional(std::wstring(L"c:\\physical\\repo"));
		if (path == L"C:\\other") return std::optional(std::wstring(L"c:\\other"));
		return std::optional<std::wstring>{};
	};

	const auto result = ProjectAgentWorkspaces(records, L"S:\\src", {}, resolver);

	ASSERT_TRUE(result.Succeeded());
	ASSERT_EQ(0U, result.currentIndex);
	EXPECT_EQ(3U, calls);
}

TEST(AgentWorkspacesModel, PreservesSelectionAndProjectsDetachedAndSafetyFlags)
{
	auto current = Record(L"C:\\repo", L"c:\\repo");
	auto selected = Record(L"C:\\other", L"c:\\other", "");
	selected.branch.reset();
	selected.detached = true;
	selected.locked = true;
	selected.prunable = true;
	const std::vector records{ current, selected };
	const AgentDirectoryIdentityResolver resolver = [](std::wstring_view) {
		return std::optional(std::wstring(L"c:\\repo"));
	};

	const auto result = ProjectAgentWorkspaces(records, L"C:\\repo", L"c:\\other", resolver);

	ASSERT_TRUE(result.Succeeded());
	ASSERT_EQ(1U, result.selectedIndex);
	EXPECT_EQ(L"Detached", result.rows[1].branch);
	EXPECT_TRUE(result.rows[1].detached);
	EXPECT_TRUE(result.rows[1].locked);
	EXPECT_TRUE(result.rows[1].prunable);
}

TEST(AgentWorkspacesModel, FailsClosedWithoutResolvingAnUnboundedRecordSet)
{
	std::vector<worktree::GitWorktreeRecord> records;
	for (std::size_t index = 0; index < 129; ++index) {
		const auto suffix = std::to_wstring(index);
		records.push_back(Record(L"C:\\worktree" + suffix, L"c:\\worktree" + suffix));
	}
	std::size_t calls = 0;
	const AgentDirectoryIdentityResolver resolver = [&calls](std::wstring_view) {
		++calls;
		return std::optional(std::wstring(L"c:\\missing"));
	};

	const auto result = ProjectAgentWorkspaces(records, L"C:\\missing", {}, resolver);

	EXPECT_EQ(EAgentWorkspacesProjectionStatus::CurrentWorktreeUnavailable, result.status);
	EXPECT_TRUE(result.rows.empty());
	EXPECT_EQ(1U, calls);
}

TEST(AgentWorkspacesModel, ReportsEmptyAndInvalidWorkspaceInputsExplicitly)
{
	const std::vector<worktree::GitWorktreeRecord> records;
	EXPECT_EQ(EAgentWorkspacesProjectionStatus::NoWorkspace,
		ProjectAgentWorkspaces(records, L"").status);
	const AgentDirectoryIdentityResolver failing = [](std::wstring_view) {
		return std::optional<std::wstring>{};
	};
	EXPECT_EQ(EAgentWorkspacesProjectionStatus::InvalidWorkspacePath,
		ProjectAgentWorkspaces(records, L"C:\\missing", {}, failing).status);
}

} // namespace
} // namespace workbench::agent
