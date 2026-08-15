/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <string>
#include <string_view>
#include <functional>
#include <vector>

namespace workbench::scm {

//! Resolves user-visible ref picker text.  The key is a stable workbench text
//! identifier, while the argument carries branch-specific data such as a
//! commit hash.  Keeping this callback optional preserves the model's English
//! fallback for headless callers and tests.
using GitRefTextResolver = std::function<std::wstring(std::string_view key, std::wstring_view argument)>;

//! Upstream `RefType`. These are the three `refs/` namespaces `git.checkout`
//! offers, and the declaration order is the order VS Code emits their groups.
enum class EGitRefKind {
	Head,
	RemoteHead,
	Tag,
};

struct GitRef {
	EGitRefKind kind = EGitRefKind::Head;
	//! `main` for a head, `origin/main` for a remote head, `v1.0.0` for a tag.
	std::wstring name;
	std::wstring commit;
	//! Only a remote head carries one. Upstream captures it separately from the
	//! name because a checkout needs the remote to find the tracking branch.
	std::wstring remote;

	[[nodiscard]] bool operator==(const GitRef&) const = default;
};

//! Upstream's `REFS_FORMAT`. NUL separators are what let a ref name containing
//! spaces or `%` survive the round trip.
inline constexpr std::wstring_view kGitRefsFormat = L"%(refname)%00%(objectname)%00%(*objectname)";

//! Decodes git's UTF-8 output. Shared so that two listings of the same refs are
//! never decoded two different ways: a tracking-branch lookup compares its
//! `%(upstream:short)` against a remote ref name that came from a separate
//! invocation, and a decoder that disagreed by one character would silently turn
//! "switch to the local branch" into "create it again".
[[nodiscard]] std::wstring DecodeGitOutput(std::string_view bytes);

//! Builds `for-each-ref --format <kGitRefsFormat>`, optionally bounded by
//! `--count`. A `maximumCount` of zero omits the flag, as upstream does.
[[nodiscard]] std::vector<std::wstring> BuildForEachRefArguments(int maximumCount = 0);

//! Reproduces upstream `parseRefs`. A line that is not `refs/<name>` followed by
//! a 40-hex object name is skipped rather than guessed at.
[[nodiscard]] std::vector<GitRef> ParseForEachRef(std::string_view bytes);

//! The kinds of row `git.checkout`'s Quick Pick contains. `Separator` is
//! upstream's `RefItemSeparator`, kept in the model even where the native
//! dialog cannot render it, so the model stays the faithful description.
enum class EGitCheckoutItemKind {
	CreateBranch,
	CreateBranchFrom,
	CheckoutDetached,
	Separator,
	Ref,
};

struct GitCheckoutItem {
	EGitCheckoutItemKind kind = EGitCheckoutItemKind::Ref;
	std::wstring label;
	std::wstring description;
	//! Empty for command and separator rows.
	std::wstring refName;
	EGitRefKind refKind = EGitRefKind::Head;
	std::wstring remote;

	[[nodiscard]] bool operator==(const GitCheckoutItem&) const = default;
};

//! Upstream truncates every object name in a Quick Pick description to
//! `git.commitShortHashLength`, whose documented default is 7.
inline constexpr int kGitCommitShortHashLength = 7;

//! Builds the `git.checkout` Quick Pick. `detached` drops the tag group and all
//! three command rows, matching `git.checkoutDetached`.
//!
//! `filterIsEmpty` selects between upstream's two orderings: with no typed
//! filter the command rows come first, otherwise the refs come first and the
//! commands move below a blank separator.
[[nodiscard]] std::vector<GitCheckoutItem> BuildCheckoutItems(
	const std::vector<GitRef>& refs, bool detached, bool filterIsEmpty = true,
	const GitRefTextResolver& text = {});

[[nodiscard]] std::wstring CheckoutPlaceholder(bool detached, const GitRefTextResolver& text = {});

//! Builds the ref picker `git.branchFrom` shows before asking for a name. It
//! leads with upstream's `HEADItem` and then lists all three groups.
[[nodiscard]] std::vector<GitCheckoutItem> BuildBranchFromItems(
	const std::vector<GitRef>& refs, std::wstring_view headCommit,
	const GitRefTextResolver& text = {});

//! Upstream's `git.branchWhitespaceChar` default.
inline constexpr wchar_t kGitBranchWhitespaceChar = L'-';

//! Reproduces upstream `sanitizeBranchName`. An empty name stays empty; every
//! character git would reject becomes `whitespaceChar`.
[[nodiscard]] std::wstring SanitizeBranchName(
	std::wstring_view name, wchar_t whitespaceChar = kGitBranchWhitespaceChar);

enum class EGitBranchNameValidation {
	Valid,
	//! The sanitized name differs from what was typed. Upstream shows this as an
	//! informational message and still accepts the name.
	Sanitized,
	AlreadyExists,
	//! The box is empty, which is upstream's cancel path rather than an error.
	//! A whitespace-only name is *not* empty: upstream sanitizes it to the
	//! whitespace character and proceeds.
	Empty,
};

struct GitBranchNameValidationResult {
	EGitBranchNameValidation state = EGitBranchNameValidation::Valid;
	std::wstring sanitizedName;
	std::wstring message;

	[[nodiscard]] bool operator==(const GitBranchNameValidationResult&) const = default;
};

//! Validates a typed branch name against the existing heads, in upstream's
//! order: existing-name collision first, then the sanitization notice.
[[nodiscard]] GitBranchNameValidationResult ValidateBranchName(
	std::wstring_view name, const std::vector<GitRef>& refs,
	wchar_t whitespaceChar = kGitBranchWhitespaceChar, const GitRefTextResolver& text = {});

//! `git checkout <name>` / `git checkout --detach <name>`.
[[nodiscard]] std::vector<std::wstring> BuildCheckoutArguments(std::wstring_view refName, bool detached);

//! `git checkout -q --track <remoteRef>`, upstream's `checkoutTracking`.
[[nodiscard]] std::vector<std::wstring> BuildCheckoutTrackingArguments(std::wstring_view remoteRefName);

//! `git checkout -q -b <name> --no-track <target>`, which is upstream's
//! `Repository.branch(name, /*checkout*/ true, target)`. Creating a branch from
//! the Quick Pick always checks it out; a `git branch` that leaves HEAD where it
//! was is a different upstream operation.
[[nodiscard]] std::vector<std::wstring> BuildCreateBranchArguments(
	std::wstring_view branchName, std::wstring_view target);

} // namespace workbench::scm
