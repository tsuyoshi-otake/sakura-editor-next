/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/projects/ProjectsModel.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>

namespace workbench::projects {
namespace {

bool IsCurrentProject(const ProjectEntry& project,
	const config::WorkspaceContextSnapshot& workspace) noexcept
{
	if (project.kind == EProjectKind::Workspace) {
		return workspace.kind == config::EWorkspaceKind::Workspace
			&& workspace.workspaceConfigUri
			&& platform::uri::UriIdentityService::IsEqual(
				project.uri, *workspace.workspaceConfigUri);
	}
	return workspace.kind == config::EWorkspaceKind::Folder
		&& workspace.folders.size() == 1
		&& platform::uri::UriIdentityService::IsEqual(project.uri, workspace.folders.front().uri);
}

std::wstring ProjectDescription(const ProjectEntry& project, const bool current)
{
	if (current) return project.kind == EProjectKind::Folder
		? L"Current Folder" : L"Current Workspace";
	return project.kind == EProjectKind::Folder ? L"Folder" : L"Workspace";
}

void AppendDescription(std::wstring& target, const std::wstring_view value)
{
	if (value.empty()) return;
	if (!target.empty()) target += L" - ";
	target += value;
}

struct CaseInsensitiveEqual final {
	bool operator()(const std::wstring_view left, const std::wstring_view right) const noexcept
	{
		return left.size() == right.size()
			&& std::ranges::equal(left, right, [](const wchar_t a, const wchar_t b) {
				return std::towlower(a) == std::towlower(b);
			});
	}
};

std::wstring WorktreeLabel(const agent::AgentWorktreeRow& worktree)
{
	if (!worktree.detached && !worktree.bare && !worktree.branch.empty()) return worktree.branch;
	if (!worktree.name.empty()) return worktree.name;
	if (!worktree.branch.empty()) return worktree.branch;
	if (!worktree.head.empty()) return worktree.head;
	return worktree.path;
}

std::wstring WorktreeDescription(const agent::AgentWorktreeRow& worktree,
	const std::wstring_view projectName, const std::wstring_view label)
{
	std::wstring result;
	if (!worktree.name.empty() && worktree.name != label && worktree.name != projectName) {
		AppendDescription(result, worktree.name);
	}
	if (worktree.detached) {
		std::wstring detached = L"Detached";
		if (!worktree.head.empty()) detached += L" @ " + worktree.head;
		AppendDescription(result, detached);
	} else if (worktree.bare) {
		AppendDescription(result, L"Bare");
	}
	if (worktree.locked) AppendDescription(result, L"Locked");
	else if (worktree.prunable) AppendDescription(result, L"Prunable");
	return result;
}

} // namespace

std::wstring ProjectDisplayName(const ProjectEntry& project)
{
	if (project.label) return *project.label;
	if (const auto path = project.uri.ToWindowsPath(); path.value) {
		std::filesystem::path native(*path.value);
		auto name = native.filename().wstring();
		if (project.kind == EProjectKind::Workspace
			&& _wcsicmp(native.extension().c_str(), L".code-workspace") == 0) {
			name = native.stem().wstring();
		}
		if (name.empty()) name = native.root_name().wstring();
		if (!name.empty()) return name;
	}
	return project.uri.ToString();
}

std::wstring ProjectWorktreeBranchLabel(const agent::AgentWorktreeRow& worktree)
{
	if (worktree.detached) {
		return worktree.head.empty() ? L"Detached" : L"Detached @ " + worktree.head;
	}
	if (worktree.bare) return L"Bare";
	return worktree.branch;
}

std::wstring ProjectsAccessibleLabel(const ProjectsRow& row)
{
	std::wstring result = row.label;
	if (!row.description.empty()) result += L", " + row.description;
	if (!row.trailing.empty()) result += L", " + row.trailing;
	if (row.kind == EProjectsRowKind::WorktreesToggle) {
		result += L", " + std::to_wstring(row.hiddenWorktreeCount) + L" linked worktrees";
	}
	if (row.primaryWorktree) result += L", Primary";
	if (row.kind == EProjectsRowKind::CurrentWorktree) result += L", This Window";
	if (!row.enabled) result += L", Unavailable";
	return result;
}

ProjectBranchSummary SummarizeProjectBranches(
	const std::span<const ProjectRepositoryBranchObservation> observations,
	const bool complete,
	const bool truncated)
{
	ProjectBranchSummary result;
	result.repositoryCount = observations.size();
	if (!complete) return result;
	if (truncated) {
		result.status = EProjectBranchSummaryStatus::Bounded;
		result.label = std::to_wstring(observations.size()) + L"+ repositories";
		return result;
	}
	if (observations.empty()) {
		result.status = EProjectBranchSummaryStatus::NoRepository;
		result.label = L"No Git";
		return result;
	}
	if (std::ranges::any_of(observations, [](const auto& observation) {
		return observation.unavailable;
	})) {
		result.status = EProjectBranchSummaryStatus::Unavailable;
		result.label = L"Git unavailable";
		return result;
	}
	std::vector<std::wstring_view> labels;
	labels.reserve(observations.size());
	for (const auto& observation : observations) {
		if (!observation.succeeded || observation.label.empty()) continue;
		if (std::ranges::find_if(labels, [&observation](const auto existing) {
			return CaseInsensitiveEqual{}(existing, observation.label);
		}) == labels.end()) {
			labels.push_back(observation.label);
		}
	}
	if (labels.empty()) {
		result.status = EProjectBranchSummaryStatus::NoRepository;
		result.label = L"No Git";
		return result;
	}
	if (labels.size() == 1) {
		result.status = EProjectBranchSummaryStatus::Ready;
		result.label.assign(labels.front());
		return result;
	}
	result.status = EProjectBranchSummaryStatus::Mixed;
	result.label = std::to_wstring(labels.size()) + L" branches";
	return result;
}

ProjectBranchDiscoveryPlan PlanProjectBranchDiscovery(
	const std::span<const ProjectBranchDiscoveryTarget> targets,
	const std::size_t maximumRepositoriesPerProject,
	const std::size_t maximumRequests)
{
	ProjectBranchDiscoveryPlan result;
	result.truncatedProjects.resize(targets.size(), false);
	if (maximumRepositoriesPerProject == 0 || maximumRequests == 0) {
		for (std::size_t index = 0; index < targets.size(); ++index) {
			result.truncatedProjects[index] = !targets[index].repositoryRoots.empty();
		}
		return result;
	}
	std::vector<std::vector<std::wstring>> roots(targets.size());
	for (std::size_t index = 0; index < targets.size(); ++index) {
		for (const auto& root : targets[index].repositoryRoots) {
			if (root.empty() || std::ranges::find_if(roots[index], [&root](const auto& existing) {
				return CaseInsensitiveEqual{}(existing, root);
			}) != roots[index].end()) continue;
			if (roots[index].size() == maximumRepositoriesPerProject) {
				result.truncatedProjects[index] = true;
				continue;
			}
			roots[index].push_back(root);
		}
	}
	std::vector<std::size_t> order;
	order.reserve(targets.size());
	for (std::size_t index = 0; index < targets.size(); ++index) {
		if (targets[index].currentProject) order.push_back(index);
	}
	for (std::size_t index = 0; index < targets.size(); ++index) {
		if (!targets[index].currentProject) order.push_back(index);
	}
	for (std::size_t repositoryIndex = 0; repositoryIndex < maximumRepositoriesPerProject; ++repositoryIndex) {
		for (const auto projectIndex : order) {
			if (repositoryIndex >= roots[projectIndex].size()) continue;
			if (result.requests.size() == maximumRequests) {
				result.truncatedProjects[projectIndex] = true;
				continue;
			}
			result.requests.push_back({
				.projectIndex = projectIndex,
				.repositoryIndex = repositoryIndex,
				.identity = targets[projectIndex].identity,
				.repositoryRoot = roots[projectIndex][repositoryIndex],
			});
		}
	}
	for (std::size_t projectIndex = 0; projectIndex < roots.size(); ++projectIndex) {
		const auto scheduled = std::ranges::count_if(result.requests,
			[projectIndex](const auto& request) { return request.projectIndex == projectIndex; });
		if (static_cast<std::size_t>(scheduled) < roots[projectIndex].size()) {
			result.truncatedProjects[projectIndex] = true;
		}
	}
	return result;
}

ProjectsProjection ProjectProjects(
	const std::span<const ProjectEntry> projects,
	const config::WorkspaceContextSnapshot& workspace,
	const agent::AgentWorkspacesProjectionResult* worktrees,
	const bool worktreesExpanded,
	const std::span<const ProjectBranchSummary> branchSummaries,
	const std::optional<EProjectsRowKind> preferredKind,
	const std::wstring_view preferredWorktreeIdentity)
{
	ProjectsProjection result;
	for (std::size_t index = 0; index < projects.size(); ++index) {
		if (IsCurrentProject(projects[index], workspace)) {
			result.currentProjectIndex = index;
			break;
		}
	}
	result.rows.reserve(projects.size() + (worktrees ? worktrees->rows.size() + 1U : 0U));
	for (std::size_t projectIndex = 0; projectIndex < projects.size(); ++projectIndex) {
		const bool current = result.currentProjectIndex
			&& *result.currentProjectIndex == projectIndex;
		const auto projectName = ProjectDisplayName(projects[projectIndex]);
		result.rows.push_back({
			.kind = EProjectsRowKind::Project,
			.projectIndex = projectIndex,
			.label = projectName,
			.description = ProjectDescription(projects[projectIndex], current),
			.trailing = projectIndex < branchSummaries.size()
				? branchSummaries[projectIndex].label : std::wstring{},
			.currentProject = current,
		});
		if (!current || worktrees == nullptr || !worktrees->Succeeded()
			|| !worktrees->currentIndex || *worktrees->currentIndex >= worktrees->rows.size()) {
			continue;
		}

		const auto currentWorktree = *worktrees->currentIndex;
		const auto& currentRow = worktrees->rows[currentWorktree];
		const auto currentLabel = WorktreeLabel(currentRow);
		result.rows.push_back({
			.kind = EProjectsRowKind::CurrentWorktree,
			.projectIndex = projectIndex,
			.worktreeIndex = currentWorktree,
			.label = currentLabel,
			.description = WorktreeDescription(currentRow, projectName, currentLabel),
			.currentProject = true,
			.primaryWorktree = currentWorktree == 0,
			.enabled = !currentRow.locked && !currentRow.prunable && !currentRow.bare,
		});

		const auto hiddenCount = worktrees->rows.size() - 1U;
		if (hiddenCount == 0) continue;
		result.rows.push_back({
			.kind = EProjectsRowKind::WorktreesToggle,
			.projectIndex = projectIndex,
			.label = worktreesExpanded ? L"Hide linked worktrees" : L"Show linked worktrees",
			.hiddenWorktreeCount = hiddenCount,
			.currentProject = true,
			.expanded = worktreesExpanded,
		});
		if (!worktreesExpanded) continue;
		for (std::size_t worktreeIndex = 0; worktreeIndex < worktrees->rows.size(); ++worktreeIndex) {
			if (worktreeIndex == currentWorktree) continue;
			const auto& worktree = worktrees->rows[worktreeIndex];
			const auto worktreeLabel = WorktreeLabel(worktree);
			result.rows.push_back({
				.kind = EProjectsRowKind::Worktree,
				.projectIndex = projectIndex,
				.worktreeIndex = worktreeIndex,
				.label = worktreeLabel,
				.description = WorktreeDescription(worktree, projectName, worktreeLabel),
				.currentProject = true,
				.primaryWorktree = worktreeIndex == 0,
				.enabled = !worktree.locked && !worktree.prunable && !worktree.bare,
			});
		}
	}

	if (preferredKind) {
		for (std::size_t rowIndex = 0; rowIndex < result.rows.size(); ++rowIndex) {
			const auto& row = result.rows[rowIndex];
			if (row.kind != *preferredKind) continue;
			if (row.worktreeIndex && worktrees
				&& *row.worktreeIndex < worktrees->rows.size()
				&& !preferredWorktreeIdentity.empty()
				&& worktrees->rows[*row.worktreeIndex].identity != preferredWorktreeIdentity) {
				continue;
			}
			result.selectedRowIndex = rowIndex;
			break;
		}
	}
	if (!result.selectedRowIndex && result.currentProjectIndex) {
		const auto selected = std::ranges::find_if(result.rows, [&result](const auto& row) {
			return row.kind == EProjectsRowKind::Project
				&& row.projectIndex == *result.currentProjectIndex;
		});
		if (selected != result.rows.end()) {
			result.selectedRowIndex = static_cast<std::size_t>(selected - result.rows.begin());
		}
	}
	if (!result.selectedRowIndex && !result.rows.empty()) result.selectedRowIndex = 0;
	return result;
}

} // namespace workbench::projects
