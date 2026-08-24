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

bool GlobMatches(std::wstring_view pattern, std::wstring_view value) noexcept
{
	if (pattern.size() > 1024 || value.size() > 32768) return false;
	std::vector<bool> previous(value.size() + 1, false);
	std::vector<bool> current(value.size() + 1, false);
	previous[0] = true;
	for (const wchar_t rawPattern : pattern) {
		std::fill(current.begin(), current.end(), false);
		const wchar_t patternChar = static_cast<wchar_t>(::towlower(rawPattern));
		if (patternChar == L'*') current[0] = previous[0];
		for (std::size_t index = 1; index <= value.size(); ++index) {
			const wchar_t valueChar = static_cast<wchar_t>(::towlower(value[index - 1]));
			if (patternChar == L'*') current[index] = previous[index] || current[index - 1];
			else if (patternChar == L'?' || patternChar == valueChar) current[index] = previous[index - 1];
		}
		previous.swap(current);
	}
	return previous[value.size()];
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
		|| error || std::filesystem::file_size(path, error) > kMaximumGrammarBytes || error) return std::nullopt;
	std::ifstream input(path, std::ios::binary);
	if (!input) return std::nullopt;
	std::string source(static_cast<std::size_t>(std::filesystem::file_size(path, error)), '\0');
	if (error || (!source.empty() && !input.read(source.data(), static_cast<std::streamsize>(source.size())))) {
		return std::nullopt;
	}
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
	const std::wstring foldedPath = Fold(resourcePath);
	int score = 0;
	for (const auto& candidate : language.filenames) {
		if (Fold(candidate) == filename) score = (std::max)(score, 40'000 + static_cast<int>(candidate.size()));
	}
	for (const auto& candidate : language.extensions) {
		const auto extension = Fold(candidate);
		if (filename.ends_with(extension)) score = (std::max)(score, 30'000 + static_cast<int>(extension.size()));
	}
	for (const auto& candidate : language.filenamePatterns) {
		if (GlobMatches(candidate, filename) || GlobMatches(candidate, foldedPath)) {
			score = (std::max)(score, 20'000 + static_cast<int>(candidate.size()));
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
