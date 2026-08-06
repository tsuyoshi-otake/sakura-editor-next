/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/scm/GitStageCommands.h"

#include "workbench/commands/CommandArgumentsJson.h"
#include "workbench/scm/GitFailureText.h"

#include <algorithm>
#include <utility>

namespace workbench::scm {

namespace {

//! A quoted `git.exe` path becomes argv[0] ahead of everything this file builds.
constexpr std::size_t kProgramNameAllowance = 512;

[[nodiscard]] GitStageCommandResult Succeeded()
{
	return { EGitStageCommandStatus::Succeeded, {} };
}

[[nodiscard]] GitStageCommandResult NotApplicable()
{
	return { EGitStageCommandStatus::NotApplicable, {} };
}

[[nodiscard]] GitStageCommandResult Cancelled()
{
	return { EGitStageCommandStatus::Cancelled, {} };
}

[[nodiscard]] GitStageCommandResult Failed(std::wstring message)
{
	return { EGitStageCommandStatus::Failed, std::move(message) };
}

void Notify(const GitStageCommandContext& context, std::wstring_view message)
{
	if (context.message) {
		context.message(message);
	}
}

//! `path.basename`. Upstream's confirmations name the file, not its path, so a
//! deeply nested path cannot push the verb off the end of the dialog.
[[nodiscard]] std::wstring BaseName(std::wstring_view path)
{
	const auto separator = path.find_last_of(L"/\\");
	return std::wstring(separator == std::wstring_view::npos ? path : path.substr(separator + 1));
}

[[nodiscard]] std::wstring Count(std::size_t value)
{
	return std::to_wstring(value);
}

[[nodiscard]] std::vector<std::wstring> PathsOf(const std::vector<GitStageResource>& resources)
{
	std::vector<std::wstring> paths;
	paths.reserve(resources.size());
	for (const auto& resource : resources) {
		paths.push_back(resource.path);
	}
	return paths;
}

//! The quoted width one argument adds, including its separating space.
[[nodiscard]] std::size_t QuotedWidth(std::wstring_view value)
{
	return QuoteGitArgument(value).size() + 1;
}

//! Upstream's `getDiscardUntrackedChangesDialogDetails`, returned as its triple.
struct UntrackedDialogDetails final {
	std::wstring message;
	std::wstring detail;
	std::wstring primaryAction;
};

[[nodiscard]] UntrackedDialogDetails BuildUntrackedDialogDetails(
	const std::vector<GitStageResource>& resources, bool toTrash)
{
	const auto count = resources.size();
	// Only the permanent path carries the warning; the Recycle Bin path is
	// recoverable, and saying otherwise would train the user to ignore it.
	std::wstring warning;
	if (!toTrash) {
		warning = count == 1
			? std::wstring(L"\n\nThis is IRREVERSIBLE!\nThis file will be FOREVER LOST if you proceed.")
			: std::wstring(L"\n\nThis is IRREVERSIBLE!\nThese files will be FOREVER LOST if you proceed.");
	}

	UntrackedDialogDetails details;
	details.message = count == 1
		? L"Are you sure you want to DELETE the following untracked file: '"
			+ BaseName(resources.front().path) + L"'?" + warning
		: L"Are you sure you want to DELETE the " + Count(count) + L" untracked files?" + warning;
	if (toTrash) {
		// This build is Windows-only, so upstream's `isWindows` branch is the
		// only reachable one and the Trash wording never applies.
		details.detail = count == 1
			? std::wstring(L"You can restore this file from the Recycle Bin.")
			: std::wstring(L"You can restore these files from the Recycle Bin.");
		details.primaryAction = L"Move to Recycle Bin";
	}
	else {
		details.primaryAction = count == 1
			? std::wstring(L"Delete File")
			: L"Delete All " + Count(count) + L" Files";
	}
	return details;
}

//! Whether HEAD names a commit, decided the way `Git.revert` decides it.
[[nodiscard]] bool RepositoryHasCommits(const GitStageCommandContext& context)
{
	const auto result = context.run({ L"branch" });
	if (!result.Succeeded() || result.exitCode != 0) {
		// The probe itself failed, so nothing is known about HEAD. The reset
		// form is the one that applies to every repository that has ever
		// committed; guessing the unborn form instead would unstage by removing
		// paths from the index of a repository that has a HEAD to reset to.
		return true;
	}
	return std::any_of(result.standardOutput.begin(), result.standardOutput.end(), [](std::uint8_t byte) {
		return byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n';
	});
}

//!
//! @brief Run every chunk, stopping at the first failure.
//!
//! A later chunk failing leaves the earlier ones applied. Upstream has exactly
//! the same property — it awaits its chunks in sequence — and the alternative,
//! rolling back a partially applied index, is not something git offers. The
//! reported failure is therefore "some of this did not happen", which is why the
//! caller surfaces git's own reason rather than a generic sentence.
//!
[[nodiscard]] GitStageCommandResult RunChunks(const GitStageCommandContext& context,
	const std::vector<std::wstring>& prefix, const std::vector<std::wstring>& paths)
{
	const auto limits = GitPathChunkLimits::ForRepository(context.repositoryRoot);
	for (const auto& arguments : BuildGitPathChunks(prefix, paths, limits)) {
		const auto result = context.run(arguments);
		if (!result.Succeeded() || result.exitCode != 0) {
			auto message = DescribeGitFailure(result);
			Notify(context, message);
			return Failed(std::move(message));
		}
	}
	return Succeeded();
}

//! Reproduces `Repository._clean`: the Recycle Bin first, and a permanent delete
//! only after the user confirms it for the exact paths the bin refused.
[[nodiscard]] GitStageCommandResult DiscardUntracked(
	const GitStageCommandContext& context, const std::vector<GitStageResource>& resources, bool toTrash)
{
	if (!toTrash) {
		return RunChunks(context, BuildCleanPrefix(), PathsOf(resources));
	}

	std::vector<std::wstring> absolute;
	absolute.reserve(resources.size());
	for (const auto& resource : resources) {
		absolute.push_back(JoinRepositoryPath(context.repositoryRoot, resource.path));
	}
	const auto refused = context.trash(absolute);
	if (refused.empty()) {
		return Succeeded();
	}

	std::vector<GitStageResource> remaining;
	for (std::size_t index = 0; index < resources.size(); ++index) {
		if (std::find(refused.begin(), refused.end(), absolute[index]) != refused.end()) {
			remaining.push_back(resources[index]);
		}
	}
	if (remaining.empty()) {
		// The deleter named paths it was never given. Treating that as "nothing
		// left to do" is the safe reading: it cannot justify deleting anything.
		return Succeeded();
	}

	const auto prompt = BuildTrashFallbackPrompt(remaining);
	const auto chosen = context.confirm(prompt);
	if (!chosen.has_value() || *chosen >= prompt.choices.size()) {
		return Cancelled();
	}
	return RunChunks(context, BuildCleanPrefix(), PathsOf(remaining));
}

} // namespace

GitPathChunkLimits GitPathChunkLimits::ForRepository(std::wstring_view repositoryRoot)
{
	GitPathChunkLimits limits;
	// `BuildEffectiveGitArguments` prepends `-C <root>` after a chunk is built,
	// and the runner checks its limits against that composed vector.
	limits.maximumArguments = kMaximumGitArguments > 2 ? kMaximumGitArguments - 2 : 1;
	const std::size_t reserved =
		QuotedWidth(L"-C") + QuotedWidth(repositoryRoot) + kProgramNameAllowance;
	limits.maximumCommandLineLength =
		kMaximumGitCommandLineLength > reserved ? kMaximumGitCommandLineLength - reserved : 1;
	return limits;
}

std::vector<std::vector<std::wstring>> BuildGitPathChunks(const std::vector<std::wstring>& prefix,
	const std::vector<std::wstring>& paths, const GitPathChunkLimits& limits)
{
	std::vector<std::vector<std::wstring>> chunks;
	std::size_t prefixWidth = 0;
	for (const auto& argument : prefix) {
		prefixWidth += QuotedWidth(argument);
	}
	if (paths.empty()) {
		chunks.push_back(prefix);
		return chunks;
	}

	std::vector<std::wstring> current = prefix;
	std::size_t width = prefixWidth;
	for (const auto& path : paths) {
		const auto pathWidth = QuotedWidth(path);
		const bool holdsPath = current.size() > prefix.size();
		const bool overflows = current.size() + 1 > limits.maximumArguments
			|| width + pathWidth > limits.maximumCommandLineLength;
		if (holdsPath && overflows) {
			chunks.push_back(std::move(current));
			current = prefix;
			width = prefixWidth;
		}
		current.push_back(path);
		width += pathWidth;
	}
	chunks.push_back(std::move(current));
	return chunks;
}

std::vector<std::wstring> BuildStagePrefix(bool updateOnly)
{
	return { L"add", updateOnly ? L"-u" : L"-A", L"--" };
}

std::vector<std::wstring> BuildUnstagePrefix(bool hasCommits)
{
	if (hasCommits) {
		return { L"reset", L"-q", L"HEAD", L"--" };
	}
	return { L"rm", L"--cached", L"-r", L"--" };
}

std::vector<std::wstring> BuildDiscardCheckoutPrefix()
{
	return { L"checkout", L"-q", L"--" };
}

std::vector<std::wstring> BuildCleanPrefix()
{
	return { L"clean", L"-f", L"-q", L"--" };
}

std::vector<GitStageResource> SelectStageableResources(const std::vector<GitStageResource>& resources)
{
	std::vector<GitStageResource> selected;
	for (const auto& resource : resources) {
		if (resource.group == EGitResourceGroup::WorkingTree || resource.group == EGitResourceGroup::Untracked) {
			selected.push_back(resource);
		}
	}
	return selected;
}

std::vector<GitStageResource> SelectUnstageableResources(const std::vector<GitStageResource>& resources)
{
	std::vector<GitStageResource> selected;
	for (const auto& resource : resources) {
		if (resource.group == EGitResourceGroup::Index) {
			selected.push_back(resource);
		}
	}
	return selected;
}

std::vector<GitStageResource> SelectDiscardableResources(const std::vector<GitStageResource>& resources)
{
	return SelectStageableResources(resources);
}

bool HasMergeResource(const std::vector<GitStageResource>& resources) noexcept
{
	return std::any_of(resources.begin(), resources.end(),
		[](const GitStageResource& resource) { return resource.group == EGitResourceGroup::Merge; });
}

GitDiscardPrompt BuildDiscardPrompt(const std::vector<GitStageResource>& resources, bool untrackedToTrash)
{
	std::vector<GitStageResource> tracked;
	std::vector<GitStageResource> untracked;
	for (const auto& resource : resources) {
		(resource.untracked ? untracked : tracked).push_back(resource);
	}

	GitDiscardPrompt prompt;
	const auto count = resources.size();

	if (untracked.empty()) {
		// `_cleanTrackedChanges`. Restoring a deleted file and discarding an edit
		// are opposite outcomes, so upstream words them differently and so does
		// this: a dialog that says "discard" while it is about to bring a file
		// back is worse than no dialog.
		const bool allDeleted = !resources.empty()
			&& std::all_of(resources.begin(), resources.end(),
				[](const GitStageResource& resource) { return resource.deleted; });
		if (allDeleted) {
			prompt.message = count == 1
				? L"Are you sure you want to restore '" + BaseName(resources.front().path) + L"'?"
				: L"Are you sure you want to restore ALL " + Count(count) + L" files?";
			prompt.choices.push_back({ count == 1
					? std::wstring(L"Restore File")
					: L"Restore All " + Count(count) + L" Files",
				resources });
		}
		else {
			prompt.message = count == 1
				? L"Are you sure you want to discard changes in '" + BaseName(resources.front().path) + L"'?"
				: L"Are you sure you want to discard ALL changes in " + Count(count)
					+ L" files?\n\nThis is IRREVERSIBLE!\nYour current working set will be FOREVER LOST if you proceed.";
			prompt.choices.push_back({ count == 1
					? std::wstring(L"Discard File")
					: L"Discard All " + Count(count) + L" Files",
				resources });
		}
		return prompt;
	}

	const auto details = BuildUntrackedDialogDetails(untracked, untrackedToTrash);
	if (tracked.empty()) {
		// `_cleanUntrackedChanges`, which is the one case that passes a detail.
		prompt.message = details.message;
		prompt.detail = details.detail;
		prompt.choices.push_back({ details.primaryAction, resources });
		return prompt;
	}

	// `_cleanAll`'s mixed branch. Its two buttons discard different sets, and
	// that difference is the whole point: the tracked files are recoverable from
	// the index while the untracked ones are not, so collapsing them into one
	// "yes" would hide the only irreversible half of the operation.
	const auto trackedMessage = tracked.size() == 1
		? L"\n\nAre you sure you want to discard changes in '" + BaseName(tracked.front().path) + L"'?"
		: L"\n\nAre you sure you want to discard ALL changes in " + Count(tracked.size()) + L" files?";
	prompt.message = details.message + L" " + details.detail + trackedMessage
		+ L"\n\nThis is IRREVERSIBLE!\nYour current working set will be FOREVER LOST if you proceed.";
	prompt.choices.push_back({ tracked.size() == 1
			? std::wstring(L"Discard 1 Tracked File")
			: L"Discard All " + Count(tracked.size()) + L" Tracked Files",
		tracked });
	prompt.choices.push_back({ L"Discard All " + Count(count) + L" Files", resources });
	return prompt;
}

GitDiscardPrompt BuildTrashFallbackPrompt(const std::vector<GitStageResource>& resources)
{
	GitDiscardPrompt prompt;
	prompt.message = L"Failed to delete using the Recycle Bin. Do you want to permanently delete instead?";
	prompt.choices.push_back({ resources.size() == 1
			? std::wstring(L"Delete File")
			: L"Delete All " + Count(resources.size()) + L" Files",
		resources });
	return prompt;
}

std::wstring JoinRepositoryPath(std::wstring_view repositoryRoot, std::wstring_view relativePath)
{
	std::wstring joined(repositoryRoot);
	while (!joined.empty() && (joined.back() == L'\\' || joined.back() == L'/')) {
		joined.pop_back();
	}
	std::wstring_view tail = relativePath;
	while (!tail.empty() && (tail.front() == L'\\' || tail.front() == L'/')) {
		tail.remove_prefix(1);
	}
	if (joined.empty()) {
		joined.assign(tail);
	}
	else if (!tail.empty()) {
		joined.push_back(L'\\');
		joined.append(tail);
	}
	// Porcelain reports `/` separators. A git pathspec accepts them, but a shell
	// delete wants the native form, and this path only exists for that delete.
	std::replace(joined.begin(), joined.end(), L'/', L'\\');
	return joined;
}

GitStageCommandResult RunGitStage(
	const GitStageCommandContext& context, const std::vector<GitStageResource>& resources, bool updateOnly)
{
	if (!context.run) {
		return Failed(L"The stage command has no git invoker.");
	}
	if (HasMergeResource(resources)) {
		// Upstream categorizes a merge row into resolved, unresolved, and
		// deletion-conflict sets, scanning the file for conflict markers and
		// opening a picker for the deletion case. None of that exists yet, so
		// this fails closed rather than staging a file that may still hold
		// `<<<<<<<` markers.
		std::wstring message =
			L"Staging a file with merge conflicts is not available yet. Resolve the conflict first.";
		Notify(context, message);
		return { EGitStageCommandStatus::UnsupportedMergeConflict, std::move(message) };
	}
	const auto selected = SelectStageableResources(resources);
	if (selected.empty()) {
		return NotApplicable();
	}
	return RunChunks(context, BuildStagePrefix(updateOnly), PathsOf(selected));
}

GitStageCommandResult RunGitUnstage(
	const GitStageCommandContext& context, const std::vector<GitStageResource>& resources)
{
	if (!context.run) {
		return Failed(L"The unstage command has no git invoker.");
	}
	const auto selected = SelectUnstageableResources(resources);
	if (selected.empty()) {
		return NotApplicable();
	}
	return RunChunks(context, BuildUnstagePrefix(RepositoryHasCommits(context)), PathsOf(selected));
}

GitStageCommandResult RunGitUnstageAll(const GitStageCommandContext& context)
{
	if (!context.run) {
		return Failed(L"The unstage command has no git invoker.");
	}
	// Upstream's `revert([])`: one whole-index reset, not a listing of every
	// staged path. The distinction matters for an index holding more paths than
	// one command line can carry.
	return RunChunks(context, BuildUnstagePrefix(RepositoryHasCommits(context)), { L"." });
}

GitStageCommandResult RunGitDiscard(
	const GitStageCommandContext& context, const std::vector<GitStageResource>& resources)
{
	if (!context.run) {
		return Failed(L"The discard command has no git invoker.");
	}
	if (!context.confirm) {
		// Never silently discard. A missing confirmation presenter is a
		// composition defect, and the safe reading of it is "do nothing".
		return Failed(L"The discard command has no confirmation presenter.");
	}
	const auto selected = SelectDiscardableResources(resources);
	if (selected.empty()) {
		return NotApplicable();
	}

	const bool toTrash = static_cast<bool>(context.trash);
	const auto prompt = BuildDiscardPrompt(selected, toTrash);
	const auto chosen = context.confirm(prompt);
	if (!chosen.has_value() || *chosen >= prompt.choices.size()) {
		return Cancelled();
	}
	const auto confirmed = prompt.choices[*chosen].resources;
	if (confirmed.empty()) {
		return NotApplicable();
	}

	std::vector<GitStageResource> toClean;
	std::vector<GitStageResource> toCheckout;
	for (const auto& resource : confirmed) {
		(resource.untracked ? toClean : toCheckout).push_back(resource);
	}

	// Upstream's order: the untracked deletion first, then the tracked restore.
	// A cancelled Recycle Bin fallback does not abort the restore — upstream's
	// `_clean` simply returns — but the overall result stays `Cancelled`, so the
	// caller never reports that everything the user asked for happened.
	bool cancelled = false;
	if (!toClean.empty()) {
		const auto result = DiscardUntracked(context, toClean, toTrash);
		switch (result.status) {
		case EGitStageCommandStatus::Succeeded:
			break;
		case EGitStageCommandStatus::Cancelled:
			cancelled = true;
			break;
		default:
			return result;
		}
	}
	if (!toCheckout.empty()) {
		auto result = RunChunks(context, BuildDiscardCheckoutPrefix(), PathsOf(toCheckout));
		if (!result.Succeeded()) {
			return result;
		}
	}
	return cancelled ? Cancelled() : Succeeded();
}

namespace {

using commands::json::AppendEscaped;
using commands::json::Expect;
using commands::json::ReadBoolean;
using commands::json::ReadString;
using commands::json::SkipWhitespace;
using commands::json::ToUtf8;
using commands::json::ToWideStrict;

[[nodiscard]] std::string_view GroupToken(EGitResourceGroup group) noexcept
{
	switch (group) {
	case EGitResourceGroup::Merge:
		return kGitStageGroupTokenMerge;
	case EGitResourceGroup::Index:
		return kGitStageGroupTokenIndex;
	case EGitResourceGroup::Untracked:
		return kGitStageGroupTokenUntracked;
	case EGitResourceGroup::WorkingTree:
	default:
		return kGitStageGroupTokenWorkingTree;
	}
}

[[nodiscard]] std::optional<EGitResourceGroup> GroupFromToken(std::string_view token) noexcept
{
	if (token == kGitStageGroupTokenMerge) return EGitResourceGroup::Merge;
	if (token == kGitStageGroupTokenIndex) return EGitResourceGroup::Index;
	if (token == kGitStageGroupTokenWorkingTree) return EGitResourceGroup::WorkingTree;
	if (token == kGitStageGroupTokenUntracked) return EGitResourceGroup::Untracked;
	return std::nullopt;
}


} // namespace

std::string BuildGitStageArguments(const std::vector<GitStageResource>& resources)
{
	std::string json;
	json += '[';
	bool first = true;
	for (const auto& resource : resources) {
		if (!first) {
			json += ',';
		}
		first = false;
		json += "{\"group\":\"";
		json += GroupToken(resource.group);
		json += "\",\"path\":\"";
		AppendEscaped(json, ToUtf8(resource.path));
		json += "\",\"untracked\":";
		json += resource.untracked ? "true" : "false";
		json += ",\"deleted\":";
		json += resource.deleted ? "true" : "false";
		json += '}';
	}
	json += ']';
	return json;
}

std::optional<std::vector<GitStageResource>> ParseGitStageArguments(std::string_view argumentsJson)
{
	std::size_t index = 0;
	SkipWhitespace(argumentsJson, index);
	if (index >= argumentsJson.size()) {
		// The argument-less invocation, not a malformed payload.
		return std::vector<GitStageResource>{};
	}
	if (!Expect(argumentsJson, index, '[')) {
		return std::nullopt;
	}

	std::vector<GitStageResource> resources;
	SkipWhitespace(argumentsJson, index);
	if (index < argumentsJson.size() && argumentsJson[index] == ']') {
		++index;
		SkipWhitespace(argumentsJson, index);
		return index == argumentsJson.size() ? std::optional{ std::move(resources) } : std::nullopt;
	}

	while (true) {
		if (resources.size() >= kMaximumGitStageArgumentResources) {
			return std::nullopt;
		}
		if (!Expect(argumentsJson, index, '{')) {
			return std::nullopt;
		}
		GitStageResource resource;
		bool sawPath = false;
		bool sawGroup = false;
		SkipWhitespace(argumentsJson, index);
		if (index < argumentsJson.size() && argumentsJson[index] == '}') {
			// Neither required member is present.
			return std::nullopt;
		}
		while (true) {
			SkipWhitespace(argumentsJson, index);
			std::string key;
			if (!ReadString(argumentsJson, index, key)) {
				return std::nullopt;
			}
			if (!Expect(argumentsJson, index, ':')) {
				return std::nullopt;
			}
			SkipWhitespace(argumentsJson, index);
			if (key == "path") {
				std::string raw;
				if (!ReadString(argumentsJson, index, raw)) {
					return std::nullopt;
				}
				if (raw.empty() || raw.size() > kMaximumGitArgumentLength) {
					return std::nullopt;
				}
				auto wide = ToWideStrict(raw);
				if (!wide.has_value() || wide->empty()) {
					return std::nullopt;
				}
				resource.path = *std::move(wide);
				sawPath = true;
			} else if (key == "group") {
				std::string raw;
				if (!ReadString(argumentsJson, index, raw)) {
					return std::nullopt;
				}
				const auto group = GroupFromToken(raw);
				if (!group.has_value()) {
					return std::nullopt;
				}
				resource.group = *group;
				sawGroup = true;
			} else if (key == "untracked") {
				if (!ReadBoolean(argumentsJson, index, resource.untracked)) {
					return std::nullopt;
				}
			} else if (key == "deleted") {
				if (!ReadBoolean(argumentsJson, index, resource.deleted)) {
					return std::nullopt;
				}
			} else {
				// An unknown key means the producer and this parser disagree about
				// what the operand is. Skipping it would act on a selection nobody
				// described.
				return std::nullopt;
			}
			SkipWhitespace(argumentsJson, index);
			if (index < argumentsJson.size() && argumentsJson[index] == ',') {
				++index;
				continue;
			}
			break;
		}
		if (!Expect(argumentsJson, index, '}')) {
			return std::nullopt;
		}
		if (!sawPath || !sawGroup) {
			return std::nullopt;
		}
		resources.push_back(std::move(resource));

		SkipWhitespace(argumentsJson, index);
		if (index < argumentsJson.size() && argumentsJson[index] == ',') {
			++index;
			continue;
		}
		break;
	}

	if (!Expect(argumentsJson, index, ']')) {
		return std::nullopt;
	}
	SkipWhitespace(argumentsJson, index);
	if (index != argumentsJson.size()) {
		return std::nullopt;
	}
	return resources;
}

} // namespace workbench::scm
