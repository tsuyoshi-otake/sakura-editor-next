/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/scm/GitScmPublisher.h"

#include "workbench/commands/ApiCommandArguments.h"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace workbench::scm {
namespace {

std::string ToUtf8(std::wstring_view value)
{
	if (value.empty()) return {};
	const int required = ::WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
		nullptr, 0, nullptr, nullptr);
	if (required <= 0) return {};
	std::string result(static_cast<std::size_t>(required), '\0');
	::WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
		result.data(), required, nullptr, nullptr);
	return result;
}

//! The last path component of the repository root. Upstream's
//! `MainThreadSCMProvider` derives the provider `name` the same way when no
//! workspace folder matches the root, and the repository row renders that name.
std::wstring RepositoryName(std::wstring_view root)
{
	while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.remove_suffix(1);
	const auto separator = root.find_last_of(L"\\/");
	return std::wstring(separator == std::wstring_view::npos ? root : root.substr(separator + 1));
}

ScmResourceGroupState MakeGroup(const ScmOwner& owner, std::string_view handle, std::string_view id,
	std::string_view label, bool hideWhenEmpty)
{
	ScmResourceGroupState group;
	group.owner = owner;
	group.providerHandle = std::string(handle);
	group.id = std::string(id);
	group.label = std::string(label);
	group.hideWhenEmpty = hideWhenEmpty;
	return group;
}

std::string LocalizedLabel(const GitDiffTextResolver& text, std::string_view key,
	std::string_view fallback)
{
	if (!text) return std::string(fallback);
	const auto value = text(key, {});
	return value.empty() ? std::string(fallback) : ToUtf8(value);
}

//!
//! @brief Upstream's `CommandResolver.resolveChangeCommand`.
//!
//! Its title is `localize('open', "Open")` on **both** branches, so the two
//! commands share one label; what differs is which command runs and what it is
//! handed. A row whose comparison has neither side publishes no command at all,
//! which is how the view renders "there is nothing here to open" rather than
//! offering an action that would do nothing.
//!
//! This is `git.openDiffOnClick`'s default half. The former `git.openFile`
//! publication was a stand-in for a diff editor that did not exist; it does now.
//!
std::optional<ScmCommand> MakeChangeCommand(const GitDiffRow& row, std::wstring_view repositoryRoot,
	const GitDiffTextResolver& text)
{
	const auto input = ResolveGitDiffInput(row, text);
	ScmCommand command;
	command.title = LocalizedLabel(text, "GitScmOpen", "Open");
	switch (input.kind) {
	case EGitDiffCommandKind::Diff: {
		commands::ApiDiffArguments arguments;
		arguments.originalUri = BuildGitDiffEndpointUri(*input.original, repositoryRoot);
		arguments.modifiedUri = BuildGitDiffEndpointUri(*input.modified, repositoryRoot);
		arguments.title = input.title;
		// A side whose path will not join onto the root cannot be read either, so
		// publishing the command would only move the refusal to the click.
		if (arguments.originalUri.empty() || arguments.modifiedUri.empty()) return std::nullopt;
		command.command = "vscode.diff";
		command.argumentsJson = commands::BuildApiDiffArguments(arguments);
		return command;
	}
	case EGitDiffCommandKind::Open: {
		commands::ApiOpenArguments arguments;
		arguments.resourceUri = BuildGitDiffEndpointUri(*input.modified, repositoryRoot);
		// Upstream passes `override: false` for a both-modified conflict and
		// leaves it undefined otherwise. Absent and `false` are different
		// requests, so the distinction is carried rather than flattened.
		if (row.status == EGitFileStatus::BothModified) arguments.overrideEditor = false;
		arguments.label = input.title;
		if (arguments.resourceUri.empty()) return std::nullopt;
		command.command = "vscode.open";
		command.argumentsJson = commands::BuildApiOpenArguments(arguments);
		return command;
	}
	case EGitDiffCommandKind::None:
	default:
		break;
	}
	return std::nullopt;
}

bool HasAnyGroup(const GitScmState& state, EUntrackedChangesPolicy policy,
	bool GitResourceGroupSet::* member) noexcept
{
	return std::any_of(state.changes.begin(), state.changes.end(),
		[policy, member](const GitChange& change) { return ClassifyChange(change, policy).*member; });
}

//!
//! @brief The rows one already classified change publishes, in group order.
//!
//! The single enumeration every per-row derivation walks, so the resource the
//! view renders, the operand a menu names, and the diff a command opens cannot
//! describe different rows. The order is the publication's own group order,
//! which is what lets a caller pair each derived value with the resource it
//! just published.
//!
template <typename Callback>
void ForEachPublishedRow(const GitChange& change, const GitResourceGroupSet& groups, Callback&& callback)
{
	const auto visit = [&](EGitResourceGroup group, bool member) {
		if (!member) return;
		// A group whose area publishes no row has no operand and no diff
		// either. Naming a row the view never rendered would let a menu act on
		// something the user cannot see.
		const auto status = GitGroupStatus(change, group);
		if (!status) return;
		callback(group, *status);
	};
	visit(EGitResourceGroup::Merge, groups.merge);
	visit(EGitResourceGroup::Index, groups.index);
	visit(EGitResourceGroup::WorkingTree, groups.workingTree);
	visit(EGitResourceGroup::Untracked, groups.untracked);
}

//! Append one already classified change's rows as command operands.
void AppendStageResources(const GitChange& change, const GitResourceGroupSet& groups,
	std::vector<GitStageResource>& resources)
{
	ForEachPublishedRow(change, groups, [&](EGitResourceGroup group, EGitFileStatus status) {
		// `deleted` is per row, not per file: a staged deletion and an unstaged
		// one are different rows, and the discard confirmation's "restore"
		// wording follows the row it is about. Asking the same predicate the
		// view's strike-through asks keeps them from diverging.
		resources.push_back({
			.path = change.path,
			.group = group,
			.untracked = change.untracked,
			.deleted = IsGitFileStatusStruckThrough(status),
		});
	});
}

//!
//! @brief Whether this change also publishes a Staged Changes row.
//!
//! Upstream's `sanitizeRef('~')` searches `repository.indexGroup` for the path,
//! not the group the clicked row belongs to, so this is one fact per change
//! rather than one per row.
//!
bool IsStagedInIndex(const GitChange& change, const GitResourceGroupSet& groups) noexcept
{
	return groups.index && GitGroupStatus(change, EGitResourceGroup::Index).has_value();
}

} // namespace

GitResourceGroupSet ClassifyChange(const GitChange& change, EUntrackedChangesPolicy policy) noexcept
{
	GitResourceGroupSet groups;
	if (change.conflicted) {
		groups.merge = true;
		return groups;
	}
	if (change.untracked) {
		switch (policy) {
		case EUntrackedChangesPolicy::Separate: groups.untracked = true; break;
		case EUntrackedChangesPolicy::Hidden: break;
		case EUntrackedChangesPolicy::Mixed:
		default: groups.workingTree = true; break;
		}
		return groups;
	}
	if (change.indexStatus != L'.' && change.indexStatus != L'\0') groups.index = true;
	if (change.worktreeStatus != L'.' && change.worktreeStatus != L'\0') groups.workingTree = true;
	// Porcelain v2 always fills XY for a tracked change, but a caller that built a
	// GitChange by hand must still land somewhere rather than vanish.
	if (!groups.Any()) groups.workingTree = true;
	return groups;
}

std::optional<EGitFileStatus> GitGroupStatus(const GitChange& change, EGitResourceGroup group) noexcept
{
	// Only Staged Changes reads the index column. Merge Changes and Untracked
	// Changes are decided before either switch runs, so the area they pass is
	// immaterial; passing the working-tree one keeps this a single expression
	// instead of three cases that would have to agree with each other.
	return ClassifyGitFileStatus(change,
		group == EGitResourceGroup::Index ? EGitChangeArea::Index : EGitChangeArea::WorkingTree);
}

std::optional<EGitFileStatus> GitDecorationStatus(const GitChange& change,
	const GitResourceGroupSet& groups) noexcept
{
	if (groups.merge) return GitGroupStatus(change, EGitResourceGroup::Merge);
	// Upstream's map is filled index, untracked, workingTree, merge and a later
	// group overwrites an earlier one, so the worktree row wins over the index
	// row for a path that has both. It falls back rather than giving up when the
	// worktree area publishes no row at all, because the index row is then the
	// only row this file has and it is the one carrying the badge.
	if (groups.workingTree) {
		if (const auto status = GitGroupStatus(change, EGitResourceGroup::WorkingTree)) return status;
	}
	if (groups.untracked) return GitGroupStatus(change, EGitResourceGroup::Untracked);
	if (groups.index) return GitGroupStatus(change, EGitResourceGroup::Index);
	return std::nullopt;
}

std::string_view GitCheckoutStatusIcon(const GitScmState& state, EUntrackedChangesPolicy policy) noexcept
{
	if (state.branch.empty()) return "$(git-commit)";
	if (HasAnyGroup(state, policy, &GitResourceGroupSet::merge)) return "$(git-branch-conflicts)";
	if (HasAnyGroup(state, policy, &GitResourceGroupSet::index)) return "$(git-branch-staged-changes)";
	if (HasAnyGroup(state, policy, &GitResourceGroupSet::workingTree)
		|| HasAnyGroup(state, policy, &GitResourceGroupSet::untracked)) {
		return "$(git-branch-changes)";
	}
	return "$(git-branch)";
}

std::string GitHeadShortName(const GitScmState& state)
{
	if (!state.branch.empty()) return ToUtf8(state.branch);
	// Upstream's `headShortName` writes `substr(0, 8)` literally. It does **not**
	// read `git.commitShortHashLength`, whose default is 7 and which only governs
	// the Quick Pick's object-name descriptions. Unifying the two would make this
	// item disagree with the real VS Code status bar by one character.
	const auto commit = ToUtf8(state.commit);
	return commit.size() > 8 ? commit.substr(0, 8) : commit;
}

std::string GitHeadLabel(const GitScmState& state, EUntrackedChangesPolicy policy)
{
	auto label = GitHeadShortName(state);
	if (label.empty()) return label;
	if (HasAnyGroup(state, policy, &GitResourceGroupSet::workingTree)
		|| HasAnyGroup(state, policy, &GitResourceGroupSet::untracked)) {
		label.push_back('*');
	}
	if (HasAnyGroup(state, policy, &GitResourceGroupSet::index)) label.push_back('+');
	if (HasAnyGroup(state, policy, &GitResourceGroupSet::merge)) label.push_back('!');
	return label;
}

ScmCommand BuildCheckoutStatusBarCommand(const GitScmState& state, EUntrackedChangesPolicy policy)
{
	ScmCommand command;
	command.command = "git.checkout";
	auto label = GitHeadLabel(state, policy);
	// Upstream's label is empty only when there is no HEAD at all, and its icon
	// is empty in exactly that case too. A blank status item would be
	// unclickable, so an unborn HEAD is named rather than left as a gap.
	if (label.empty()) label = "HEAD";
	command.title = std::string(GitCheckoutStatusIcon(state, policy)) + " " + label;
	command.tooltip = label + ", Checkout Branch/Tag...";
	command.argumentsJson = "[]";
	return command;
}

ScmCommand BuildSyncStatusBarCommand(const GitScmState& state)
{
	ScmCommand command;
	command.argumentsJson = "[]";
	if (state.upstream.empty()) {
		command.command = "git.publish";
		command.title = "$(cloud-upload) Publish Branch";
		// Upstream distinguishes `Publish to {0}` from `Publish to...` by the
		// number of registered remote-source publishers. There is no publisher
		// registry here, so it uses the general form rather than naming a
		// remote it has not resolved.
		command.tooltip = "Publish Branch";
		return command;
	}
	command.command = "git.sync";
	command.title = "$(sync)";
	// `syncTooltip`. The upstream ref is already the full `remote/name` string,
	// which is what upstream interpolates into these two placeholders.
	const auto upstream = ToUtf8(state.upstream);
	if (state.ahead == 0 && state.behind == 0) {
		command.tooltip = "Synchronize Changes";
		return command;
	}
	command.title += " " + std::to_string(state.behind) + "\xe2\x86\x93 "
		+ std::to_string(state.ahead) + "\xe2\x86\x91";
	// Upstream's first branch also covers a remote it knows to be read-only.
	// Remote metadata is not read here, so only the "nothing to push" half of
	// that condition is evaluated; a read-only remote with local commits gets
	// the push wording instead of the pull wording.
	if (state.ahead == 0) {
		command.tooltip = "Pull " + std::to_string(state.behind) + " commits from " + upstream;
	} else if (state.behind == 0) {
		command.tooltip = "Push " + std::to_string(state.ahead) + " commits to " + upstream;
	} else {
		command.tooltip = "Pull " + std::to_string(state.behind) + " and push "
			+ std::to_string(state.ahead) + " commits between " + upstream;
	}
	return command;
}

GitPublication BuildGitPublication(const ScmOwner& owner, std::wstring_view repositoryRoot,
	const GitScmState& state, EUntrackedChangesPolicy policy, const GitDiffTextResolver& text)
{
	GitPublication publication;
	auto& provider = publication.provider;
	provider.owner = owner;
	provider.handle = std::string(kGitProviderId);
	provider.id = std::string(kGitProviderId);
	// `label` is the source-control system, `name` is this repository. Upstream
	// derives `name` from `basename(rootUri)` and falls back to `label`; the
	// repository row renders `name` and puts `label` in its title.
	provider.label = std::string(kGitProviderLabel);
	provider.name = ToUtf8(RepositoryName(repositoryRoot));
	if (auto root = platform::uri::Uri::FromWindowsPath(repositoryRoot)) {
		provider.rootUri = *root.value;
	}

	provider.inputBox.value.clear();
	// `updateInputBoxPlaceholder`. `{0}` is the resolved `git.commit`
	// keybinding, and the branch is named in double quotes.
	const auto headShortName = GitHeadShortName(state);
	provider.inputBox.placeholder = headShortName.empty()
		? std::string("Message (Ctrl+Enter to commit)")
		: "Message (Ctrl+Enter to commit on \"" + headShortName + "\")";
	provider.inputBox.enabled = true;
	provider.inputBox.visible = true;

	ScmCommand accept;
	accept.command = "git.commit";
	accept.title = "Commit";
	accept.argumentsJson = "[]";
	provider.acceptInputCommand = accept;
	provider.statusBarCommands = { BuildCheckoutStatusBarCommand(state, policy), BuildSyncStatusBarCommand(state) };

	// Upstream declaration order; the SCM view renders groups in this order.
	auto merge = MakeGroup(owner, provider.handle, kGitMergeGroupId,
		LocalizedLabel(text, "GitScmMergeChanges", kGitMergeGroupLabel), true);
	// Upstream drives this one from `git.alwaysShowStagedChangesResourceGroup`,
	// whose default is false, so Staged Changes hides itself while empty. The
	// setting is not readable here yet; hard-coding its default is what keeps the
	// empty state identical to a stock VS Code instead of inventing a third
	// behaviour.
	auto index = MakeGroup(owner, provider.handle, kGitIndexGroupId,
		LocalizedLabel(text, "GitScmStagedChanges", kGitIndexGroupLabel), true);
	auto workingTree = MakeGroup(owner, provider.handle, kGitWorkingTreeGroupId,
		LocalizedLabel(text, "GitScmChanges", kGitWorkingTreeGroupLabel), false);
	auto untracked = MakeGroup(owner, provider.handle, kGitUntrackedGroupId,
		LocalizedLabel(text, "GitScmUntrackedChanges", kGitUntrackedGroupLabel), true);

	std::vector<GitStageResource> operandScratch;
	for (const auto& change : state.changes) {
		const auto groups = ClassifyChange(change, policy);
		if (!groups.Any()) continue;
		const auto absolute = JoinRepositoryPath(repositoryRoot, change.path);
		auto uri = platform::uri::Uri::FromWindowsPath(absolute);
		if (!uri) {
			publication.rejectedPaths.push_back(change.path);
			continue;
		}
		// Upstream's `sanitizeRef('~')` searches the index group for the path, not
		// the group of the row being resolved, so this is one fact per change.
		const bool stagedInIndex = IsStagedInIndex(change, groups);
		// One resource per **group**, not one shared between them. Upstream builds
		// a separate `Resource` for every group a path belongs to, each carrying
		// its own area's status, and that is not cosmetic: the Staged Changes row
		// compares HEAD with the index while the Changes row compares the index
		// with the worktree, so one shared row would describe one of them wrongly.
		ForEachPublishedRow(change, groups, [&](EGitResourceGroup group, EGitFileStatus status) {
			ScmResourceGroupState* target = nullptr;
			switch (group) {
			case EGitResourceGroup::Merge: target = &merge; break;
			case EGitResourceGroup::Index: target = &index; break;
			case EGitResourceGroup::WorkingTree: target = &workingTree; break;
			case EGitResourceGroup::Untracked: target = &untracked; break;
			}
			if (!target) return;
			ScmResourceState resource{ *uri.value };
			resource.command = MakeChangeCommand(
				MakeGitDiffRow(change, status, stagedInIndex), repositoryRoot, text);
			resource.tooltip = std::string(GitFileStatusText(status));
			resource.strikeThrough = IsGitFileStatusStruckThrough(status);
			target->resources.push_back(std::move(resource));
		});

		// One badge per file even when the file occupies two rows, because
		// upstream's decoration map is keyed by URI. A change whose every group
		// published no row gets none, rather than a badge for a row that is not
		// on screen.
		if (const auto decoration = GitDecorationStatus(change, groups)) {
			publication.decorations.push_back({ uri.value->ToString(), GitFileStatusLetter(*decoration) });
		}
		// The rows this change contributes, in the same order they were pushed
		// into the groups above, so each operand names exactly one rendered row.
		operandScratch.clear();
		AppendStageResources(change, groups, operandScratch);
		for (auto& operand : operandScratch) {
			publication.operands.push_back({ uri.value->ToString(), std::move(operand) });
		}
	}

	const auto total = merge.resources.size() + index.resources.size()
		+ workingTree.resources.size() + untracked.resources.size();
	provider.count = static_cast<std::int32_t>(total);
	provider.groups = { std::move(merge), std::move(index), std::move(workingTree), std::move(untracked) };
	return publication;
}

std::vector<GitStageResource> CollectGitStageResources(const GitScmState& state, EUntrackedChangesPolicy policy)
{
	std::vector<GitStageResource> resources;
	if (!state.repository) return resources;
	for (const auto& change : state.changes) {
		const auto groups = ClassifyChange(change, policy);
		if (!groups.Any()) continue;
		AppendStageResources(change, groups, resources);
	}
	return resources;
}

std::vector<GitDiffRowEntry> CollectGitDiffRows(const GitScmState& state, EUntrackedChangesPolicy policy)
{
	std::vector<GitDiffRowEntry> rows;
	if (!state.repository) return rows;
	for (const auto& change : state.changes) {
		const auto groups = ClassifyChange(change, policy);
		if (!groups.Any()) continue;
		const bool stagedInIndex = IsStagedInIndex(change, groups);
		ForEachPublishedRow(change, groups, [&](EGitResourceGroup group, EGitFileStatus status) {
			rows.push_back({ group, MakeGitDiffRow(change, status, stagedInIndex) });
		});
	}
	return rows;
}

GitScmPublisher::GitScmPublisher(SourceControlService* service, ScmOwner owner)
	: m_service(service)
	, m_owner(std::move(owner))
	, m_handle(kGitProviderId)
{
}

GitScmPublisher::~GitScmPublisher()
{
	(void)Retract();
}

EScmOperationStatus GitScmPublisher::Publish(std::wstring_view repositoryRoot, const GitScmState& state,
	EUntrackedChangesPolicy policy, const GitDiffTextResolver& text)
{
	if (m_service == nullptr || !m_owner.IsValid()) return EScmOperationStatus::NotApplicable;
	if (repositoryRoot.empty() || !state.repository) return Retract();
	// A supplied resolver may have changed its language between calls while the
	// repository state stayed identical. Replaying the old provider in that case
	// would leave stale group labels and diff titles visible, so only the
	// resolver-free path uses the cheap state cache.
	if (!text && m_created && policy == m_lastPolicy && m_lastRoot == repositoryRoot && m_lastState == state) {
		return EScmOperationStatus::Replayed;
	}

	// One CreateProvider carries the provider and every group together, and the
	// service applies it as a single revision. Doing this instead of an update
	// followed by four ReplaceResources calls is what keeps a refresh from being
	// observable in a half-applied state, where the branch has already changed
	// but the file list still belongs to the previous one.
	auto publication = BuildGitPublication(m_owner, repositoryRoot, state, policy, text);
	// The build is pure and always yields an empty box; the typed message belongs
	// to the publisher, not to the repository state, so a refresh triggered by an
	// unrelated file change cannot discard it.
	publication.provider.inputBox.value = m_inputBox.value;
	m_inputBox = publication.provider.inputBox;
	ScmCreateProviderRequest create;
	create.provider = std::move(publication.provider);
	const auto result = m_service->CreateProvider(create);
	if (!result.Succeeded()) return result.status;
	m_created = true;
	m_decorations = std::move(publication.decorations);
	m_operands = std::move(publication.operands);

	m_lastRoot = repositoryRoot;
	m_lastState = state;
	m_lastPolicy = policy;
	return EScmOperationStatus::Succeeded;
}

EScmOperationStatus GitScmPublisher::SetInputBoxValue(std::string value)
{
	if (m_inputBox.value == value) return EScmOperationStatus::Replayed;
	m_inputBox.value = std::move(value);
	if (m_service == nullptr || !m_owner.IsValid() || !m_created) return EScmOperationStatus::NotApplicable;

	// Carry the whole last-published box rather than a freshly defaulted one, so
	// the placeholder naming the branch survives an operation that changes only
	// what the user typed.
	ScmInputBoxUpdateRequest update;
	update.owner = m_owner;
	update.handle = m_handle;
	update.inputBox = m_inputBox;
	return m_service->UpdateInputBox(update).status;
}

EScmOperationStatus GitScmPublisher::Retract()
{
	if (m_service == nullptr || !m_created) return EScmOperationStatus::NotApplicable;
	ScmDisposeProviderRequest dispose;
	dispose.owner = m_owner;
	dispose.handle = m_handle;
	const auto result = m_service->DisposeProvider(dispose);
	m_created = false;
	m_lastRoot.clear();
	m_lastState = {};
	// Upstream disposes the whole `Repository`, and its input box goes with it.
	// The typed message belongs to a repository that no longer exists, so keeping
	// it would offer to commit it to whichever repository appears next.
	m_inputBox = {};
	m_decorations.clear();
	m_operands.clear();
	return result.status;
}

} // namespace workbench::scm
