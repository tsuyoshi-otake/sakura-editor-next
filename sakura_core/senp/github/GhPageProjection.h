/*! @file
 * @brief Reduces a repositoryRead response to the fields its shape names.
 */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include <sakura/serialization/JsoncDocument.h>

#include <optional>
#include <string>
#include <string_view>

namespace senp::github {

/*!
	@brief Whether a repositoryRead shape has a stated field set.

	The shape vocabulary is closed where a read is built, so every shape that can
	reach a response has one. Asking is what lets a test walk that vocabulary and
	fail on a shape whose fields nobody wrote down, instead of discovering it as a
	page that silently travels whole.
*/
[[nodiscard]] bool HasRepositoryPageProjection(std::wstring_view shape) noexcept;

/*!
	@brief Reduce one repositoryRead response body to the fields the shape names.

	A completion carries its body inline and the wire bounds that body at 64 KiB,
	while a real GitHub list page is several hundred. The difference is almost
	entirely members no extension reads - issue bodies, reaction counts, the full
	actor record behind a login - so the boundary states, per shape, which members
	the answer consists of and drops the rest.

	Nothing is invented: a member that is absent stays absent, which is what keeps
	`pull_request` meaningful as the marker separating a pull request from an
	issue. A body that is not the JSON the shape describes yields no projection
	rather than a partial one.
*/
[[nodiscard]] std::optional<std::string> ProjectRepositoryPage(
	std::wstring_view shape, const std::string& body);

/*!
	@brief The same projection over an already-validated page DOM.

	The read boundary parses every page once for validation; the DOM it built
	is moved in here rather than parsed again. The document is consumed - a
	second read wanting the same page falls back to the body overload. A
	distinct name, not an overload: a `const char*` would otherwise convert to
	`bool` and silently bind the JsoncValue constructor.
*/
[[nodiscard]] std::optional<std::string> ProjectRepositoryPageDocument(
	std::wstring_view shape, platform::serialization::JsoncValue document);

} // namespace senp::github
