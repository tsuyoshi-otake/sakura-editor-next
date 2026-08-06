/*! @file
 * @brief The argument lists VS Code's own API commands are called with.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace workbench::commands {

//!
//! @brief The bound on one string inside an API command's argument list.
//!
//! A URI carrying a repository path and a `git:` query is the longest thing
//! that legitimately appears here, and it is far below this. The bound exists
//! so a malformed or hostile payload cannot make a decoder allocate without
//! limit; it is not a statement about how long a real path may be.
//!
inline constexpr std::size_t kMaximumApiCommandStringLength = 8192;

//!
//! @brief Upstream `vscode.diff`'s arguments: `[left, right, title]`.
//!
//! The URIs travel as their string forms. Upstream passes real `URI` values
//! because its caller and its handler share a process; here the argument list
//! is a wire payload, so the string form is the only thing that can cross it —
//! and it is the same string an extension would pass through `vscode.diff`,
//! which is the point of registering the API command at all.
//!
struct ApiDiffArguments final {
	std::wstring originalUri;
	std::wstring modifiedUri;
	//! Upstream's optional `title`. Empty means the argument was absent.
	std::wstring title;

	[[nodiscard]] bool operator==(const ApiDiffArguments&) const = default;
};

//!
//! @brief Upstream `vscode.open`'s arguments: `[resource, options, label]`.
//!
struct ApiOpenArguments final {
	std::wstring resourceUri;
	//!
	//! @brief `TextDocumentShowOptions.override`, VS Code's editor override.
	//!
	//! The built-in Git provider sets it to `false` for a both-modified merge
	//! resource, which forces the **default** text editor rather than any
	//! registered custom editor, and leaves it undefined otherwise. Absent and
	//! `false` are therefore different requests and are kept apart here even
	//! though this product has no custom editors to override yet.
	//!
	std::optional<bool> overrideEditor;
	//! Upstream's optional `label`. Empty means the argument was absent.
	std::wstring label;

	[[nodiscard]] bool operator==(const ApiOpenArguments&) const = default;
};

[[nodiscard]] std::string BuildApiDiffArguments(const ApiDiffArguments& arguments);

//! Fails closed on anything malformed, over-long, or carrying a member the
//! command does not define. An absent trailing argument is not malformed.
[[nodiscard]] std::optional<ApiDiffArguments> ParseApiDiffArguments(std::string_view argumentsJson);

[[nodiscard]] std::string BuildApiOpenArguments(const ApiOpenArguments& arguments);

[[nodiscard]] std::optional<ApiOpenArguments> ParseApiOpenArguments(std::string_view argumentsJson);

} // namespace workbench::commands
