/*! @file */
/*
Copyright (C) 2026, Sakura Editor Organization

SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "senp/SenpLanguageService.h"

#include "senp/SenpManagementService.h"
#include "textmate/OnigmoRegexEngine.h"
#include "textmate/TextMateJsonGrammarLoader.h"
#include "textmate/TextMatePlistGrammarLoader.h"
#include "textmate/TextMateUtf8.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace senp {
namespace {

constexpr std::uintmax_t kMaximumGrammarBytes = 32U * 1024U * 1024U;

std::wstring Fold(std::wstring_view value)
{
	std::wstring folded(value);
	std::ranges::transform(folded, folded.begin(), [](wchar_t ch) {
		return static_cast<wchar_t>(::towlower(ch));
	});
	return folded;
}

// VS Code glob tokens: '*' stays inside one path segment, '?' matches one
// non-'/' character, and '**' is special only as a whole segment. A '**'
// segment not at the end absorbs its trailing '/' and matches zero or more
// complete segments; a trailing '**' absorbs its leading '/' and also matches
// the bare prefix. A '**' run that is not a whole segment degrades to '*'.
// Every other character is a literal: '{' and '[' carry no syntax here.
enum class GlobTokenKind { Literal, Single, Star, GlobStar, GlobStarSlash, SlashGlobStar };

struct GlobToken final {
	GlobTokenKind kind;
	wchar_t literal;
};

std::vector<GlobToken> TokenizeGlob(std::wstring_view pattern)
{
	std::vector<GlobToken> tokens;
	for (std::size_t index = 0; index < pattern.size();) {
		const wchar_t ch = pattern[index];
		if (ch == L'*' && index + 1 < pattern.size() && pattern[index + 1] == L'*'
			&& (index == 0 || pattern[index - 1] == L'/')
			&& (index + 2 == pattern.size() || pattern[index + 2] == L'/')) {
			if (index + 2 < pattern.size()) {
				tokens.push_back({ GlobTokenKind::GlobStarSlash, 0 });
				index += 3;
			} else if (index == 0) {
				tokens.push_back({ GlobTokenKind::GlobStar, 0 });
				index += 2;
			} else if (!tokens.empty() && tokens.back().kind == GlobTokenKind::GlobStarSlash) {
				tokens.push_back({ GlobTokenKind::GlobStar, 0 });
				index += 2;
			} else {
				tokens.pop_back(); // the preceding '/' literal belongs to '**'
				tokens.push_back({ GlobTokenKind::SlashGlobStar, 0 });
				index += 2;
			}
		} else if (ch == L'*') {
			tokens.push_back({ GlobTokenKind::Star, 0 });
			++index;
		} else if (ch == L'?') {
			tokens.push_back({ GlobTokenKind::Single, 0 });
			++index;
		} else {
			tokens.push_back({ GlobTokenKind::Literal,
				static_cast<wchar_t>(::towlower(ch)) });
			++index;
		}
	}
	return tokens;
}

bool GlobMatches(std::wstring_view pattern, std::wstring_view value)
{
	if (pattern.size() > 1024 || value.size() > 32768) return false;
	const auto tokens = TokenizeGlob(pattern);
	const std::size_t count = value.size();
	std::vector<bool> previous(count + 1, false);
	std::vector<bool> current(count + 1, false);
	std::vector<bool> open(count + 1, false);
	previous[0] = true;
	for (const auto& token : tokens) {
		std::fill(current.begin(), current.end(), false);
		std::fill(open.begin(), open.end(), false);
		current[0] = previous[0] && (token.kind == GlobTokenKind::Star
			|| token.kind == GlobTokenKind::GlobStar
			|| token.kind == GlobTokenKind::GlobStarSlash
			|| token.kind == GlobTokenKind::SlashGlobStar);
		for (std::size_t index = 1; index <= count; ++index) {
			const wchar_t valueChar = static_cast<wchar_t>(::towlower(value[index - 1]));
			switch (token.kind) {
			case GlobTokenKind::Literal:
				current[index] = previous[index - 1] && token.literal == valueChar;
				break;
			case GlobTokenKind::Single:
				current[index] = previous[index - 1] && valueChar != L'/';
				break;
			case GlobTokenKind::Star:
				current[index] = previous[index]
					|| (current[index - 1] && valueChar != L'/');
				break;
			case GlobTokenKind::GlobStar:
				current[index] = previous[index] || current[index - 1];
				break;
			case GlobTokenKind::GlobStarSlash:
				// '**/' consumes zero or more complete segments: an open run may
				// swallow any characters but may only close on a '/'.
				open[index] = previous[index - 1] || open[index - 1];
				current[index] = previous[index] || (open[index - 1] && valueChar == L'/');
				break;
			case GlobTokenKind::SlashGlobStar:
				// '/**' at the end consumes nothing or a '/' followed by anything.
				open[index] = open[index - 1] || (previous[index - 1] && valueChar == L'/');
				current[index] = previous[index] || open[index];
				break;
			}
		}
		previous.swap(current);
	}
	return previous[count];
}

bool HasPathPrefix(const std::filesystem::path& root, const std::filesystem::path& candidate)
{
	auto rootIt = root.begin();
	auto candidateIt = candidate.begin();
	for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
		if (candidateIt == candidate.end() || Fold(rootIt->native()) != Fold(candidateIt->native())) return false;
	}
	return true;
}

std::optional<std::string> ReadGrammar(const ExtensionDescriptor& extension, std::wstring_view relative)
{
	if (extension.extensionPath.empty() || relative.empty()) return std::nullopt;
	std::error_code error;
	const auto root = std::filesystem::weakly_canonical(extension.extensionPath, error);
	if (error || root.empty()) return std::nullopt;
	const auto path = std::filesystem::weakly_canonical(root / std::filesystem::path(relative), error);
	if (error || !HasPathPrefix(root, path) || !std::filesystem::is_regular_file(path, error)
		|| error) return std::nullopt;
	std::ifstream input(path, std::ios::binary);
	if (!input) return std::nullopt;
	// Read once in bounded chunks; no file_size call, so a concurrent resize
	// cannot change the limit decision or the buffer size midway.
	std::string source;
	constexpr std::size_t kChunkBytes = 64U * 1024U;
	while (input && source.size() <= kMaximumGrammarBytes) {
		const std::size_t offset = source.size();
		source.resize(offset + kChunkBytes);
		input.read(source.data() + offset, static_cast<std::streamsize>(kChunkBytes));
		source.resize(offset + static_cast<std::size_t>(input.gcount()));
	}
	if (input.bad() || source.size() > kMaximumGrammarBytes) return std::nullopt;
	return source;
}

std::shared_ptr<const textmate::Grammar> LoadGrammar(
	const ExtensionDescriptor& extension, const GrammarContribution& contribution)
{
	const auto source = ReadGrammar(extension, contribution.path);
	if (!source) return {};
	const auto path = Fold(contribution.path);
	auto result = path.ends_with(L".json")
		? textmate::TextMateJsonGrammarLoader::Load(*source)
		: textmate::TextMatePlistGrammarLoader::Load(*source);
	if (!result.Succeeded() || result.grammar->scopeName != contribution.scopeName) return {};
	return std::shared_ptr<const textmate::Grammar>(std::move(result.grammar));
}

struct LanguageRegistration final {
	std::wstring extensionId;
	LanguageContribution language;
	std::wstring scopeName;
};

struct GrammarSet final {
	std::map<std::wstring, std::shared_ptr<const textmate::Grammar>, std::less<>> grammars;
	std::vector<LanguageRegistration> languages;
};

std::shared_ptr<const GrammarSet> BuildGrammarSet(const ManagementSnapshot& snapshot)
{
	auto result = std::make_shared<GrammarSet>();
	for (const auto& extension : snapshot.extensions) {
		if (!extension.installed || !extension.enabled) continue;
		for (const auto& grammar : extension.grammars) {
			auto loaded = LoadGrammar(extension, grammar);
			if (loaded) result->grammars.try_emplace(grammar.scopeName, std::move(loaded));
		}
		for (const auto& language : extension.languages) {
			const auto grammar = std::ranges::find_if(extension.grammars, [&](const auto& candidate) {
				return candidate.language == language.id && candidate.injectTo.empty()
					&& result->grammars.contains(candidate.scopeName);
			});
			if (grammar != extension.grammars.end()) {
				result->languages.push_back({ extension.id, language, grammar->scopeName });
			}
		}
	}
	std::ranges::sort(result->languages, [](const auto& left, const auto& right) {
		if (left.language.id != right.language.id) return left.language.id < right.language.id;
		return left.extensionId < right.extensionId;
	});
	return result;
}

bool FirstLineMatches(std::wstring_view pattern, std::wstring_view firstLine)
{
	if (pattern.empty() || firstLine.empty()) return false;
	std::wstring diagnostic;
	auto compiled = textmate::OnigmoPattern::Compile(textmate::EncodeUtf8(pattern), &diagnostic);
	if (!compiled) return false;
	return compiled->Search(textmate::EncodeLineForSearch(firstLine), 0).has_value();
}

int MatchScore(const LanguageContribution& language, std::wstring_view resourcePath,
	std::wstring_view firstLine)
{
	const std::filesystem::path path(resourcePath);
	const std::wstring filename = Fold(path.filename().native());
	std::optional<std::wstring> foldedSlashedPath;
	int score = 0;
	for (const auto& candidate : language.filenames) {
		if (Fold(candidate) == filename) score = (std::max)(score, 40'000 + static_cast<int>(candidate.size()));
	}
	for (const auto& candidate : language.extensions) {
		const auto extension = Fold(candidate);
		if (filename.ends_with(extension)) score = (std::max)(score, 20'000 + static_cast<int>(extension.size()));
	}
	for (const auto& candidate : language.filenamePatterns) {
		// VS Code's filepatternOnPath rule: a pattern containing '/' is matched
		// against the '/'-normalised full path, any other pattern only against
		// the basename.
		if (candidate.find(L'/') != std::wstring::npos) {
			if (!foldedSlashedPath) {
				foldedSlashedPath = Fold(resourcePath);
				std::ranges::replace(*foldedSlashedPath, L'\\', L'/');
			}
			if (GlobMatches(candidate, *foldedSlashedPath)) {
				score = (std::max)(score, 30'000 + static_cast<int>(candidate.size()));
			}
		} else if (GlobMatches(candidate, filename)) {
			score = (std::max)(score, 30'000 + static_cast<int>(candidate.size()));
		}
	}
	if (language.firstLine.size() <= 4096 && FirstLineMatches(language.firstLine, firstLine)) {
		score = (std::max)(score, 10'000);
	}
	return score;
}

class CTextMateSession final : public ISenpTextMateSession, private textmate::IExternalGrammarResolver {
public:
	CTextMateSession(std::shared_ptr<const GrammarSet> set, LanguageRegistration registration)
		: m_set(std::move(set))
		, m_registration(std::move(registration))
		, m_tokenizer(*m_set->grammars.at(m_registration.scopeName), this)
	{
	}

	std::wstring_view LanguageId() const noexcept override { return m_registration.language.id; }
	std::wstring_view ScopeName() const noexcept override { return m_registration.scopeName; }
	textmate::RuleStackHandle InitialState() const override { return m_tokenizer.InitialState(); }
	textmate::TextMateLineTokenizeResult TokenizeLine(std::wstring_view line,
		const textmate::RuleStackHandle& previousState,
		textmate::RuleStackHandle& nextState) const override
	{
		return m_tokenizer.TokenizeLine(line, previousState, nextState);
	}

private:
	const textmate::Grammar* ResolveGrammar(std::wstring_view scopeName) override
	{
		const auto found = m_set->grammars.find(scopeName);
		return found == m_set->grammars.end() ? nullptr : found->second.get();
	}

	std::shared_ptr<const GrammarSet> m_set;
	LanguageRegistration m_registration;
	textmate::TextMateTokenizer m_tokenizer;
};

class CSenpLanguageService final : public ISenpLanguageService {
public:
	explicit CSenpLanguageService(ISenpManagementService& management) : m_management(management) {}

	bool Start() override
	{
		const auto snapshot = m_management.Snapshot();
		if (snapshot.state != EManagementState::Ready
			&& snapshot.state != EManagementState::ReadyWithDiagnostics) return false;
		auto grammars = BuildGrammarSet(snapshot);
		std::lock_guard lock(m_mutex);
		m_grammars = std::move(grammars);
		if (m_revision != std::numeric_limits<std::uint64_t>::max()) ++m_revision;
		m_stopped = false;
		return true;
	}

	void Stop() noexcept override
	{
		try {
			std::lock_guard lock(m_mutex);
			m_stopped = true;
			m_grammars.reset();
			if (m_revision != std::numeric_limits<std::uint64_t>::max()) ++m_revision;
		} catch (...) {
		}
	}

	void NotifyExtensionsChanged() noexcept override
	{
		try {
			const auto snapshot = m_management.Snapshot();
			auto grammars = BuildGrammarSet(snapshot);
			std::lock_guard lock(m_mutex);
			if (m_stopped) return;
			m_grammars = std::move(grammars);
			if (m_revision != std::numeric_limits<std::uint64_t>::max()) ++m_revision;
		} catch (...) {
		}
	}

	std::uint64_t Revision() const noexcept override
	{
		try {
			std::lock_guard lock(m_mutex);
			return m_revision;
		} catch (...) {
			return 0;
		}
	}

	std::unique_ptr<ISenpTextMateSession> CreateSession(std::wstring_view resourcePath,
		std::wstring_view firstLine, std::wstring_view preferredLanguageId) override
	{
		std::shared_ptr<const GrammarSet> grammars;
		{
			std::lock_guard lock(m_mutex);
			if (m_stopped || !m_grammars) return {};
			grammars = m_grammars;
		}
		const LanguageRegistration* best = nullptr;
		int bestScore = 0;
		for (const auto& registration : grammars->languages) {
			if (!preferredLanguageId.empty() && registration.language.id != preferredLanguageId) continue;
			const int score = preferredLanguageId.empty()
				? MatchScore(registration.language, resourcePath, firstLine) : 1;
			if (score > bestScore) {
				best = &registration;
				bestScore = score;
			}
		}
		return best == nullptr ? nullptr
			: std::make_unique<CTextMateSession>(std::move(grammars), *best);
	}

private:
	ISenpManagementService& m_management;
	mutable std::mutex m_mutex;
	std::shared_ptr<const GrammarSet> m_grammars;
	std::uint64_t m_revision = 0;
	bool m_stopped = true;
};

} // namespace

std::unique_ptr<ISenpLanguageService> CreateSenpLanguageService(ISenpManagementService& management)
{
	return std::make_unique<CSenpLanguageService>(management);
}

} // namespace senp
