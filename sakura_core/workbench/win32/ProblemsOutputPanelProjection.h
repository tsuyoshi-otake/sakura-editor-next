/*! @file
	@brief HWND-free Problems and Output panel presentation snapshots.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace workbench::problems {
struct ProblemsSnapshot;
}

namespace workbench::output {
struct OutputServiceSnapshot;
}

namespace workbench::win32 {

//! The native panel preserves the complete zero-based half-open marker range.
struct ProblemsPanelRange final {
	std::uint32_t startLine{};
	std::uint32_t startColumn{};
	std::uint32_t endLine{};
	std::uint32_t endColumn{};

	[[nodiscard]] constexpr bool operator==(const ProblemsPanelRange&) const noexcept = default;
};

enum class EProblemsPanelSeverity : std::uint8_t {
	Error,
	Warning,
	Information,
	Hint,
};

//! A value-only native panel entry. resourceUri is a full canonical URI, never a truncated file path.
struct ProblemsPanelEntry final {
	std::wstring resourceUri;
	ProblemsPanelRange range;
	EProblemsPanelSeverity severity{ EProblemsPanelSeverity::Error };
	std::wstring message;
	std::wstring source;
	std::wstring location;

	[[nodiscard]] constexpr bool operator==(const ProblemsPanelEntry&) const noexcept = default;
};

struct ProblemsPanelSnapshot final {
	std::uint64_t revision{};
	bool stopped{};
	std::vector<ProblemsPanelEntry> entries;

	[[nodiscard]] constexpr bool operator==(const ProblemsPanelSnapshot&) const noexcept = default;
};

//! A value-only channel presentation. projectedText covers both Output and structured Log channels.
struct OutputPanelChannel final {
	std::string channelId;
	std::wstring label;
	std::wstring projectedText;
	bool visible{};
	bool lastShowPreservedFocus{};

	[[nodiscard]] constexpr bool operator==(const OutputPanelChannel&) const noexcept = default;
};

struct OutputPanelSnapshot final {
	std::uint64_t revision{};
	bool stopped{};
	std::optional<std::string> activeChannelId;
	std::vector<OutputPanelChannel> channels;

	[[nodiscard]] constexpr bool operator==(const OutputPanelSnapshot&) const noexcept = default;
};

//! Projects a copied MarkerService snapshot into a deterministic native presentation value.
[[nodiscard]] ProblemsPanelSnapshot ProjectProblemsPanel(const problems::ProblemsSnapshot& snapshot);

//! Projects a copied OutputService snapshot into a deterministic native presentation value.
[[nodiscard]] OutputPanelSnapshot ProjectOutputPanel(const output::OutputServiceSnapshot& snapshot);

} // namespace workbench::win32
