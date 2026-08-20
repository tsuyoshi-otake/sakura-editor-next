/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/scm/GitHistoryModel.h"

#include <algorithm>
#include <charconv>

namespace workbench::scm {
namespace {

std::wstring ToWide(std::string_view value)
{
	if (value.empty()) return {};
	const int length = ::MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
		nullptr, 0);
	if (length <= 0) return {};
	std::wstring result(static_cast<std::size_t>(length), L'\0');
	::MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
		result.data(), length);
	return result;
}

std::wstring_view Trim(std::wstring_view value)
{
	while (!value.empty() && (value.front() == L' ' || value.front() == L'\r' || value.front() == L'\n')) {
		value.remove_prefix(1);
	}
	while (!value.empty() && (value.back() == L' ' || value.back() == L'\r' || value.back() == L'\n')) {
		value.remove_suffix(1);
	}
	return value;
}

//! Split on one separator, keeping empty fields: an empty parent list and an
//! empty decoration list are both meaningful.
std::vector<std::wstring_view> Split(std::wstring_view value, wchar_t separator)
{
	std::vector<std::wstring_view> parts;
	for (;;) {
		const auto found = value.find(separator);
		if (found == std::wstring_view::npos) { parts.push_back(value); break; }
		parts.push_back(value.substr(0, found));
		value.remove_prefix(found + 1);
	}
	return parts;
}

std::int64_t ParseTimestamp(std::wstring_view value)
{
	value = Trim(value);
	std::int64_t seconds = 0;
	bool any = false;
	for (const wchar_t character : value) {
		if (character < L'0' || character > L'9') break;
		seconds = seconds * 10 + (character - L'0');
		any = true;
	}
	return any ? seconds : 0;
}

} // namespace

std::wstring MakeGitHistoryFormat()
{
	std::wstring format = L"--format=%H%x1f%P%x1f%D%x1f%an%x1f%ae%x1f%at%x1f%s%x1e";
	return format;
}

std::vector<std::wstring> MakeGitHistoryArguments(std::size_t maximumCount)
{
	return {
		L"log",
		L"--topo-order",
		L"--max-count=" + std::to_wstring(maximumCount == 0 ? 1u : maximumCount),
		MakeGitHistoryFormat(),
	};
}

std::vector<GitHistoryRef> ParseGitHistoryRefs(std::wstring_view decorations)
{
	std::vector<GitHistoryRef> refs;
	for (const auto part : Split(decorations, L',')) {
		auto name = Trim(part);
		if (name.empty()) continue;
		GitHistoryRef ref;
		// `HEAD -> main` is one decoration naming the checked-out branch. Upstream
		// badges that branch as the current one, so the arrow is what carries the
		// distinction and must not be dropped with the prefix.
		if (const auto arrow = name.find(L" -> "); arrow != std::wstring_view::npos
			&& name.substr(0, arrow) == L"HEAD") {
			ref.kind = EGitHistoryRefKind::Head;
			ref.name = std::wstring(Trim(name.substr(arrow + 4)));
			refs.push_back(std::move(ref));
			continue;
		}
		if (name.starts_with(L"tag: ")) {
			ref.kind = EGitHistoryRefKind::Tag;
			ref.name = std::wstring(Trim(name.substr(5)));
			refs.push_back(std::move(ref));
			continue;
		}
		if (name == L"HEAD") {
			// A detached HEAD decorates the commit by itself.
			ref.kind = EGitHistoryRefKind::Head;
			ref.name = L"HEAD";
			refs.push_back(std::move(ref));
			continue;
		}
		// git's decoration list carries short names only, so a name with a slash
		// is a remote-tracking branch (`origin/main`) and one without is local.
		ref.kind = name.find(L'/') == std::wstring_view::npos
			? EGitHistoryRefKind::LocalBranch : EGitHistoryRefKind::RemoteBranch;
		ref.name = std::wstring(name);
		refs.push_back(std::move(ref));
	}
	return refs;
}

std::vector<GitHistoryItem> ParseGitHistory(std::string_view bytes)
{
	std::vector<GitHistoryItem> items;
	const std::wstring text = ToWide(bytes);
	for (auto record : Split(text, kGitHistoryRecordSeparator)) {
		record = Trim(record);
		if (record.empty()) continue;
		const auto fields = Split(record, kGitHistoryFieldSeparator);
		// Seven fields, in the order `MakeGitHistoryFormat` asks for them. A record
		// with fewer is not a commit we can describe, and guessing which field is
		// missing would attach a subject to the wrong commit.
		if (fields.size() < 7) continue;
		GitHistoryItem item;
		item.id = std::wstring(Trim(fields[0]));
		if (item.id.empty()) continue;
		for (const auto parent : Split(fields[1], L' ')) {
			const auto trimmed = Trim(parent);
			if (!trimmed.empty()) item.parentIds.emplace_back(trimmed);
		}
		item.refs = ParseGitHistoryRefs(fields[2]);
		item.authorName = std::wstring(Trim(fields[3]));
		item.authorEmail = std::wstring(Trim(fields[4]));
		item.authorTimestamp = ParseTimestamp(fields[5]);
		// The subject is the last field and is taken verbatim: only its record
		// separator was removed, so a subject containing a space or a colon
		// survives unchanged.
		item.subject = std::wstring(fields[6]);
		items.push_back(std::move(item));
	}
	return items;
}

std::vector<ScmGraphRow> BuildScmHistoryGraph(const std::vector<GitHistoryItem>& items)
{
	std::vector<ScmGraphRow> rows;
	rows.reserve(items.size());
	std::vector<ScmGraphSwimlane> inputSwimlanes;
	std::size_t nextColorIndex = 0;
	const auto takeColor = [&nextColorIndex]() {
		const std::size_t color = nextColorIndex % kScmGraphColorCount;
		nextColorIndex = (nextColorIndex + 1) % kScmGraphColorCount;
		return color;
	};

	for (const auto& item : items) {
		ScmGraphRow row;
		row.inputSwimlanes = inputSwimlanes;

		std::vector<ScmGraphSwimlane> outputSwimlanes;
		bool circleFound = false;
		bool firstParentPlaced = false;
		for (std::size_t index = 0; index < inputSwimlanes.size(); ++index) {
			const auto& lane = inputSwimlanes[index];
			if (lane.id != item.id) { outputSwimlanes.push_back(lane); continue; }
			if (!circleFound) {
				circleFound = true;
				row.circleLane = index;
				row.circleColorIndex = lane.colorIndex;
			}
			// The first lane waiting for this commit continues into its first
			// parent; any further lane waiting for the same commit is a merge
			// arriving here and simply ends.
			if (!firstParentPlaced && !item.parentIds.empty()) {
				firstParentPlaced = true;
				outputSwimlanes.push_back(ScmGraphSwimlane{ item.parentIds.front(), lane.colorIndex });
			}
		}
		if (!circleFound) {
			// Nothing was waiting for this commit, so it starts its own lane at the
			// right-hand end: a tip that no already-drawn commit descends from.
			row.circleLane = outputSwimlanes.size();
			row.circleColorIndex = takeColor();
			if (!item.parentIds.empty()) {
				firstParentPlaced = true;
				outputSwimlanes.push_back(ScmGraphSwimlane{ item.parentIds.front(), row.circleColorIndex });
			}
		}
		for (std::size_t parent = 1; parent < item.parentIds.size(); ++parent) {
			const auto& id = item.parentIds[parent];
			const bool present = std::any_of(outputSwimlanes.begin(), outputSwimlanes.end(),
				[&id](const ScmGraphSwimlane& lane) { return lane.id == id; });
			if (present) continue;
			outputSwimlanes.push_back(ScmGraphSwimlane{ id, takeColor() });
		}

		row.outputSwimlanes = outputSwimlanes;
		inputSwimlanes = std::move(outputSwimlanes);
		rows.push_back(std::move(row));
	}
	return rows;
}

} // namespace workbench::scm
