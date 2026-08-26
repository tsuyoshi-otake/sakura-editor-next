/*! @file @brief Bounded native syntax highlighting for Markdown fenced code. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "markdown/MarkdownCodeHighlighter.h"
#include "util/CpuDispatch.h"
#include "util/Utf16BenchmarkTelemetry.h"

#include <algorithm>
#include <array>
#include <limits>
#include <span>

namespace markdown {
namespace {

enum class LanguageId : std::uint8_t {
	Plain,
	C,
	Cpp,
	Cs,
	Js,
	Ts,
	Jsx,
	Json,
	Python,
	Sh,
	PowerShell,
	Css,
	Html,
	Markdown,
};

enum class NormalAction : std::uint8_t {
	Skip,
	Identifier,
	Number,
	Slash,
	LineComment,
	SingleString,
	DoubleString,
	BacktickString,
	Variable,
	Preprocessor,
	Operator,
	Punctuation,
	CsharpAt,
	CsharpDollar,
	JsxTag,
	PowershellLess,
	PowershellAt,
	CssHash,
	AtKeyword,
};

using ActionTable = std::array<NormalAction, 128>;

constexpr ActionTable MakeBaseActionTable()
{
	ActionTable table{};
	for (auto& action : table) {
		action = NormalAction::Skip;
	}
	for (unsigned char value = 'a'; value <= 'z'; ++value) {
		table[value] = NormalAction::Identifier;
	}
	for (unsigned char value = 'A'; value <= 'Z'; ++value) {
		table[value] = NormalAction::Identifier;
	}
	table[static_cast<unsigned char>('_')] = NormalAction::Identifier;
	for (unsigned char value = '0'; value <= '9'; ++value) {
		table[value] = NormalAction::Number;
	}
	table[static_cast<unsigned char>('\'')] = NormalAction::SingleString;
	table[static_cast<unsigned char>('"')] = NormalAction::DoubleString;
	for (const char value : std::string_view{ "+-*%=^&|!~?:.<>\\" }) {
		table[static_cast<unsigned char>(value)] = NormalAction::Operator;
	}
	for (const char value : std::string_view{ "(){}[],;" }) {
		table[static_cast<unsigned char>(value)] = NormalAction::Punctuation;
	}
	table[static_cast<unsigned char>('/')] = NormalAction::Operator;
	table[static_cast<unsigned char>('#')] = NormalAction::Operator;
	table[static_cast<unsigned char>('`')] = NormalAction::Operator;
	table[static_cast<unsigned char>('$')] = NormalAction::Operator;
	table[static_cast<unsigned char>('@')] = NormalAction::Operator;
	return table;
}

constexpr ActionTable MakeCLikeActionTable()
{
	auto table = MakeBaseActionTable();
	table[static_cast<unsigned char>('/')] = NormalAction::Slash;
	table[static_cast<unsigned char>('#')] = NormalAction::Preprocessor;
	return table;
}

constexpr ActionTable MakeCsharpActionTable()
{
	auto table = MakeCLikeActionTable();
	table[static_cast<unsigned char>('@')] = NormalAction::CsharpAt;
	table[static_cast<unsigned char>('$')] = NormalAction::CsharpDollar;
	return table;
}

constexpr ActionTable MakeJavascriptActionTable(bool jsx)
{
	auto table = MakeBaseActionTable();
	table[static_cast<unsigned char>('/')] = NormalAction::Slash;
	table[static_cast<unsigned char>('`')] = NormalAction::BacktickString;
	table[static_cast<unsigned char>('$')] = NormalAction::Identifier;
	if (jsx) {
		table[static_cast<unsigned char>('<')] = NormalAction::JsxTag;
	}
	return table;
}

constexpr ActionTable MakeHashCommentActionTable(bool shell)
{
	auto table = MakeBaseActionTable();
	table[static_cast<unsigned char>('#')] = NormalAction::LineComment;
	if (shell) {
		table[static_cast<unsigned char>('$')] = NormalAction::Variable;
		table[static_cast<unsigned char>('`')] = NormalAction::BacktickString;
	}
	return table;
}

constexpr ActionTable MakePowershellActionTable()
{
	auto table = MakeHashCommentActionTable(true);
	table[static_cast<unsigned char>('`')] = NormalAction::Operator;
	table[static_cast<unsigned char>('<')] = NormalAction::PowershellLess;
	table[static_cast<unsigned char>('@')] = NormalAction::PowershellAt;
	return table;
}

constexpr ActionTable MakeCssActionTable()
{
	auto table = MakeBaseActionTable();
	table[static_cast<unsigned char>('/')] = NormalAction::Slash;
	table[static_cast<unsigned char>('#')] = NormalAction::CssHash;
	table[static_cast<unsigned char>('@')] = NormalAction::AtKeyword;
	return table;
}

constexpr ActionTable kCLikeActions = MakeCLikeActionTable();
constexpr ActionTable kCsharpActions = MakeCsharpActionTable();
constexpr ActionTable kJavascriptActions = MakeJavascriptActionTable(false);
constexpr ActionTable kJsxActions = MakeJavascriptActionTable(true);
constexpr ActionTable kJsonActions = MakeJavascriptActionTable(false);
constexpr ActionTable kPythonActions = MakeHashCommentActionTable(false);
constexpr ActionTable kShellActions = MakeHashCommentActionTable(true);
constexpr ActionTable kPowershellActions = MakePowershellActionTable();
constexpr ActionTable kCssActions = MakeCssActionTable();

template <typename... Words>
constexpr auto MakeWords(Words... words)
{
	return std::array<std::wstring_view, sizeof...(Words)>{ std::wstring_view{words}... };
}

constexpr auto kCKeywords = MakeWords(
	L"auto", L"break", L"case", L"const", L"continue", L"default",
	L"do", L"else", L"enum", L"extern", L"for", L"goto", L"if", L"inline",
	L"register", L"restrict", L"return", L"sizeof", L"static", L"struct", L"switch",
	L"typedef", L"union", L"volatile", L"while");
constexpr auto kCtypes = MakeWords(
	L"bool", L"char", L"double", L"float", L"int", L"long",
	L"short", L"signed", L"unsigned", L"void");
constexpr auto kCppKeywords = MakeWords(
	L"alignas", L"alignof", L"asm", L"auto", L"break", L"case",
	L"catch", L"class", L"concept", L"const", L"constexpr", L"continue", L"default",
	L"delete", L"do", L"else", L"enum", L"explicit", L"export", L"extern", L"false",
	L"for", L"friend", L"if", L"inline", L"mutable", L"namespace", L"new", L"noexcept",
	L"nullptr", L"operator", L"private", L"protected", L"public", L"return", L"sizeof",
	L"static", L"struct", L"switch", L"template", L"this", L"throw", L"true", L"try",
	L"typedef", L"typename", L"union", L"using", L"virtual", L"volatile", L"while");
constexpr auto kCppTypes = MakeWords(
	L"bool", L"char", L"double", L"float", L"int", L"long",
	L"short", L"signed", L"unsigned", L"void", L"wchar_t");
constexpr auto kCsharpKeywords = MakeWords(
	L"abstract", L"as", L"async", L"await", L"base", L"break",
	L"case", L"catch", L"class", L"const", L"continue", L"default", L"delegate", L"do",
	L"else", L"enum", L"event", L"false", L"finally", L"for", L"foreach", L"if", L"in",
	L"interface", L"internal", L"is", L"lock", L"namespace", L"new", L"null", L"private",
	L"protected", L"public", L"readonly", L"ref", L"return", L"sealed", L"static",
	L"struct", L"switch", L"this", L"throw", L"true", L"try", L"using", L"virtual",
	L"while");
constexpr auto kCsharpTypes = MakeWords(
	L"bool", L"byte", L"char", L"decimal", L"double", L"float",
	L"int", L"long", L"object", L"sbyte", L"short", L"string", L"uint", L"ulong",
	L"ushort", L"void");
constexpr auto kJavascriptKeywords = MakeWords(
	L"async", L"await", L"break", L"case", L"catch", L"class", L"const",
	L"continue", L"debugger", L"default", L"delete", L"do", L"else", L"export", L"extends",
	L"false", L"finally", L"for", L"function", L"if", L"import", L"in", L"instanceof",
	L"let", L"new", L"null", L"return", L"static", L"super", L"switch", L"this", L"throw",
	L"true", L"try", L"typeof", L"undefined", L"var", L"void", L"while", L"with", L"yield");
constexpr auto kTypescriptKeywords = MakeWords(
	L"abstract", L"as", L"asserts", L"declare", L"enum", L"implements",
	L"interface", L"keyof", L"namespace", L"private", L"protected", L"public", L"readonly",
	L"type");
constexpr auto kTypescriptTypes = MakeWords(
	L"any", L"boolean", L"never", L"number", L"object", L"string",
	L"symbol", L"unknown", L"void");
constexpr auto kJavascriptLiterals = MakeWords(
	L"false", L"null", L"true", L"undefined");
constexpr auto kJsonLiterals = MakeWords(
	L"false", L"null", L"true");
constexpr auto kPythonKeywords = MakeWords(
	L"and", L"as", L"assert", L"async", L"await", L"break", L"class",
	L"continue", L"def", L"del", L"elif", L"else", L"except", L"finally", L"for", L"from",
	L"global", L"if", L"import", L"in", L"is", L"lambda", L"nonlocal", L"not", L"or",
	L"pass", L"raise", L"return", L"try", L"while", L"with", L"yield");
constexpr auto kPythonLiterals = MakeWords(
	L"False", L"None", L"True");
constexpr auto kShellKeywords = MakeWords(
	L"case", L"do", L"done", L"elif", L"else", L"esac", L"fi", L"for",
	L"function", L"if", L"in", L"select", L"then", L"time", L"until", L"while");
constexpr auto kPowershellKeywords = MakeWords(
	L"begin", L"break", L"catch", L"class", L"continue", L"data", L"do",
	L"dynamicparam", L"else", L"elseif", L"end", L"enum", L"exit", L"filter", L"finally",
	L"for", L"foreach", L"from", L"function", L"if", L"in", L"param", L"process", L"return",
	L"switch", L"throw", L"trap", L"try", L"until", L"using", L"var", L"while", L"workflow");
constexpr std::array<std::wstring_view, 0> kNoWords{};

template <std::size_t Size>
constexpr bool IsSorted(const std::array<std::wstring_view, Size>& words)
{
	for (std::size_t index = 1; index < Size; ++index) {
		if (words[index - 1].compare(words[index]) > 0) {
			return false;
		}
	}
	return true;
}

static_assert(IsSorted(kCKeywords));
static_assert(IsSorted(kCtypes));
static_assert(IsSorted(kCppKeywords));
static_assert(IsSorted(kCppTypes));
static_assert(IsSorted(kCsharpKeywords));
static_assert(IsSorted(kCsharpTypes));
static_assert(IsSorted(kJavascriptKeywords));
static_assert(IsSorted(kTypescriptKeywords));
static_assert(IsSorted(kTypescriptTypes));
static_assert(IsSorted(kJavascriptLiterals));
static_assert(IsSorted(kJsonLiterals));
static_assert(IsSorted(kPythonKeywords));
static_assert(IsSorted(kPythonLiterals));
static_assert(IsSorted(kShellKeywords));
static_assert(IsSorted(kPowershellKeywords));

struct LanguageSpec {
	LanguageId id = LanguageId::Plain;
	const ActionTable* actions = nullptr;
	std::span<const std::wstring_view> keywords;
	std::span<const std::wstring_view> additionalKeywords;
	std::span<const std::wstring_view> types;
	std::span<const std::wstring_view> literals;
	bool caseInsensitiveKeywords = false;
	bool pythonTripleStrings = false;
	bool shellSingleQuote = false;
	bool allowDollarInIdentifier = false;
};

LanguageSpec GetLanguageSpec(LanguageId id)
{
	switch (id) {
	case LanguageId::C:
		return { id, &kCLikeActions, kCKeywords, kNoWords, kCtypes, kNoWords };
	case LanguageId::Cpp:
		return { id, &kCLikeActions, kCppKeywords, kNoWords, kCppTypes, kNoWords };
	case LanguageId::Cs:
		return { id, &kCsharpActions, kCsharpKeywords, kNoWords, kCsharpTypes, kNoWords };
	case LanguageId::Js:
		return { id, &kJavascriptActions, kJavascriptKeywords, kNoWords, kNoWords,
			kJavascriptLiterals, false, false, false, true };
	case LanguageId::Ts:
		return { id, &kJavascriptActions, kJavascriptKeywords, kTypescriptKeywords,
			kTypescriptTypes, kJavascriptLiterals, false, false, false, true };
	case LanguageId::Jsx:
		return { id, &kJsxActions, kJavascriptKeywords, kTypescriptKeywords,
			kTypescriptTypes, kJavascriptLiterals, false, false, false, true };
	case LanguageId::Json:
		return { id, &kJsonActions, kNoWords, kNoWords, kNoWords, kJsonLiterals };
	case LanguageId::Python:
		return { id, &kPythonActions, kPythonKeywords, kNoWords, kNoWords,
			kPythonLiterals, false, true };
	case LanguageId::Sh:
		return { id, &kShellActions, kShellKeywords, kNoWords, kNoWords, kNoWords,
			false, false, true };
	case LanguageId::PowerShell:
		return { id, &kPowershellActions, kPowershellKeywords, kNoWords, kNoWords,
			kNoWords, true };
	case LanguageId::Css:
		return { id, &kCssActions, kNoWords, kNoWords, kNoWords, kNoWords };
	default:
		return {};
	}
}

[[nodiscard]] wchar_t FoldAscii(wchar_t value) noexcept
{
	return value >= L'A' && value <= L'Z' ? value + (L'a' - L'A') : value;
}

int CompareWords(std::wstring_view left, std::wstring_view right, bool foldAscii) noexcept
{
	const std::size_t commonLength = std::min(left.size(), right.size());
	for (std::size_t index = 0; index < commonLength; ++index) {
		const wchar_t leftValue = foldAscii ? FoldAscii(left[index]) : left[index];
		const wchar_t rightValue = foldAscii ? FoldAscii(right[index]) : right[index];
		if (leftValue < rightValue) {
			return -1;
		}
		if (leftValue > rightValue) {
			return 1;
		}
	}
	return left.size() < right.size() ? -1 : left.size() > right.size() ? 1 : 0;
}

bool ContainsWord(
	std::span<const std::wstring_view> words,
	std::wstring_view candidate,
	bool foldAscii) noexcept
{
	const auto found = std::lower_bound(words.begin(), words.end(), candidate,
		[foldAscii](std::wstring_view entry, std::wstring_view value) {
			return CompareWords(entry, value, foldAscii) < 0;
		});
	return found != words.end() && CompareWords(*found, candidate, foldAscii) == 0;
}

LanguageId GetLanguageId(std::wstring_view language) noexcept
{
	if (language == L"c") return LanguageId::C;
	if (language == L"cpp") return LanguageId::Cpp;
	if (language == L"cs") return LanguageId::Cs;
	if (language == L"js") return LanguageId::Js;
	if (language == L"ts") return LanguageId::Ts;
	if (language == L"jsx") return LanguageId::Jsx;
	if (language == L"json") return LanguageId::Json;
	if (language == L"python") return LanguageId::Python;
	if (language == L"sh") return LanguageId::Sh;
	if (language == L"powershell") return LanguageId::PowerShell;
	if (language == L"css") return LanguageId::Css;
	if (language == L"html") return LanguageId::Html;
	if (language == L"markdown") return LanguageId::Markdown;
	return LanguageId::Plain;
}

class LexerContext final {
public:
	LexerContext(
		std::wstring language,
		std::wstring_view source,
		std::size_t maximumTokens)
		: m_source(source)
		, m_maximumTokens(maximumTokens)
		, m_dispatch(CpuDispatch::Get())
	{
		m_result.language = std::move(language);
		m_result.tokens.reserve(std::min(maximumTokens, std::size_t{256}));
	}

	[[nodiscard]] std::wstring_view Source() const noexcept { return m_source; }
	[[nodiscard]] std::size_t Size() const noexcept { return m_source.size(); }
	[[nodiscard]] std::size_t Position() const noexcept { return m_position; }
	void SetPosition(std::size_t position) noexcept { m_position = std::min(position, Size()); }
	void Advance(std::size_t length = 1) noexcept
	{
		m_position += std::min(length, Size() - m_position);
	}

	[[nodiscard]] wchar_t At(std::size_t position) noexcept
	{
		RecordWork();
		return position < Size() ? m_source[position] : L'\0';
	}

	[[nodiscard]] bool StartsWith(std::size_t position, std::wstring_view value) noexcept
	{
		if (value.size() > Size() - std::min(position, Size())) {
			RecordWork();
			return false;
		}
		for (std::size_t index = 0; index < value.size(); ++index) {
			if (At(position + index) != value[index]) {
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] std::size_t FindCrOrLf(std::size_t position) noexcept
	{
		if (position >= Size()) {
			return Size();
		}
		const std::size_t length = Size() - position;
		const std::size_t offset = m_dispatch.findCrOrLfUtf16(
			m_source.data() + position, length);
		SAKURA_UTF16_BENCHMARK_RECORD(
			"crlf", length, offset, m_source.data() + position,
			m_dispatch.utf16CrOrLfIsa, "simd");
		RecordWork(offset + (offset < length ? 1 : 0));
		return position + offset;
	}

	[[nodiscard]] static bool IsMarkdownInlineSpecial(wchar_t value) noexcept
	{
		switch (value) {
		case L'\\':
		case L'`':
		case L'!':
		case L'[':
		case L'*':
		case L'_':
		case L'~':
		case L'<':
		case L'&':
		case L'$':
			return true;
		default:
			return false;
		}
	}

	[[nodiscard]] std::size_t FindMarkdownSpecial(
		std::size_t position,
		std::size_t end) noexcept
	{
		if (position >= end || position >= Size()) {
			return std::min(end, Size());
		}
		const std::size_t length = std::min(end, Size()) - position;
		std::size_t offset = 0;
		// Token-bounded scans are usually short; below the per-ISA minimum the
		// local scalar loop beats an indirect call into the dispatched scanner.
		if (length >= m_dispatch.utf16ScanPolicy.markdownInlineSpecialMinimumLength) {
			offset = m_dispatch.findMarkdownInlineSpecialUtf16(
				m_source.data() + position, length);
		} else {
			while (offset < length
				&& !IsMarkdownInlineSpecial(m_source[position + offset])) {
				++offset;
			}
		}
		SAKURA_UTF16_BENCHMARK_RECORD(
			"markdown", length, offset, m_source.data() + position,
			m_dispatch.utf16MarkdownIsa,
			length >= m_dispatch.utf16ScanPolicy.markdownInlineSpecialMinimumLength
				? "simd" : "scalar");
		RecordWork(offset + (offset < length ? 1 : 0));
		return position + offset;
	}

	void Emit(CodeTokenKind kind, std::size_t start, std::size_t end)
	{
		end = std::min(end, Size());
		if (start >= end) {
			return;
		}
		if (!m_result.tokens.empty()) {
			auto& previous = m_result.tokens.back();
			const std::size_t previousEnd = previous.start + previous.length;
			if (start < previousEnd) {
				start = previousEnd;
				if (start >= end) {
					return;
				}
			}
			if (previous.kind == kind && previousEnd == start) {
				previous.length += end - start;
				return;
			}
		}
		if (m_result.tokens.size() >= m_maximumTokens) {
			m_tokenLimitReached = true;
			return;
		}
		m_result.tokens.push_back({ kind, start, end - start });
	}

	void MarkTerminal(CodeHighlightTerminalState terminalState) noexcept
	{
		if (m_terminalState == CodeHighlightTerminalState::Completed) {
			m_terminalState = terminalState;
		}
	}

	void RecordWork(std::size_t units = 1) noexcept
	{
		const std::size_t available =
			std::numeric_limits<std::size_t>::max() - m_result.workUnits;
		m_result.workUnits += std::min(units, available);
	}

	CodeHighlightResult Finish()
	{
		m_result.scannedLength = m_position;
		m_result.terminalState = m_tokenLimitReached
			? CodeHighlightTerminalState::TokenLimitReached
			: m_terminalState;
		return std::move(m_result);
	}

private:
	std::wstring_view m_source;
	std::size_t m_position = 0;
	std::size_t m_maximumTokens = 0;
	const CpuDispatch::Dispatch& m_dispatch;
	CodeHighlightResult m_result;
	CodeHighlightTerminalState m_terminalState = CodeHighlightTerminalState::Completed;
	bool m_tokenLimitReached = false;
};

[[nodiscard]] bool IsAsciiDigit(wchar_t value) noexcept
{
	return value >= L'0' && value <= L'9';
}

[[nodiscard]] bool IsAsciiHexDigit(wchar_t value) noexcept
{
	return IsAsciiDigit(value)
		|| (value >= L'a' && value <= L'f')
		|| (value >= L'A' && value <= L'F');
}

[[nodiscard]] bool IsIdentifierStart(wchar_t value, bool allowDollar) noexcept
{
	return (value >= L'a' && value <= L'z')
		|| (value >= L'A' && value <= L'Z')
		|| value == L'_'
		|| (allowDollar && value == L'$')
		|| value >= 0x80;
}

[[nodiscard]] bool IsIdentifierContinue(wchar_t value, bool allowDollar) noexcept
{
	return IsIdentifierStart(value, allowDollar) || IsAsciiDigit(value);
}

[[nodiscard]] NormalAction GetAction(const LanguageSpec& spec, wchar_t value) noexcept
{
	if (static_cast<std::uint32_t>(value) < 128) {
		return (*spec.actions)[static_cast<std::size_t>(value)];
	}
	return NormalAction::Identifier;
}

void ScanLineComment(LexerContext& context, std::size_t markerLength)
{
	const std::size_t start = context.Position();
	context.Advance(markerLength);
	const std::size_t end = context.FindCrOrLf(context.Position());
	context.SetPosition(end);
	context.Emit(CodeTokenKind::Comment, start, end);
}

void ScanBlockComment(
	LexerContext& context,
	std::wstring_view opening,
	std::wstring_view closing)
{
	const std::size_t start = context.Position();
	context.Advance(opening.size());
	while (context.Position() < context.Size()) {
		if (context.StartsWith(context.Position(), closing)) {
			context.Advance(closing.size());
			context.Emit(CodeTokenKind::Comment, start, context.Position());
			return;
		}
		context.Advance();
	}
	context.Emit(CodeTokenKind::Comment, start, context.Position());
	context.MarkTerminal(CodeHighlightTerminalState::UnterminatedComment);
}

void ScanQuotedString(
	LexerContext& context,
	std::size_t start,
	std::size_t quotePosition,
	wchar_t quote,
	wchar_t escapeCharacter,
	bool doubledQuote,
	bool tripleQuote,
	bool crossesLines)
{
	const std::size_t openingLength = tripleQuote ? 3 : 1;
	context.SetPosition(quotePosition + openingLength);
	while (context.Position() < context.Size()) {
		const wchar_t value = context.At(context.Position());
		if (!crossesLines && !tripleQuote && (value == L'\r' || value == L'\n')) {
			context.Emit(CodeTokenKind::String, start, context.Position());
			context.MarkTerminal(CodeHighlightTerminalState::UnterminatedString);
			return;
		}
		if (escapeCharacter != L'\0' && value == escapeCharacter) {
			context.Advance();
			if (context.Position() < context.Size()) {
				context.Advance();
			}
			continue;
		}
		if (value == quote) {
			if (tripleQuote) {
				if (context.At(context.Position() + 1) == quote
					&& context.At(context.Position() + 2) == quote) {
					context.Advance(3);
					context.Emit(CodeTokenKind::String, start, context.Position());
					return;
				}
			} else if (doubledQuote && context.At(context.Position() + 1) == quote) {
				context.Advance(2);
				continue;
			} else {
				context.Advance();
				context.Emit(CodeTokenKind::String, start, context.Position());
				return;
			}
		}
		context.Advance();
	}
	context.Emit(CodeTokenKind::String, start, context.Position());
	context.MarkTerminal(CodeHighlightTerminalState::UnterminatedString);
}

void ScanString(LexerContext& context, const LanguageSpec& spec, wchar_t quote)
{
	const std::size_t start = context.Position();
	const bool tripleQuote = spec.pythonTripleStrings
		&& context.At(start + 1) == quote
		&& context.At(start + 2) == quote;
	const bool shellSingleQuote = spec.shellSingleQuote && quote == L'\'';
	const wchar_t escapeCharacter = shellSingleQuote ? L'\0' : L'\\';
	ScanQuotedString(
		context,
		start,
		start,
		quote,
		escapeCharacter,
		false,
		tripleQuote,
		spec.shellSingleQuote);
}

void ScanNumber(LexerContext& context)
{
	const std::size_t start = context.Position();
	if (context.At(start) == L'0'
		&& (context.At(start + 1) == L'x' || context.At(start + 1) == L'X')) {
		context.Advance(2);
		while (IsAsciiHexDigit(context.At(context.Position()))
			|| context.At(context.Position()) == L'_') {
			context.Advance();
		}
		context.Emit(CodeTokenKind::Number, start, context.Position());
		return;
	}

	while (IsAsciiDigit(context.At(context.Position()))
		|| context.At(context.Position()) == L'_') {
		context.Advance();
	}
	if (context.At(context.Position()) == L'.'
		&& IsAsciiDigit(context.At(context.Position() + 1))) {
		context.Advance();
		while (IsAsciiDigit(context.At(context.Position()))
			|| context.At(context.Position()) == L'_') {
			context.Advance();
		}
	}
	if (context.At(context.Position()) == L'e' || context.At(context.Position()) == L'E') {
		const std::size_t exponent = context.Position();
		context.Advance();
		if (context.At(context.Position()) == L'+' || context.At(context.Position()) == L'-') {
			context.Advance();
		}
		if (!IsAsciiDigit(context.At(context.Position()))) {
			context.SetPosition(exponent);
		} else {
			while (IsAsciiDigit(context.At(context.Position()))
				|| context.At(context.Position()) == L'_') {
				context.Advance();
			}
		}
	}
	context.Emit(CodeTokenKind::Number, start, context.Position());
}

void ScanIdentifier(LexerContext& context, const LanguageSpec& spec)
{
	const std::size_t start = context.Position();
	context.Advance();
	while (IsIdentifierContinue(
		context.At(context.Position()), spec.allowDollarInIdentifier)) {
		context.Advance();
	}
	const std::wstring_view word =
		context.Source().substr(start, context.Position() - start);
	if (ContainsWord(spec.literals, word, spec.caseInsensitiveKeywords)) {
		context.Emit(CodeTokenKind::Literal, start, context.Position());
	} else if (ContainsWord(spec.types, word, spec.caseInsensitiveKeywords)) {
		context.Emit(CodeTokenKind::Type, start, context.Position());
	} else if (ContainsWord(spec.keywords, word, spec.caseInsensitiveKeywords)
		|| ContainsWord(spec.additionalKeywords, word, spec.caseInsensitiveKeywords)) {
		context.Emit(CodeTokenKind::Keyword, start, context.Position());
	} else if (spec.id == LanguageId::Css) {
		std::size_t lookahead = context.Position();
		while (context.At(lookahead) == L' ' || context.At(lookahead) == L'\t') {
			++lookahead;
		}
		if (context.At(lookahead) == L':') {
			context.Emit(CodeTokenKind::Attribute, start, context.Position());
		}
	}
}

void ScanVariable(LexerContext& context)
{
	const std::size_t start = context.Position();
	context.Advance();
	if (context.At(context.Position()) == L'{') {
		context.Advance();
		while (context.Position() < context.Size()
			&& context.At(context.Position()) != L'}') {
			context.Advance();
		}
		if (context.Position() < context.Size()) {
			context.Advance();
		} else {
			context.MarkTerminal(CodeHighlightTerminalState::UnterminatedConstruct);
		}
	} else if (IsIdentifierContinue(context.At(context.Position()), true)) {
		while (IsIdentifierContinue(context.At(context.Position()), true)) {
			context.Advance();
		}
	} else if (context.Position() < context.Size()) {
		context.Advance();
	}
	context.Emit(CodeTokenKind::Variable, start, context.Position());
}

void ScanPreprocessor(LexerContext& context)
{
	const std::size_t start = context.Position();
	const std::size_t end = context.FindCrOrLf(start);
	context.SetPosition(end);
	context.Emit(CodeTokenKind::Preprocessor, start, end);
}

void ScanAtKeyword(LexerContext& context)
{
	const std::size_t start = context.Position();
	context.Advance();
	while (IsIdentifierContinue(context.At(context.Position()), false)
		|| context.At(context.Position()) == L'-') {
		context.Advance();
	}
	context.Emit(CodeTokenKind::Keyword, start, context.Position());
}

void ScanOperator(LexerContext& context, const LanguageSpec& spec)
{
	const std::size_t start = context.Position();
	context.Advance();
	while (context.Position() < context.Size()
		&& GetAction(spec, context.At(context.Position())) == NormalAction::Operator) {
		context.Advance();
	}
	context.Emit(CodeTokenKind::Operator, start, context.Position());
}

[[nodiscard]] bool IsTagNameCharacter(wchar_t value) noexcept
{
	return IsIdentifierContinue(value, false)
		|| value == L'-' || value == L':' || value == L'.';
}

void ScanTagAttributeString(LexerContext& context, wchar_t quote)
{
	const std::size_t start = context.Position();
	context.Advance();
	while (context.Position() < context.Size()) {
		if (context.At(context.Position()) == quote) {
			context.Advance();
			context.Emit(CodeTokenKind::String, start, context.Position());
			return;
		}
		context.Advance();
	}
	context.Emit(CodeTokenKind::String, start, context.Position());
	context.MarkTerminal(CodeHighlightTerminalState::UnterminatedString);
}

void ScanJsxExpression(LexerContext& context)
{
	const std::size_t start = context.Position();
	std::size_t depth = 0;
	while (context.Position() < context.Size()) {
		const wchar_t value = context.At(context.Position());
		context.Advance();
		if (value == L'{') {
			++depth;
		} else if (value == L'}') {
			if (--depth == 0) {
				context.Emit(CodeTokenKind::Code, start, context.Position());
				return;
			}
		}
	}
	context.Emit(CodeTokenKind::Code, start, context.Position());
	context.MarkTerminal(CodeHighlightTerminalState::UnterminatedConstruct);
}

bool ScanHtmlLikeTag(LexerContext& context, bool jsx)
{
	const std::size_t start = context.Position();
	if (context.At(start) != L'<') {
		return false;
	}
	std::size_t cursor = start + 1;
	if (context.At(cursor) == L'/') {
		++cursor;
	}
	if (!IsTagNameCharacter(context.At(cursor))) {
		return false;
	}
	context.Emit(CodeTokenKind::Punctuation, start, cursor);
	const std::size_t tagStart = cursor;
	while (IsTagNameCharacter(context.At(cursor))) {
		++cursor;
	}
	context.Emit(CodeTokenKind::Tag, tagStart, cursor);
	context.SetPosition(cursor);

	while (context.Position() < context.Size()) {
		const wchar_t value = context.At(context.Position());
		if (value == L'>') {
			const std::size_t punctuationStart = context.Position();
			context.Advance();
			context.Emit(CodeTokenKind::Punctuation, punctuationStart, context.Position());
			return true;
		}
		if (value == L'/' && context.At(context.Position() + 1) == L'>') {
			const std::size_t punctuationStart = context.Position();
			context.Advance(2);
			context.Emit(CodeTokenKind::Punctuation, punctuationStart, context.Position());
			return true;
		}
		if (jsx && value == L'{') {
			ScanJsxExpression(context);
			continue;
		}
		if (value == L'\'' || value == L'"') {
			ScanTagAttributeString(context, value);
			continue;
		}
		if (IsTagNameCharacter(value)) {
			const std::size_t attributeStart = context.Position();
			context.Advance();
			while (IsTagNameCharacter(context.At(context.Position()))) {
				context.Advance();
			}
			context.Emit(CodeTokenKind::Attribute, attributeStart, context.Position());
			continue;
		}
		if (value == L'=') {
			const std::size_t operatorStart = context.Position();
			context.Advance();
			context.Emit(CodeTokenKind::Operator, operatorStart, context.Position());
			continue;
		}
		context.Advance();
	}
	context.MarkTerminal(CodeHighlightTerminalState::UnterminatedConstruct);
	return true;
}

void ScanCsharpAt(LexerContext& context)
{
	const std::size_t start = context.Position();
	if (context.At(start + 1) == L'"') {
		ScanQuotedString(context, start, start + 1, L'"', L'\0', true, false, true);
		return;
	}
	context.Advance();
	context.Emit(CodeTokenKind::Operator, start, context.Position());
}

void ScanCsharpDollar(LexerContext& context)
{
	const std::size_t start = context.Position();
	if (context.At(start + 1) == L'"') {
		ScanQuotedString(context, start, start + 1, L'"', L'\\', false, false, false);
		return;
	}
	context.Advance();
	context.Emit(CodeTokenKind::Operator, start, context.Position());
}

void ScanPowershellAt(LexerContext& context)
{
	const std::size_t start = context.Position();
	const wchar_t quote = context.At(start + 1);
	if ((quote == L'\'' || quote == L'"')
		&& (context.At(start + 2) == L'\r' || context.At(start + 2) == L'\n')) {
		context.Advance(2);
		while (context.Position() < context.Size()) {
			if (context.At(context.Position()) == quote
				&& context.At(context.Position() + 1) == L'@') {
				context.Advance(2);
				context.Emit(CodeTokenKind::String, start, context.Position());
				return;
			}
			context.Advance();
		}
		context.Emit(CodeTokenKind::String, start, context.Position());
		context.MarkTerminal(CodeHighlightTerminalState::UnterminatedString);
		return;
	}
	context.Advance();
	context.Emit(CodeTokenKind::Operator, start, context.Position());
}

void ScanCssHash(LexerContext& context)
{
	const std::size_t start = context.Position();
	if (!IsAsciiHexDigit(context.At(start + 1))) {
		context.Advance();
		context.Emit(CodeTokenKind::Operator, start, context.Position());
		return;
	}
	context.Advance();
	while (IsAsciiHexDigit(context.At(context.Position()))) {
		context.Advance();
	}
	context.Emit(CodeTokenKind::Literal, start, context.Position());
}

void LexGeneric(LexerContext& context, const LanguageSpec& spec)
{
	while (context.Position() < context.Size()) {
		const wchar_t value = context.At(context.Position());
		switch (GetAction(spec, value)) {
		case NormalAction::Identifier:
			ScanIdentifier(context, spec);
			break;
		case NormalAction::Number:
			ScanNumber(context);
			break;
		case NormalAction::Slash:
			if (context.At(context.Position() + 1) == L'/') {
				ScanLineComment(context, 2);
			} else if (context.At(context.Position() + 1) == L'*') {
				ScanBlockComment(context, L"/*", L"*/");
			} else {
				ScanOperator(context, spec);
			}
			break;
		case NormalAction::LineComment:
			ScanLineComment(context, 1);
			break;
		case NormalAction::SingleString:
			ScanString(context, spec, L'\'');
			break;
		case NormalAction::DoubleString:
			ScanString(context, spec, L'"');
			break;
		case NormalAction::BacktickString:
			ScanQuotedString(context, context.Position(), context.Position(), L'`', L'\\',
				false, false, spec.id == LanguageId::Sh);
			break;
		case NormalAction::Variable:
			ScanVariable(context);
			break;
		case NormalAction::Preprocessor:
			ScanPreprocessor(context);
			break;
		case NormalAction::Operator:
			ScanOperator(context, spec);
			break;
		case NormalAction::Punctuation: {
			const std::size_t start = context.Position();
			context.Advance();
			context.Emit(CodeTokenKind::Punctuation, start, context.Position());
			break;
		}
		case NormalAction::CsharpAt:
			ScanCsharpAt(context);
			break;
		case NormalAction::CsharpDollar:
			ScanCsharpDollar(context);
			break;
		case NormalAction::JsxTag:
			if (!ScanHtmlLikeTag(context, true)) {
				ScanOperator(context, spec);
			}
			break;
		case NormalAction::PowershellLess:
			if (context.StartsWith(context.Position(), L"<#")) {
				ScanBlockComment(context, L"<#", L"#>");
			} else {
				ScanOperator(context, spec);
			}
			break;
		case NormalAction::PowershellAt:
			ScanPowershellAt(context);
			break;
		case NormalAction::CssHash:
			ScanCssHash(context);
			break;
		case NormalAction::AtKeyword:
			ScanAtKeyword(context);
			break;
		default:
			context.Advance();
			break;
		}
	}
}

void ScanHtmlEntity(LexerContext& context, std::size_t end)
{
	const std::size_t start = context.Position();
	context.Advance();
	while (context.Position() < end) {
		const wchar_t value = context.At(context.Position());
		context.Advance();
		if (value == L';') {
			context.Emit(CodeTokenKind::Literal, start, context.Position());
			return;
		}
		if (value == L' ' || value == L'\t' || value == L'\r' || value == L'\n'
			|| value == L'<' || value == L'&') {
			return;
		}
	}
}

void LexHtml(LexerContext& context)
{
	while (context.Position() < context.Size()) {
		const std::size_t special =
			context.FindMarkdownSpecial(context.Position(), context.Size());
		context.SetPosition(special);
		if (context.Position() >= context.Size()) {
			break;
		}
		const wchar_t value = context.At(context.Position());
		if (value == L'<' && context.StartsWith(context.Position(), L"<!--")) {
			ScanBlockComment(context, L"<!--", L"-->");
		} else if (value == L'<' && context.StartsWith(context.Position(), L"<!")) {
			const std::size_t start = context.Position();
			context.Advance(2);
			while (context.Position() < context.Size()
				&& context.At(context.Position()) != L'>') {
				context.Advance();
			}
			if (context.Position() < context.Size()) {
				context.Advance();
			} else {
				context.MarkTerminal(CodeHighlightTerminalState::UnterminatedConstruct);
			}
			context.Emit(CodeTokenKind::Tag, start, context.Position());
		} else if (value == L'<' && ScanHtmlLikeTag(context, false)) {
			continue;
		} else if (value == L'&') {
			ScanHtmlEntity(context, context.Size());
		} else {
			context.Advance();
		}
	}
}

[[nodiscard]] bool IsMarkdownFence(
	LexerContext& context,
	std::size_t lineStart,
	std::size_t lineEnd,
	wchar_t requiredFence,
	std::size_t requiredLength,
	wchar_t& fence,
	std::size_t& fenceLength)
{
	std::size_t cursor = lineStart;
	std::size_t spaces = 0;
	while (cursor < lineEnd && spaces < 3 && context.At(cursor) == L' ') {
		++cursor;
		++spaces;
	}
	const wchar_t value = context.At(cursor);
	if ((value != L'`' && value != L'~')
		|| (requiredFence != L'\0' && value != requiredFence)) {
		return false;
	}
	const std::size_t runStart = cursor;
	while (cursor < lineEnd && context.At(cursor) == value) {
		++cursor;
	}
	const std::size_t runLength = cursor - runStart;
	if (runLength < 3 || runLength < requiredLength) {
		return false;
	}
	fence = value;
	fenceLength = runLength;
	return true;
}

void ScanMarkdownCodeSpan(LexerContext& context, std::size_t lineEnd)
{
	const std::size_t start = context.Position();
	const wchar_t delimiter = context.At(start);
	std::size_t openingLength = 0;
	while (context.Position() < lineEnd && context.At(context.Position()) == delimiter) {
		context.Advance();
		++openingLength;
	}
	while (context.Position() < lineEnd) {
		if (context.At(context.Position()) != delimiter) {
			context.Advance();
			continue;
		}
		const std::size_t runStart = context.Position();
		std::size_t runLength = 0;
		while (context.Position() < lineEnd
			&& context.At(context.Position()) == delimiter) {
			context.Advance();
			++runLength;
		}
		if (runLength >= openingLength) {
			context.Emit(CodeTokenKind::Code, start, context.Position());
			return;
		}
		context.SetPosition(runStart + runLength);
	}
	context.Emit(CodeTokenKind::Code, start, lineEnd);
	context.MarkTerminal(CodeHighlightTerminalState::UnterminatedConstruct);
}

void ScanMarkdownLink(LexerContext& context, std::size_t lineEnd, bool image)
{
	const std::size_t start = context.Position();
	if (image) {
		context.Advance();
	}
	context.Advance(); // '['
	while (context.Position() < lineEnd && context.At(context.Position()) != L']') {
		context.Advance();
	}
	if (context.Position() >= lineEnd) {
		context.SetPosition(lineEnd);
		context.MarkTerminal(CodeHighlightTerminalState::UnterminatedConstruct);
		return;
	}
	context.Advance();
	if (context.At(context.Position()) == L'(') {
		context.Advance();
		while (context.Position() < lineEnd && context.At(context.Position()) != L')') {
			context.Advance();
		}
		if (context.Position() >= lineEnd) {
			context.SetPosition(lineEnd);
			context.Emit(CodeTokenKind::Link, start, lineEnd);
			context.MarkTerminal(CodeHighlightTerminalState::UnterminatedConstruct);
			return;
		}
		context.Advance();
	}
	context.Emit(CodeTokenKind::Link, start, context.Position());
}

void ScanMarkdownAutolink(LexerContext& context, std::size_t lineEnd)
{
	const std::size_t start = context.Position();
	context.Advance();
	while (context.Position() < lineEnd && context.At(context.Position()) != L'>') {
		context.Advance();
	}
	if (context.Position() >= lineEnd) {
		context.SetPosition(lineEnd);
		context.MarkTerminal(CodeHighlightTerminalState::UnterminatedConstruct);
		return;
	}
	context.Advance();
	context.Emit(CodeTokenKind::Link, start, context.Position());
}

void ScanMarkdownMath(LexerContext& context, std::size_t lineEnd)
{
	const std::size_t start = context.Position();
	context.Advance();
	while (context.Position() < lineEnd && context.At(context.Position()) != L'$') {
		context.Advance();
	}
	if (context.Position() >= lineEnd) {
		context.SetPosition(lineEnd);
		context.Emit(CodeTokenKind::Code, start, lineEnd);
		context.MarkTerminal(CodeHighlightTerminalState::UnterminatedConstruct);
		return;
	}
	context.Advance();
	context.Emit(CodeTokenKind::Code, start, context.Position());
}

void LexMarkdownInline(LexerContext& context, std::size_t lineEnd)
{
	while (context.Position() < lineEnd) {
		const std::size_t special =
			context.FindMarkdownSpecial(context.Position(), lineEnd);
		context.SetPosition(special);
		if (context.Position() >= lineEnd) {
			return;
		}
		const wchar_t value = context.At(context.Position());
		if (value == L'`') {
			ScanMarkdownCodeSpan(context, lineEnd);
		} else if (value == L'!' && context.At(context.Position() + 1) == L'[') {
			ScanMarkdownLink(context, lineEnd, true);
		} else if (value == L'[') {
			ScanMarkdownLink(context, lineEnd, false);
		} else if (value == L'<') {
			ScanMarkdownAutolink(context, lineEnd);
		} else if (value == L'&') {
			ScanHtmlEntity(context, lineEnd);
		} else if (value == L'$') {
			ScanMarkdownMath(context, lineEnd);
		} else if (value == L'\\') {
			const std::size_t start = context.Position();
			context.Advance(context.Position() + 1 < lineEnd ? 2 : 1);
			context.Emit(CodeTokenKind::Literal, start, context.Position());
		} else if (value == L'*' || value == L'_' || value == L'~') {
			const std::size_t start = context.Position();
			while (context.Position() < lineEnd && context.At(context.Position()) == value) {
				context.Advance();
			}
			context.Emit(CodeTokenKind::Emphasis, start, context.Position());
		} else {
			context.Advance();
		}
	}
}

void LexMarkdown(LexerContext& context)
{
	bool insideFence = false;
	wchar_t fence = L'\0';
	std::size_t fenceLength = 0;
	while (context.Position() < context.Size()) {
		const std::size_t lineStart = context.Position();
		const std::size_t lineEnd = context.FindCrOrLf(lineStart);
		wchar_t candidateFence = L'\0';
		std::size_t candidateLength = 0;
		if (IsMarkdownFence(
			context,
			lineStart,
			lineEnd,
			insideFence ? fence : L'\0',
			insideFence ? fenceLength : 3,
			candidateFence,
			candidateLength)) {
			context.Emit(CodeTokenKind::Code, lineStart, lineEnd);
			context.SetPosition(lineEnd);
			if (insideFence) {
				insideFence = false;
				fence = L'\0';
				fenceLength = 0;
			} else {
				insideFence = true;
				fence = candidateFence;
				fenceLength = candidateLength;
			}
		} else if (insideFence) {
			context.Emit(CodeTokenKind::Code, lineStart, lineEnd);
			context.SetPosition(lineEnd);
		} else {
			std::size_t headingEnd = lineStart;
			while (headingEnd < lineEnd && headingEnd - lineStart < 6
				&& context.At(headingEnd) == L'#') {
				++headingEnd;
			}
			if (headingEnd > lineStart
				&& (headingEnd == lineEnd || context.At(headingEnd) == L' ')) {
				context.Emit(CodeTokenKind::Heading, lineStart, lineEnd);
				context.SetPosition(lineEnd);
			} else {
				context.SetPosition(lineStart);
				LexMarkdownInline(context, lineEnd);
			}
		}

		if (context.Position() < context.Size()) {
			const wchar_t first = context.At(context.Position());
			context.Advance();
			if (first == L'\r' && context.At(context.Position()) == L'\n') {
				context.Advance();
			}
		}
	}
	if (insideFence) {
		context.MarkTerminal(CodeHighlightTerminalState::UnterminatedConstruct);
	}
}

} // namespace

std::wstring NormalizeMarkdownCodeLanguage(std::wstring_view language)
{
	while (!language.empty()
		&& (language.front() == L' ' || language.front() == L'\t'
			|| language.front() == L'\r' || language.front() == L'\n')) {
		language.remove_prefix(1);
	}
	while (!language.empty()
		&& (language.back() == L' ' || language.back() == L'\t'
			|| language.back() == L'\r' || language.back() == L'\n')) {
		language.remove_suffix(1);
	}
	std::wstring normalized(language);
	for (wchar_t& value : normalized) {
		value = FoldAscii(value);
	}

	if (normalized.empty() || normalized == L"plain" || normalized == L"plaintext"
		|| normalized == L"text" || normalized == L"txt") return L"plain";
	if (normalized == L"c") return L"c";
	if (normalized == L"cpp" || normalized == L"c++" || normalized == L"cc"
		|| normalized == L"cxx") return L"cpp";
	if (normalized == L"cs" || normalized == L"c#" || normalized == L"csharp") return L"cs";
	if (normalized == L"js" || normalized == L"javascript") return L"js";
	if (normalized == L"ts" || normalized == L"typescript") return L"ts";
	if (normalized == L"jsx" || normalized == L"tsx"
		|| normalized == L"typescriptreact") return L"jsx";
	if (normalized == L"json" || normalized == L"json5" || normalized == L"jsonc") return L"json";
	if (normalized == L"python" || normalized == L"py" || normalized == L"py3"
		|| normalized == L"python3") return L"python";
	if (normalized == L"sh" || normalized == L"shell" || normalized == L"bash"
		|| normalized == L"zsh") return L"sh";
	if (normalized == L"powershell" || normalized == L"ps1" || normalized == L"pwsh") {
		return L"powershell";
	}
	if (normalized == L"css") return L"css";
	if (normalized == L"html" || normalized == L"htm") return L"html";
	if (normalized == L"markdown" || normalized == L"md") return L"markdown";
	return L"plain";
}

CodeHighlightResult HighlightMarkdownCode(
	std::wstring_view language,
	std::wstring_view source,
	std::size_t maximumTokens)
{
	static_assert(sizeof(wchar_t) == 2, "Markdown offsets require UTF-16 wchar_t");
	std::wstring normalized = NormalizeMarkdownCodeLanguage(language);
	const LanguageId languageId = GetLanguageId(normalized);
	LexerContext context(std::move(normalized), source, maximumTokens);

	switch (languageId) {
	case LanguageId::Plain:
		context.RecordWork(source.size());
		context.SetPosition(source.size());
		break;
	case LanguageId::Html:
		LexHtml(context);
		break;
	case LanguageId::Markdown:
		LexMarkdown(context);
		break;
	default:
		LexGeneric(context, GetLanguageSpec(languageId));
		break;
	}
	return context.Finish();
}

} // namespace markdown
