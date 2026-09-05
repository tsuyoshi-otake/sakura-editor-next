/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/search/WorkspaceSearchEngine.h"

#include "agent/CSearchAgent.h"
#include "charset/CCodeBase.h"
#include "charset/CCodeFactory.h"
#include "env/CDocTypeManager.h"
#include "extmodule/CBregexp.h"
#include "io/CBinaryStream.h"
#include "io/CFileLoad.h"
#include "env/CShareData.h"
#include "types/CType.h"
#include "util/file.h"

#include <algorithm>
#include <deque>
#include <memory>

namespace workbench::search {
namespace {

//! VS Code's registered `files.exclude` and `search.exclude` defaults, as plain
//! directory names because both default sets are `**/<name>` folder globs. A
//! user-editable glob surface is a separate feature; these are the defaults a
//! real VS Code applies before the user touches anything.
constexpr std::wstring_view kExcludedFolderNames[] = {
	L".git", L".svn", L".hg", L"CVS", L".DS_Store",
	L"node_modules", L"bower_components",
};

//! How deep the walk goes. A workspace tree deeper than this is pathological,
//! and an unbounded recursion here would be a stack hazard on a junction loop.
constexpr int kMaximumDepth = 64;

//! Files at or above this size are skipped, matching the intent of upstream's
//! `maxFileSize` guard on the text search provider.
constexpr ULONGLONG kMaximumFileBytes = 16ull * 1024ull * 1024ull;

[[nodiscard]] bool IsExcludedFolder(std::wstring_view name) noexcept
{
	return std::ranges::any_of(kExcludedFolderNames,
		[name](std::wstring_view excluded) { return ::_wcsicmp(std::wstring(name).c_str(),
			std::wstring(excluded).c_str()) == 0; });
}

[[nodiscard]] bool Cancelled(const SearchCancelPredicate& cancelled)
{
	return cancelled && cancelled();
}

//! A line the text search must not report. A NUL means the file is binary in
//! the same sense upstream's binary detection means it.
[[nodiscard]] bool LooksBinary(const wchar_t* line, int length) noexcept
{
	for (int index = 0; index < length; ++index) {
		if (line[index] == L'\0') return true;
	}
	return false;
}

//! Trims the leading whitespace once, exactly as upstream's preview does, and
//! bounds the stored text so one very long line cannot dominate the model.
void BuildPreview(const wchar_t* line, int lineLength, int matchOffset, int matchLength,
	SearchMatch& match)
{
	int start = 0;
	while (start < lineLength && (line[start] == L' ' || line[start] == L'\t')) ++start;
	// Never trim past the match itself: a match inside the indentation must stay visible.
	start = std::min(start, matchOffset);
	// Keep the hit in the bounded window; a long hit starts at the window's
	// beginning and retains its original source length in SearchMatch::length.
	const int visibleHit = std::min(matchLength, kSearchPreviewMaxLength);
	if (matchOffset - start > kSearchPreviewMaxLength - visibleHit) {
		const int context = std::min(40, kSearchPreviewMaxLength - visibleHit);
		start = std::max(start, matchOffset - context);
	}
	const auto high = [](wchar_t c) { return c >= 0xd800 && c <= 0xdbff; };
	const auto low = [](wchar_t c) { return c >= 0xdc00 && c <= 0xdfff; };
	if (start > 0 && start < lineLength && low(line[start]) && high(line[start - 1])) --start;
	int end = start + std::min(lineLength - start, kSearchPreviewMaxLength);
	if (end > start && end < lineLength && high(line[end - 1]) && low(line[end])) --end;
	match.preview.assign(line + start, static_cast<std::size_t>(std::max(0, end - start)));
	match.previewOffset = matchOffset - start;
	match.previewLength = matchLength;
	if (match.previewOffset > static_cast<int>(match.preview.size())) {
		match.previewOffset = static_cast<int>(match.preview.size());
		match.previewLength = 0;
	} else if (match.previewOffset + match.previewLength > static_cast<int>(match.preview.size())) {
		match.previewLength = static_cast<int>(match.preview.size()) - match.previewOffset;
	}
}

//! Everything the per-line matcher needs, compiled once per search.
struct CompiledPattern final {
	SSearchOption option;
	CSearchStringPattern pattern;
	std::unique_ptr<CBregexp> regexp;
	std::vector<std::pair<const wchar_t*, CLogicInt>> words;
	std::wstring key;
	bool valid = false;
	ESearchCompletion failure = ESearchCompletion::Complete;
	std::wstring failureText;
};

void CompileQuery(const SearchQuery& query, CompiledPattern& compiled)
{
	compiled.key = query.text;
	compiled.option = SSearchOption(query.useRegex, query.matchCase, query.wholeWord);
	if (query.useRegex) {
		compiled.regexp = std::make_unique<CBregexp>();
		// `bShowMessage` is false on purpose: this runs on a worker thread, where a
		// modal message box would block the search behind a dialog nobody asked for.
		if (!InitRegexp(nullptr, *compiled.regexp, false)) {
			compiled.failure = ESearchCompletion::RegexUnavailable;
			return;
		}
		const int flags = query.matchCase ? CBregexp::optCaseSensitive : CBregexp::optNothing;
		if (!compiled.regexp->Compile(compiled.key.c_str(), flags)) {
			compiled.failure = ESearchCompletion::InvalidPattern;
			compiled.failureText = compiled.regexp->GetLastMessage();
			return;
		}
		compiled.valid = true;
		return;
	}
	if (query.wholeWord) {
		CSearchAgent::CreateWordList(compiled.words, compiled.key.c_str(),
			static_cast<int>(compiled.key.size()));
		compiled.valid = true;
		return;
	}
	if (!compiled.pattern.SetPattern(nullptr, compiled.key.c_str(), compiled.key.size(),
			compiled.option, nullptr)) {
		compiled.failure = ESearchCompletion::InvalidPattern;
		return;
	}
	compiled.valid = true;
}

//! One match inside a line, in line-local coordinates.
struct LineHit final {
	int offset = 0;
	int length = 0;
};

//! Every hit on one line, left to right and non-overlapping, exactly as
//! upstream's `TextSearchProvider` reports them.
void CollectLineHits(const CompiledPattern& compiled, const SearchQuery& query,
	const wchar_t* line, int lineLength, std::vector<LineHit>& hits)
{
	hits.clear();
	if (lineLength <= 0) return;
	if (query.useRegex) {
		int index = 0;
		while (index <= lineLength && compiled.regexp->Match(line, lineLength, index)) {
			const int start = static_cast<int>(compiled.regexp->GetIndex());
			int length = static_cast<int>(compiled.regexp->GetMatchLen());
			hits.push_back(LineHit{ start, length });
			if (length <= 0) {
				length = CNativeW::GetSizeOfChar(line, lineLength, start);
				if (length <= 0) length = 1;
			}
			index = start + length;
		}
		return;
	}
	if (query.wholeWord) {
		int index = 0;
		int matchLength = 0;
		const wchar_t* found = nullptr;
		while ((found = CSearchAgent::SearchStringWord(line, lineLength, index, compiled.words,
				query.matchCase, &matchLength)) != nullptr) {
			const int start = static_cast<int>(found - line);
			hits.push_back(LineHit{ start, matchLength });
			index = start + std::max(1, matchLength);
		}
		return;
	}
	const int keyLength = static_cast<int>(compiled.key.size());
	if (keyLength <= 0) return;
	int index = 0;
	while (index <= lineLength - keyLength) {
		const wchar_t* found = CSearchAgent::SearchString(line + index, lineLength - index, 0,
			compiled.pattern);
		if (found == nullptr) break;
		const int start = index + static_cast<int>(found - (line + index));
		hits.push_back(LineHit{ start, keyLength });
		index = start + keyLength;
	}
}

//! The encoding configuration a file would be opened with in an editor window,
//! so the search reads a Shift_JIS file the same way opening it would.
[[nodiscard]] const STypeConfigMini* TypeConfigFor(const std::wstring& path)
{
	const STypeConfigMini* type = nullptr;
	if (!CDocTypeManager().GetTypeConfigMini(CDocTypeManager().GetDocumentTypeOfPath(path.c_str()), &type)) {
		return nullptr;
	}
	return type;
}

//! Searches one file. Returns false only when the file could not be read at all.
bool SearchOneFile(const std::wstring& fullPath, const SearchQuery& query,
	const CompiledPattern& compiled, const SearchCancelPredicate& cancelled,
	std::size_t remainingBudget, std::vector<SearchMatch>& matches, bool& limitHit)
{
	const STypeConfigMini* type = TypeConfigFor(fullPath);
	if (type == nullptr) return false;
	CFileLoad loader(type->m_encoding);
	CNativeW lineBuffer;
	CEol eol;
	std::int64_t lineNumber = 0;
	std::vector<LineHit> hits;
	try {
		(void)loader.FileOpen(fullPath.c_str(), true, CODE_AUTODETECT,
			GetDllShareData().m_Common.m_sFile.GetAutoMIMEdecode(), nullptr);
		while (loader.ReadLine(&lineBuffer, &eol) != RESULT_FAILURE) {
			++lineNumber;
			if (Cancelled(cancelled)) break;
			const wchar_t* const line = lineBuffer.GetStringPtr();
			// `ReadLine` hands back the EOL as part of the line; a hit must never
			// span it, and the preview must never render it.
			const int lineLength = std::max<int>(0, lineBuffer.GetStringLength() - eol.GetLen());
			if (LooksBinary(line, lineLength)) {
				matches.clear();
				break;
			}
			CollectLineHits(compiled, query, line, lineLength, hits);
			for (const auto& hit : hits) {
				if (matches.size() >= remainingBudget) {
					limitHit = true;
					return true;
				}
				SearchMatch match;
				match.line = lineNumber;
				match.column = hit.offset + 1;
				match.length = hit.length;
				BuildPreview(line, lineLength, hit.offset, hit.length, match);
				matches.push_back(std::move(match));
			}
		}
	} catch (...) {
		// A file that stops being readable mid-way contributes what it already
		// produced rather than failing the whole search. An open failure contributes none.
		return !matches.empty();
	}
	loader.FileClose();
	return true;
}

//! Splits `fullPath` into the parts one result row renders.
void SplitResultPath(const std::wstring& root, const std::wstring& fullPath,
	SearchFileResult& result)
{
	const std::size_t separator = fullPath.find_last_of(L"\\/");
	result.fileName = separator == std::wstring::npos ? fullPath : fullPath.substr(separator + 1);
	std::wstring folder = separator == std::wstring::npos ? std::wstring{} : fullPath.substr(0, separator);
	if (!root.empty() && folder.size() >= root.size()
		&& ::_wcsnicmp(folder.c_str(), root.c_str(), root.size()) == 0) {
		folder = folder.substr(root.size());
		while (!folder.empty() && (folder.front() == L'\\' || folder.front() == L'/')) {
			folder.erase(folder.begin());
		}
	}
	result.folderLabel = std::move(folder);
}

void SearchFolder(const std::wstring& root, const std::wstring& folder, const SearchQuery& query,
	const CompiledPattern& compiled, const SearchCancelPredicate& cancelled, int depth,
	SearchResults& results)
{
	if (depth > kMaximumDepth || Cancelled(cancelled)) return;
	if (results.completion == ESearchCompletion::LimitHit) return;
	std::wstring spec = folder;
	if (!spec.empty() && spec.back() != L'\\' && spec.back() != L'/') spec += L'\\';
	const std::wstring prefix = spec;
	spec += L'*';
	WIN32_FIND_DATAW found{};
	const HANDLE handle = ::FindFirstFileExW(spec.c_str(), FindExInfoBasic, &found,
		FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
	if (handle == INVALID_HANDLE_VALUE) return;
	std::vector<std::wstring> subFolders;
	do {
		if (Cancelled(cancelled)) break;
		const std::wstring_view name = found.cFileName;
		if (name == L"." || name == L"..") continue;
		if ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;
		if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
			if (IsExcludedFolder(name)) continue;
			subFolders.push_back(prefix + std::wstring(name));
			continue;
		}
		if (IsExcludedFolder(name)) continue;
		const ULONGLONG size = (static_cast<ULONGLONG>(found.nFileSizeHigh) << 32)
			| static_cast<ULONGLONG>(found.nFileSizeLow);
		if (size == 0 || size >= kMaximumFileBytes) continue;
		const std::wstring fullPath = prefix + std::wstring(name);
		std::vector<SearchMatch> matches;
		bool limitHit = false;
		const std::size_t budget = kSearchMaxResults - std::min(kSearchMaxResults, results.matchCount);
		if (budget == 0) {
			results.completion = ESearchCompletion::LimitHit;
			break;
		}
		if (!SearchOneFile(fullPath, query, compiled, cancelled, budget, matches, limitHit)) continue;
		if (!matches.empty()) {
			results.matchCount += matches.size();
			SearchFileResult result;
			result.fullPath = fullPath;
			SplitResultPath(root, fullPath, result);
			result.matches = std::move(matches);
			results.files.push_back(std::move(result));
		}
		if (limitHit) {
			results.completion = ESearchCompletion::LimitHit;
			break;
		}
	} while (::FindNextFileW(handle, &found) != FALSE);
	::FindClose(handle);
	for (const auto& subFolder : subFolders) {
		if (Cancelled(cancelled) || results.completion == ESearchCompletion::LimitHit) break;
		SearchFolder(root, subFolder, query, compiled, cancelled, depth + 1, results);
	}
}

//! The casing shape `preserveCase` recognizes.
enum class ECasePattern { Mixed, Upper, Lower, Title };

[[nodiscard]] ECasePattern DetectCase(std::wstring_view text) noexcept
{
	bool anyUpper = false;
	bool anyLower = false;
	bool restLower = true;
	for (std::size_t index = 0; index < text.size(); ++index) {
		const wchar_t character = text[index];
		if (::iswupper(character)) {
			anyUpper = true;
			if (index != 0) restLower = false;
		} else if (::iswlower(character)) {
			anyLower = true;
		}
	}
	if (anyUpper && !anyLower) return ECasePattern::Upper;
	if (anyLower && !anyUpper) return ECasePattern::Lower;
	if (anyUpper && anyLower && restLower && ::iswupper(text.front())) return ECasePattern::Title;
	return ECasePattern::Mixed;
}

} // namespace

std::wstring ApplyPreserveCase(std::wstring_view replacement, std::wstring_view matched)
{
	std::wstring result(replacement);
	if (matched.empty() || result.empty()) return result;
	switch (DetectCase(matched)) {
	case ECasePattern::Upper:
		for (auto& character : result) character = static_cast<wchar_t>(::towupper(character));
		break;
	case ECasePattern::Lower:
		for (auto& character : result) character = static_cast<wchar_t>(::towlower(character));
		break;
	case ECasePattern::Title:
		for (auto& character : result) character = static_cast<wchar_t>(::towlower(character));
		result.front() = static_cast<wchar_t>(::towupper(result.front()));
		break;
	case ECasePattern::Mixed:
		break;
	}
	return result;
}

SearchResults RunWorkspaceSearch(std::wstring_view root, const SearchQuery& query,
	const SearchCancelPredicate& cancelled)
{
	SearchResults results;
	if (query.text.empty()) return results;
	if (root.empty()) {
		results.completion = ESearchCompletion::NoWorkspace;
		return results;
	}
	CompiledPattern compiled;
	CompileQuery(query, compiled);
	if (!compiled.valid) {
		results.completion = compiled.failure;
		results.failureText = compiled.failureText;
		return results;
	}
	std::wstring rootPath(root);
	while (rootPath.size() > 3 && (rootPath.back() == L'\\' || rootPath.back() == L'/')) {
		rootPath.pop_back();
	}
	SearchFolder(rootPath, rootPath, query, compiled, cancelled, 0, results);
	if (Cancelled(cancelled)) results.completion = ESearchCompletion::Cancelled;
	return results;
}

ReplaceOutcome ReplaceMatches(const std::vector<SearchFileResult>& files, const SearchQuery& query,
	const SearchCancelPredicate& cancelled)
{
	ReplaceOutcome outcome;
	if (query.text.empty()) return outcome;
	CompiledPattern compiled;
	CompileQuery(query, compiled);
	if (!compiled.valid) {
		for (const auto& file : files) outcome.failedFiles.push_back(file.fullPath);
		return outcome;
	}
	// Regular-expression replacement needs its own compilation, because the
	// replacement text carries `$1` group references that only the pattern that
	// was compiled together with it can expand.
	std::unique_ptr<CBregexp> replacer;
	if (query.useRegex) {
		replacer = std::make_unique<CBregexp>();
		if (!InitRegexp(nullptr, *replacer, false)
			|| !replacer->Compile(query.text.c_str(), query.replaceText.c_str(),
				query.matchCase ? CBregexp::optCaseSensitive : CBregexp::optNothing)) {
			for (const auto& file : files) outcome.failedFiles.push_back(file.fullPath);
			return outcome;
		}
	}
	std::vector<LineHit> hits;
	for (const auto& file : files) {
		if (Cancelled(cancelled)) break;
		if (file.matches.empty()) continue;
		const STypeConfigMini* type = TypeConfigFor(file.fullPath);
		if (type == nullptr) {
			outcome.failedFiles.push_back(file.fullPath);
			continue;
		}
		CFileLoad loader(type->m_encoding);
		ECodeType codeType = CODE_AUTODETECT;
		bool bom = false;
		try {
			codeType = loader.FileOpen(file.fullPath.c_str(), true, CODE_AUTODETECT,
				GetDllShareData().m_Common.m_sFile.GetAutoMIMEdecode(), &bom);
		} catch (...) {
			outcome.failedFiles.push_back(file.fullPath);
			continue;
		}
		// The whole file is rebuilt in memory first: a partially rewritten file
		// on a mid-way mismatch would be worse than not replacing at all.
		CNativeW rebuilt;
		CNativeW lineBuffer;
		CEol eol;
		std::int64_t lineNumber = 0;
		std::size_t matchIndex = 0;
		std::size_t replaced = 0;
		bool mismatch = false;
		try {
			while (loader.ReadLine(&lineBuffer, &eol) != RESULT_FAILURE) {
				++lineNumber;
				const wchar_t* const line = lineBuffer.GetStringPtr();
				const int lineLength = lineBuffer.GetStringLength();
				const int textLength = lineLength - eol.GetLen();
				int emitted = 0;
				while (matchIndex < file.matches.size()
					&& file.matches[matchIndex].line == lineNumber) {
					const auto& match = file.matches[matchIndex];
					const int offset = match.column - 1;
					if (offset < emitted || offset + match.length > std::max(0, textLength)) {
						mismatch = true;
						break;
					}
					// Prove the recorded position still holds the recorded match
					// before overwriting it, so a file edited since the search is
					// reported as failed rather than silently corrupted.
					CollectLineHits(compiled, query, line, textLength, hits);
					const bool stillMatches = std::ranges::any_of(hits, [&match, offset](const LineHit& hit) {
						return hit.offset == offset && hit.length == match.length;
					});
					if (!stillMatches) {
						mismatch = true;
						break;
					}
					rebuilt.AppendString(line + emitted, offset - emitted);
					const std::wstring_view matched(line + offset, static_cast<std::size_t>(match.length));
					std::wstring replacement;
					if (query.useRegex) {
						// Without `optGlobal` this replaces the single match at
						// `offset` and returns the rewritten tail from `offset`
						// onward, so the replacement itself is that tail minus the
						// untouched remainder after the match.
						if (replacer->Replace(line, textLength, offset) <= 0) {
							mismatch = true;
							break;
						}
						const int rewrittenLength = static_cast<int>(replacer->GetStringLen());
						const int remainder = textLength - (offset + match.length);
						const int replacementLength = rewrittenLength - remainder;
						if (replacementLength < 0) {
							mismatch = true;
							break;
						}
						replacement.assign(replacer->GetString(),
							static_cast<std::size_t>(replacementLength));
					} else {
						replacement = query.preserveCase
							? ApplyPreserveCase(query.replaceText, matched)
							: query.replaceText;
					}
					rebuilt.AppendString(replacement.c_str(), static_cast<int>(replacement.size()));
					emitted = offset + match.length;
					++matchIndex;
					++replaced;
				}
				if (mismatch) break;
				rebuilt.AppendString(line + emitted, lineLength - emitted);
			}
		} catch (...) {
			mismatch = true;
		}
		loader.FileClose();
		if (mismatch || matchIndex != file.matches.size() || replaced == 0) {
			outcome.failedFiles.push_back(file.fullPath);
			continue;
		}
		std::unique_ptr<CCodeBase> codeBase(CCodeFactory::CreateCodeBase(codeType, 0));
		if (!codeBase) {
			outcome.failedFiles.push_back(file.fullPath);
			continue;
		}
		std::wstring temporaryPath = file.fullPath + L".skrnew";
		bool written = false;
		try {
			CBinaryOutputStream out(temporaryPath.c_str(), true);
			if (bom) {
				CMemory bomBytes;
				codeBase->GetBom(&bomBytes);
				out.Write(bomBytes.GetRawPtr(), bomBytes.GetRawLength());
			}
			CMemory encoded;
			codeBase->UnicodeToCode(rebuilt, &encoded);
			out.Write(encoded.GetRawPtr(), encoded.GetRawLength());
			out.Close();
			written = true;
		} catch (...) {
			written = false;
		}
		if (!written) {
			(void)::DeleteFileW(temporaryPath.c_str());
			outcome.failedFiles.push_back(file.fullPath);
			continue;
		}
		if (::MoveFileExW(temporaryPath.c_str(), file.fullPath.c_str(), MOVEFILE_REPLACE_EXISTING) == FALSE) {
			(void)::DeleteFileW(temporaryPath.c_str());
			outcome.failedFiles.push_back(file.fullPath);
			continue;
		}
		outcome.replacedMatches += replaced;
		++outcome.replacedFiles;
	}
	return outcome;
}

} // namespace workbench::search
