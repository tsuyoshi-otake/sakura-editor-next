/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace editor::document {

//! A half-open byte range in the editor's UTF-8 document representation.
class TextRange final {
public:
	constexpr TextRange(std::size_t start = 0, std::size_t end = 0) noexcept
		: m_start(start), m_end(end) {}
	[[nodiscard]] constexpr std::size_t Start() const noexcept { return m_start; }
	[[nodiscard]] constexpr std::size_t End() const noexcept { return m_end; }

private:
	const std::size_t m_start;
	const std::size_t m_end;
};

enum class ETextEditOutcome : std::uint8_t {
	Applied,
	NoChange,
	InvalidRange,
	Failed,
};

class TextEditResult final {
public:
	TextEditResult(ETextEditOutcome outcome = ETextEditOutcome::Failed,
		std::uint64_t version = 0, bool changed = false) noexcept
		: m_outcome(outcome), m_version(version), m_changed(changed) {}
	[[nodiscard]] ETextEditOutcome Outcome() const noexcept { return m_outcome; }
	[[nodiscard]] std::uint64_t Version() const noexcept { return m_version; }
	[[nodiscard]] bool Changed() const noexcept { return m_changed; }

private:
	const ETextEditOutcome m_outcome;
	const std::uint64_t m_version;
	const bool m_changed;
};

//! Owns the logical UTF-8 text and its monotonic content version.
//!
//! It deliberately knows neither visual wrapping, a selection, a file path, nor
//! persistence. Callers retain their own presentation coordinates and translate
//! them through LayoutProjection.
class TextDocument final {
public:
	explicit TextDocument(std::string initialText = {});

	[[nodiscard]] const std::string& Text() const noexcept { return m_text; }
	[[nodiscard]] std::uint64_t Version() const noexcept { return m_version; }
	[[nodiscard]] std::size_t Size() const noexcept { return m_text.size(); }

	[[nodiscard]] TextEditResult Replace(TextRange range, std::string_view replacement) noexcept;
	//! Replaces the complete logical content. Passing the value by value permits a
	//! caller to prepare its allocation before this object commits a version.
	[[nodiscard]] TextEditResult ReplaceAll(std::string replacement) noexcept;

private:
	[[nodiscard]] TextEditResult Commit(std::string replacement) noexcept;

	std::string m_text;
	std::uint64_t m_version = 0;
};

} // namespace editor::document
