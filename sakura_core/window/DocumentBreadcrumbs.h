/*! @file
	@brief Pure filesystem breadcrumb projection for an editor input.
*/
/*
	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_DOCUMENTBREADCRUMBS_H_
#define SAKURA_DOCUMENTBREADCRUMBS_H_
#pragma once

#include <cwctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace breadcrumbs {

struct DocumentBreadcrumbs final {
	std::vector<std::wstring> segments;
	bool workspaceRelative = false;
};

namespace detail {

[[nodiscard]] inline bool EqualWindowsPathComponent(
	std::wstring_view left, std::wstring_view right) noexcept
{
	if (left.size() != right.size()) return false;
	for (size_t index = 0; index < left.size(); ++index) {
		if (std::towlower(left[index]) != std::towlower(right[index])) return false;
	}
	return true;
}

[[nodiscard]] inline std::vector<std::wstring> RelativeWindowsPathSegments(
	const std::filesystem::path& document, const std::filesystem::path& workspaceRoot)
{
	auto documentPart = document.begin();
	auto rootPart = workspaceRoot.begin();
	for (; rootPart != workspaceRoot.end(); ++rootPart, ++documentPart) {
		if (documentPart == document.end()
			|| !EqualWindowsPathComponent(documentPart->wstring(), rootPart->wstring())) {
			return {};
		}
	}

	std::vector<std::wstring> segments;
	for (; documentPart != document.end(); ++documentPart) {
		auto segment = documentPart->wstring();
		if (!segment.empty() && segment != L"." && segment != L"\\" && segment != L"/") {
			segments.push_back(std::move(segment));
		}
	}
	return segments;
}

} // namespace detail

//! VS Code breadcrumbs are workspace-relative. The longest containing root wins
//! in a multi-root workspace; resources outside every root reveal only the file
//! name rather than leaking an absolute path into workbench chrome.
[[nodiscard]] inline DocumentBreadcrumbs BuildDocumentBreadcrumbs(
	std::wstring_view documentPath, const std::vector<std::wstring>& workspaceRoots)
{
	DocumentBreadcrumbs result;
	if (documentPath.empty()) return result;

	const std::filesystem::path document = std::filesystem::path(documentPath).lexically_normal();
	const auto fileName = document.filename().wstring();
	if (fileName.empty()) return result;
	result.segments.push_back(fileName);

	size_t bestRootLength = 0;
	for (const auto& workspaceRootText : workspaceRoots) {
		if (workspaceRootText.empty()) continue;
		const auto workspaceRoot = std::filesystem::path(workspaceRootText).lexically_normal();
		auto candidate = detail::RelativeWindowsPathSegments(document, workspaceRoot);
		if (candidate.empty()) continue;
		const size_t rootLength = workspaceRoot.native().size();
		if (rootLength >= bestRootLength) {
			bestRootLength = rootLength;
			result.segments = std::move(candidate);
			result.workspaceRelative = true;
		}
	}
	return result;
}

} // namespace breadcrumbs

#endif // SAKURA_DOCUMENTBREADCRUMBS_H_
