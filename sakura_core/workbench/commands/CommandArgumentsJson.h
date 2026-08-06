/*! @file
 * @brief The one JSON vocabulary this subsystem uses for command arguments.
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

//!
//! @brief Reading and writing the JSON that carries a command's arguments.
//!
//! `WorkbenchCommandArgumentExecutor` receives one UTF-8 JSON string, so every
//! command that takes an operand has to encode and decode one. These are the
//! primitives all of them share. They live beside the registry that defines
//! that executor signature, so a producer such as `workbench/scm/` depends on
//! the command layer and the command layer never depends back.
//!
//! They are deliberately **not** the JSONC parser under `config/`. That parser
//! is a settings parser, it accepts comments and trailing commas that a wire
//! payload must reject, and depending on it would point this directory at the
//! settings subsystem — the wrong way round. It is equally deliberately not a
//! second private copy per file: two decoders that disagreed by one escape
//! would silently hand git a different path than the one the view rendered,
//! which is the same argument `DecodeGitOutput` exists for.
//!
//! Every reader fails closed. It returns `false` and leaves `index` unusable
//! rather than guessing at malformed input; a caller that cannot say what its
//! operand is must not act on it.
//!
namespace workbench::commands::json {

//! UTF-16 to UTF-8. Empty for text that does not convert.
[[nodiscard]] std::string ToUtf8(std::wstring_view text);

//! Rejects invalid UTF-8 rather than substituting U+FFFD: a path whose bytes
//! did not survive the trip is not a path this may hand to git.
[[nodiscard]] std::optional<std::wstring> ToWideStrict(std::string_view text);

//! Append one string's **contents**, escaped. The quotes are the caller's.
void AppendEscaped(std::string& target, std::string_view text);

//! Append one complete quoted JSON string, converting from UTF-16.
void AppendQuoted(std::string& target, std::wstring_view text);

//! Advance past JSON's four whitespace characters.
void SkipWhitespace(std::string_view text, std::size_t& index) noexcept;

//! Read one quoted string, resolving escapes and surrogate pairs, into UTF-8.
[[nodiscard]] bool ReadString(std::string_view text, std::size_t& index, std::string& value);

//! `ReadString` followed by a strict UTF-8 decode.
[[nodiscard]] bool ReadWideString(std::string_view text, std::size_t& index, std::wstring& value);

//! Read a bare `true` or `false`. Anything else, `null` included, fails.
[[nodiscard]] bool ReadBoolean(std::string_view text, std::size_t& index, bool& value) noexcept;

//! Skip whitespace and consume exactly the expected punctuation character.
[[nodiscard]] bool Expect(std::string_view text, std::size_t& index, char character) noexcept;

//! Skip trailing whitespace and require that nothing else follows.
[[nodiscard]] bool AtEnd(std::string_view text, std::size_t& index) noexcept;

} // namespace workbench::commands::json
