/*! @file
 * @brief One confirmation the built-in Git provider shows.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace workbench::scm {

//!
//! @brief A confirmation a git command family shows before acting.
//!
//! `choices` is in upstream's argument order, so index 0 is the primary action;
//! dismissal is a cancel and never an implicit yes. `modal` and `warning`
//! reproduce which of `showWarningMessage` / `showInformationMessage` upstream
//! used and whether it passed `{ modal: true }`, because an informational,
//! non-modal message is a materially weaker thing to answer than a modal one.
//!
//! Shared by every command family rather than declared once per family: the
//! commit path and the sync path ask the same *kind* of question, and a caller
//! that renders one must not have to learn a second shape to render the other.
//!
struct GitPrompt final {
	std::wstring message;
	std::wstring detail;
	std::vector<std::wstring> choices;
	bool warning{ true };
	bool modal{ true };

	[[nodiscard]] bool operator==(const GitPrompt&) const = default;
};

//! Presents a confirmation and returns the chosen index, or nothing on dismissal.
using GitPromptPresenter = std::function<std::optional<std::size_t>(const GitPrompt& prompt)>;

} // namespace workbench::scm
