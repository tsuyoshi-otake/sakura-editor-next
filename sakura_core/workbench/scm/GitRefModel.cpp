/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/scm/GitRefModel.h"

#include <algorithm>
#include <iterator>
#include <regex>

namespace workbench::scm {

//! Upstream reads `for-each-ref` output as UTF-8 and matches on UTF-16 text, so
//! decode rather than widen byte-by-byte. This matches `GitScmModel.cpp`'s
//! `FromUtf8`, including its fallback: a ref name git wrote in some other
//! encoding still names a real ref, and dropping the whole listing would be a
//! worse answer than a lossy one.
std::wstring DecodeGitOutput(std::string_view bytes)
{
	if (bytes.empty()) {
		return {};
	}
	const int required = ::MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
	if (required <= 0) {
		return std::wstring(bytes.begin(), bytes.end());
	}
	std::wstring text(static_cast<size_t>(required), L'\0');
	::MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), text.data(), required);
	return text;
}

namespace {

[[nodiscard]] bool IsObjectName(std::wstring_view text) noexcept
{
	// Upstream's `[0-9a-f]{40}` is SHA-1 only. Reproduce the same bound: a repo
	// git reports with wider object names would drop its refs upstream too, and
	// silently accepting them here would make this list disagree with VS Code.
	if (text.size() != 40) {
		return false;
	}
	return std::all_of(text.begin(), text.end(), [](wchar_t c) noexcept {
		return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
	});
}

//! Splits one output line on NUL. `for-each-ref` emits the format verbatim, so
//! the fields are positional and a short line simply yields fewer of them.
std::vector<std::wstring_view> SplitNul(std::wstring_view line)
{
	std::vector<std::wstring_view> fields;
	size_t begin = 0;
	while (true) {
		const size_t nul = line.find(L'\0', begin);
		if (nul == std::wstring_view::npos) {
			fields.push_back(line.substr(begin));
			break;
		}
		fields.push_back(line.substr(begin, nul - begin));
		begin = nul + 1;
	}
	return fields;
}

[[nodiscard]] std::wstring ShortCommit(std::wstring_view commit)
{
	return std::wstring(commit.substr(0, std::min<size_t>(commit.size(), kGitCommitShortHashLength)));
}

//! `RefItemSeparator`'s label for each kind, verbatim from upstream.
[[nodiscard]] std::wstring SeparatorLabel(EGitRefKind kind)
{
	switch (kind) {
	case EGitRefKind::Head: return L"branches";
	case EGitRefKind::RemoteHead: return L"remote branches";
	case EGitRefKind::Tag: return L"tags";
	}
	return {};
}

//! `RefItem`'s label and description for each kind, verbatim from upstream.
[[nodiscard]] GitCheckoutItem MakeRefItem(const GitRef& ref)
{
	GitCheckoutItem item;
	item.kind = EGitCheckoutItemKind::Ref;
	item.refKind = ref.kind;
	item.refName = ref.name;
	item.remote = ref.remote;

	const std::wstring shortCommit = ShortCommit(ref.commit);
	switch (ref.kind) {
	case EGitRefKind::Head:
		item.label = L"$(git-branch) " + ref.name;
		item.description = shortCommit;
		break;
	case EGitRefKind::RemoteHead:
		item.label = L"$(cloud) " + ref.name;
		item.description = L"Remote branch at " + shortCommit;
		break;
	case EGitRefKind::Tag:
		item.label = L"$(tag) " + ref.name;
		item.description = L"Tag at " + shortCommit;
		break;
	}
	return item;
}

//! Emits one upstream `RefProcessor` group: nothing at all when empty, and
//! otherwise its separator followed by its items.
void AppendRefGroup(
	std::vector<GitCheckoutItem>& items, const std::vector<GitRef>& refs, EGitRefKind kind)
{
	const bool any = std::any_of(refs.begin(), refs.end(), [kind](const GitRef& ref) noexcept {
		return ref.kind == kind && !(ref.name.empty() && ref.commit.empty());
	});
	if (!any) {
		return;
	}

	GitCheckoutItem separator;
	separator.kind = EGitCheckoutItemKind::Separator;
	separator.refKind = kind;
	separator.label = SeparatorLabel(kind);
	items.push_back(std::move(separator));

	for (const auto& ref : refs) {
		if (ref.kind != kind || (ref.name.empty() && ref.commit.empty())) {
			continue;
		}
		items.push_back(MakeRefItem(ref));
	}
}

[[nodiscard]] std::vector<GitCheckoutItem> MakeCheckoutCommandItems()
{
	std::vector<GitCheckoutItem> commands;
	commands.push_back({ EGitCheckoutItemKind::CreateBranch, L"$(plus) Create new branch...", {}, {}, EGitRefKind::Head, {} });
	commands.push_back({ EGitCheckoutItemKind::CreateBranchFrom, L"$(plus) Create new branch from...", {}, {}, EGitRefKind::Head, {} });
	commands.push_back({ EGitCheckoutItemKind::CheckoutDetached, L"$(debug-disconnect) Checkout detached...", {}, {}, EGitRefKind::Head, {} });
	return commands;
}

} // namespace

std::vector<std::wstring> BuildForEachRefArguments(int maximumCount)
{
	std::vector<std::wstring> arguments;
	arguments.emplace_back(L"for-each-ref");
	if (maximumCount > 0) {
		arguments.emplace_back(L"--count=" + std::to_wstring(maximumCount));
	}
	arguments.emplace_back(L"--format");
	arguments.emplace_back(kGitRefsFormat);
	return arguments;
}

std::vector<GitRef> ParseForEachRef(std::string_view bytes)
{
	const std::wstring text = DecodeGitOutput(bytes);
	std::vector<GitRef> refs;

	size_t begin = 0;
	while (begin <= text.size()) {
		size_t end = text.find(L'\n', begin);
		if (end == std::wstring::npos) {
			end = text.size();
		}
		std::wstring_view line(text.data() + begin, end - begin);
		begin = end + 1;

		if (!line.empty() && line.back() == L'\r') {
			line.remove_suffix(1);
		}
		if (line.empty()) {
			continue;
		}

		const auto fields = SplitNul(line);
		if (fields.size() < 2) {
			continue;
		}
		const std::wstring_view refName = fields[0];
		const std::wstring_view commit = fields[1];
		// `%(*objectname)` is empty for anything but an annotated tag.
		const std::wstring_view tagCommit = fields.size() > 2 ? fields[2] : std::wstring_view{};

		if (!refName.starts_with(L"refs/") || !IsObjectName(commit)) {
			continue;
		}

		GitRef ref;
		if (refName.starts_with(L"refs/heads/")) {
			const std::wstring_view name = refName.substr(11);
			// Upstream's `([^ ]+)`: a name with a space is not a ref it offers.
			if (name.empty() || name.find(L' ') != std::wstring_view::npos) {
				continue;
			}
			ref.kind = EGitRefKind::Head;
			ref.name = name;
			ref.commit = commit;
		}
		else if (refName.starts_with(L"refs/remotes/")) {
			const std::wstring_view rest = refName.substr(13);
			const size_t slash = rest.find(L'/');
			if (slash == std::wstring_view::npos || slash == 0) {
				continue;
			}
			const std::wstring_view branch = rest.substr(slash + 1);
			if (branch.empty() || branch.find(L' ') != std::wstring_view::npos) {
				continue;
			}
			ref.kind = EGitRefKind::RemoteHead;
			ref.remote = rest.substr(0, slash);
			ref.name = std::wstring(ref.remote) + L"/" + std::wstring(branch);
			ref.commit = commit;
		}
		else if (refName.starts_with(L"refs/tags/")) {
			const std::wstring_view name = refName.substr(10);
			if (name.empty() || name.find(L' ') != std::wstring_view::npos) {
				continue;
			}
			ref.kind = EGitRefKind::Tag;
			ref.name = name;
			// An annotated tag reports the commit it points at, not the tag object.
			ref.commit = IsObjectName(tagCommit) ? std::wstring(tagCommit) : std::wstring(commit);
		}
		else {
			continue;
		}

		refs.push_back(std::move(ref));
	}

	return refs;
}

std::vector<GitCheckoutItem> BuildCheckoutItems(
	const std::vector<GitRef>& refs, bool detached, bool filterIsEmpty)
{
	// `origin/HEAD` is a symbolic alias, so upstream hides it from the ordinary
	// branch list and only keeps it when an explicit detached checkout is asked
	// for.
	std::vector<GitRef> selected;
	selected.reserve(refs.size());
	for (const auto& ref : refs) {
		if (!detached && ref.name == L"origin/HEAD") {
			continue;
		}
		if (detached && ref.kind == EGitRefKind::Tag) {
			continue;
		}
		selected.push_back(ref);
	}

	std::vector<GitCheckoutItem> picks;
	AppendRefGroup(picks, selected, EGitRefKind::Head);
	AppendRefGroup(picks, selected, EGitRefKind::RemoteHead);
	if (!detached) {
		AppendRefGroup(picks, selected, EGitRefKind::Tag);
	}

	// Detached mode offers no command rows at all.
	if (detached) {
		return picks;
	}

	auto commands = MakeCheckoutCommandItems();
	if (picks.empty()) {
		return commands;
	}
	if (filterIsEmpty) {
		// Nothing typed yet: the actions the user cannot reach by typing a name
		// come first.
		commands.insert(commands.end(), picks.begin(), picks.end());
		return commands;
	}

	// Once a filter narrows the refs, the matching refs lead and the commands
	// move below a blank separator.
	std::vector<GitCheckoutItem> items = std::move(picks);
	GitCheckoutItem blank;
	blank.kind = EGitCheckoutItemKind::Separator;
	items.push_back(std::move(blank));
	items.insert(items.end(), commands.begin(), commands.end());
	return items;
}

std::wstring CheckoutPlaceholder(bool detached)
{
	return detached
		? L"Select a branch to checkout in detached mode"
		: L"Select a branch or tag to checkout";
}

std::vector<GitCheckoutItem> BuildBranchFromItems(
	const std::vector<GitRef>& refs, std::wstring_view headCommit)
{
	std::vector<GitCheckoutItem> items;

	// Upstream's `HEADItem`: the branch is created from HEAD unless another ref
	// is chosen, so HEAD is the first and default row.
	GitCheckoutItem head;
	head.kind = EGitCheckoutItemKind::Ref;
	head.label = L"HEAD";
	head.description = ShortCommit(headCommit);
	head.refName = L"HEAD";
	items.push_back(std::move(head));

	// `RefItemsProcessor` always skips `origin/HEAD`; only the checkout picker
	// makes an exception for it, and only in detached mode.
	std::vector<GitRef> selected;
	selected.reserve(refs.size());
	std::copy_if(refs.begin(), refs.end(), std::back_inserter(selected), [](const GitRef& ref) {
		return ref.name != L"origin/HEAD";
	});

	AppendRefGroup(items, selected, EGitRefKind::Head);
	AppendRefGroup(items, selected, EGitRefKind::RemoteHead);
	AppendRefGroup(items, selected, EGitRefKind::Tag);
	return items;
}

std::wstring SanitizeBranchName(std::wstring_view name, wchar_t whitespaceChar)
{
	if (name.empty()) {
		return {};
	}

	// Upstream applies two JavaScript replacements to the trimmed name. The
	// second pattern is reproduced verbatim rather than reimplemented by hand:
	// its alternation order decides which character a match consumes, and any
	// rewrite would quietly produce different names for the same input.
	static const std::wregex leadingDashes(L"^-+");
	static const std::wregex invalid(
		L"^\\.|/\\.|\\.\\.|~|\\^|:|/$|\\.lock$|\\.lock/|\\\\|\\*|\\s|^\\s*$|\\.$|\\[|\\]$");

	std::wstring trimmed(name);
	const size_t first = trimmed.find_first_not_of(L" \t\n\v\f\r");
	if (first == std::wstring::npos) {
		trimmed.clear();
	}
	else {
		const size_t last = trimmed.find_last_not_of(L" \t\n\v\f\r");
		trimmed = trimmed.substr(first, last - first + 1);
	}

	std::wstring result = std::regex_replace(trimmed, leadingDashes, std::wstring());
	return std::regex_replace(result, invalid, std::wstring(1, whitespaceChar));
}

GitBranchNameValidationResult ValidateBranchName(
	std::wstring_view name, const std::vector<GitRef>& refs, wchar_t whitespaceChar)
{
	GitBranchNameValidationResult result;
	result.sanitizedName = SanitizeBranchName(name, whitespaceChar);

	if (result.sanitizedName.empty()) {
		result.state = EGitBranchNameValidation::Empty;
		return result;
	}

	const auto existing = std::find_if(refs.begin(), refs.end(), [&](const GitRef& ref) {
		return ref.name == result.sanitizedName;
	});
	if (existing != refs.end()) {
		result.state = EGitBranchNameValidation::AlreadyExists;
		result.message = L"Branch \"" + result.sanitizedName + L"\" already exists";
		return result;
	}

	if (result.sanitizedName != name) {
		result.state = EGitBranchNameValidation::Sanitized;
		result.message = L"The new branch will be \"" + result.sanitizedName + L"\"";
		return result;
	}

	result.state = EGitBranchNameValidation::Valid;
	return result;
}

std::vector<std::wstring> BuildCheckoutArguments(std::wstring_view refName, bool detached)
{
	std::vector<std::wstring> arguments;
	arguments.emplace_back(L"checkout");
	arguments.emplace_back(L"-q");
	if (detached) {
		arguments.emplace_back(L"--detach");
	}
	if (!refName.empty()) {
		arguments.emplace_back(refName);
	}
	return arguments;
}

std::vector<std::wstring> BuildCheckoutTrackingArguments(std::wstring_view remoteRefName)
{
	std::vector<std::wstring> arguments;
	arguments.emplace_back(L"checkout");
	arguments.emplace_back(L"-q");
	arguments.emplace_back(L"--track");
	if (!remoteRefName.empty()) {
		arguments.emplace_back(remoteRefName);
	}
	return arguments;
}

std::vector<std::wstring> BuildCreateBranchArguments(
	std::wstring_view branchName, std::wstring_view target)
{
	std::vector<std::wstring> arguments;
	arguments.emplace_back(L"checkout");
	arguments.emplace_back(L"-q");
	arguments.emplace_back(L"-b");
	arguments.emplace_back(branchName);
	arguments.emplace_back(L"--no-track");
	if (!target.empty()) {
		arguments.emplace_back(target);
	}
	return arguments;
}

} // namespace workbench::scm
