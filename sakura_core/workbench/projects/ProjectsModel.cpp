/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/projects/ProjectsModel.h"

#include <algorithm>
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

std::wstring ProjectsAccessibleLabel(const ProjectsRow& row)
{
	std::wstring result = row.label;
	if (!row.description.empty()) result += L", " + row.description;
	if (row.kind == EProjectsRowKind::WorktreesToggle) {
		result += L", " + std::to_wstring(row.hiddenWorktreeCount) + L" linked worktrees";
	}
	if (row.primaryWorktree) result += L", Primary";
	if (row.kind == EProjectsRowKind::CurrentWorktree) result += L", This Window";
	if (!row.enabled) result += L", Unavailable";
	return result;
}

ProjectsProjection ProjectProjects(
	const std::span<const ProjectEntry> projects,
	const config::WorkspaceContextSnapshot& workspace,
	const agent::AgentWorkspacesProjectionResult* worktrees,
	const bool worktreesExpanded,
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
