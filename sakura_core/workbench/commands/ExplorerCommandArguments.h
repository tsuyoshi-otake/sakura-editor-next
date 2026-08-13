/*! @file
 * @brief The argument list the Explorer's file-operation commands are called with.
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
//! @brief The bound on the one string inside an Explorer command's argument list.
//!
//! The payload is a single `file:` URI, which sits far below this even for an
//! extended-length path. The bound exists so a malformed or hostile payload
//! cannot make a decoder allocate without limit; it is not a statement about
//! how long a real path may be.
//!
inline constexpr std::size_t kMaximumExplorerCommandStringLength = 8192;

//!
//! @brief The Explorer file-operation commands' arguments: `[resource]`.
//!
//! Upstream passes the selected Explorer resource as a real `URI` because its
//! caller and handler share a process; here the argument list is a wire
//! payload, so the URI's string form is what crosses. One struct serves all
//! eight commands (`explorer.newFile`, `explorer.newFolder`, `renameFile`,
//! `moveFileToTrash`, `deleteFile`, `copyFilePath`, `copyRelativeFilePath`,
//! `revealFileInOS`) because their operand is identical: exactly one resource.
//! Upstream's optional second argument - the multi-select resource list - is
//! deliberately not part of this contract while the native Explorer has no
//! multi-select; accepting a list and acting on its first element would be a
//! silent approximation of a request this product cannot honor.
//!
struct ExplorerResourceArguments final {
	std::wstring resourceUri;

	[[nodiscard]] bool operator==(const ExplorerResourceArguments&) const = default;
};

[[nodiscard]] std::string BuildExplorerResourceArguments(const ExplorerResourceArguments& arguments);

//! Fails closed on anything malformed, over-long, empty, or carrying a second
//! argument this contract does not define.
[[nodiscard]] std::optional<ExplorerResourceArguments> ParseExplorerResourceArguments(
	std::string_view argumentsJson);

} // namespace workbench::commands
