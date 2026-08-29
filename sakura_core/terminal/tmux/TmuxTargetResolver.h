/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "terminal/tmux/TmuxCommandTypes.h"

#include <optional>
#include <string_view>

namespace terminal::tmux {

enum class TmuxTargetCode : std::uint8_t {
	Succeeded,
	InvalidSyntax,
	TargetMissing,
	Ambiguous,
};

struct TmuxTargetResult final {
	TmuxTargetCode code{ TmuxTargetCode::InvalidSyntax };
	TmuxResolvedTarget target;
	std::string diagnosticCode;

	[[nodiscard]] bool Succeeded() const noexcept { return code == TmuxTargetCode::Succeeded; }
};

//! Resolves only exact tmux-shaped selectors against one immutable snapshot.
//! It never performs special-target, fuzzy-name, or active-target fallback for
//! an explicitly supplied selector.
class TmuxTargetResolver final {
public:
	[[nodiscard]] static TmuxTargetResult Resolve(
		const TmuxRuntimeSnapshot& snapshot,
		TmuxTargetKind kind,
		std::optional<std::string_view> selector = std::nullopt) noexcept;
};

} // namespace terminal::tmux
