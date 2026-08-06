/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"
#include "textmate/TextMatePlistGrammarLoader.h"

#include <stdexcept>
#include <string>
#include <utility>

#include "textmate/TextMateUtf8.h"

namespace textmate {

namespace {

constexpr std::size_t kMaximumInputBytes = 1024U * 1024U; // matches JsoncDocument's own cap
constexpr std::size_t kMaximumDepth = 64U;                // matches JsoncDocument's own cap
constexpr std::size_t kMaximumNodes = 65536U;              // matches JsoncDocument's own cap
constexpr std::size_t kMaximumEntityScan = 64U;             // defensive bound on one `&...;` reference

[[nodiscard]] bool IsAsciiWhitespace(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
[[nodiscard]] bool IsNameStartChar(char c) noexcept
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':';
}
[[nodiscard]] bool IsNameChar(char c) noexcept { return IsNameStartChar(c) || (c >= '0' && c <= '9') || c == '-' || c == '.'; }

//! Appends the UTF-8 encoding of one Unicode scalar value. `codepoint` must
//! already be validated as `<= 0x10FFFF` and outside the surrogate range
//! (callers check this before calling); this is the plist loader's own small
//! encoder rather than a reuse of `textmate::EncodeUtf8`, because that
//! function encodes a whole UTF-16 string, not a single already-decoded
//! numeric character reference.
void AppendUtf8(std::string& out, char32_t codepoint)
{
	if (codepoint <= 0x7F) {
		out.push_back(static_cast<char>(codepoint));
	} else if (codepoint <= 0x7FF) {
		out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
		out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
	} else if (codepoint <= 0xFFFF) {
		out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
		out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
	} else {
		out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
		out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
	}
}

[[nodiscard]] bool ParseHexDigits(std::string_view digits, char32_t& out) noexcept
{
	if (digits.empty() || digits.size() > 8) return false;
	char32_t value = 0;
	for (const char c : digits) {
		int digit;
		if (c >= '0' && c <= '9') digit = c - '0';
		else if (c >= 'a' && c <= 'f') digit = 10 + (c - 'a');
		else if (c >= 'A' && c <= 'F') digit = 10 + (c - 'A');
		else return false;
		value = value * 16 + static_cast<char32_t>(digit);
	}
	out = value;
	return true;
}

[[nodiscard]] bool ParseDecimalDigits(std::string_view digits, char32_t& out) noexcept
{
	if (digits.empty() || digits.size() > 10) return false;
	char32_t value = 0;
	for (const char c : digits) {
		if (c < '0' || c > '9') return false;
		value = value * 10 + static_cast<char32_t>(c - '0');
		if (value > 0x10FFFFu) return false;
	}
	out = value;
	return true;
}

//! A minimal, bounded, single-pass recursive-descent reader for the narrow
//! slice of XML that Apple property lists use. See the header comment on
//! `TextMatePlistGrammarLoader` for the XXE-safety argument (the short
//! version: `<!DOCTYPE ...>` internal subsets are skipped as opaque bytes and
//! never interpreted, so no `<!ENTITY>` is ever registered or expanded).
class PlistXmlParser final {
public:
	explicit PlistXmlParser(std::string_view input) noexcept : m_input(input) {}

	[[nodiscard]] TextMatePlistGrammarLoader::ParseResult ParseDocument()
	{
		if (m_input.size() > kMaximumInputBytes) {
			Fail(EPlistDiagnosticCode::InputTooLarge, L"plist input exceeds the maximum accepted size");
			return MakeFailure();
		}
		if (!ValidateUtf8()) {
			Fail(EPlistDiagnosticCode::InvalidUtf8, L"plist input is not valid UTF-8");
			return MakeFailure();
		}

		if (!SkipInterElementNoise()) return MakeFailure();

		if (!StartsWith("<plist")) {
			Fail(EPlistDiagnosticCode::MalformedXml, L"expected a <plist> root element");
			return MakeFailure();
		}

		std::wstring rootName;
		bool rootSelfClosing = false;
		if (!ParseStartTag(rootName, rootSelfClosing)) return MakeFailure();
		if (rootName != L"plist") {
			Fail(EPlistDiagnosticCode::MalformedXml, L"expected a <plist> root element, found <" + rootName + L">");
			return MakeFailure();
		}

		if (rootSelfClosing) {
			// An empty `<plist/>` has no content to compile into a grammar; the
			// compiler will fail on the missing `scopeName`, which is the
			// correct diagnostic for this case (not a parse failure).
			return TextMatePlistGrammarLoader::ParseResult{TextMateGrammarValue(TextMateGrammarValue::Object{}), std::nullopt};
		}

		if (!SkipInterElementNoise()) return MakeFailure();
		std::optional<TextMateGrammarValue> child = ParseValueElement(1);
		if (!child.has_value()) return MakeFailure();

		if (!SkipInterElementNoise()) return MakeFailure();
		if (!ParseEndTag(L"plist")) return MakeFailure();

		// Trailing bytes after `</plist>` (stray whitespace, a trailing
		// comment) are intentionally not rejected; nothing past the root
		// element is semantically meaningful for a grammar document.
		return TextMatePlistGrammarLoader::ParseResult{std::move(*child), std::nullopt};
	}

private:
	[[nodiscard]] bool ValidateUtf8() const noexcept
	{
		if (m_input.empty()) return true;
		const int wideLength =
			::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, m_input.data(), static_cast<int>(m_input.size()), nullptr, 0);
		return wideLength > 0;
	}

	[[nodiscard]] bool StartsWith(std::string_view literal) const noexcept
	{
		return m_input.compare(m_pos, literal.size(), literal) == 0;
	}

	void SkipAsciiWhitespace() noexcept
	{
		while (m_pos < m_input.size() && IsAsciiWhitespace(m_input[m_pos])) ++m_pos;
	}

	bool Fail(EPlistDiagnosticCode code, std::wstring message)
	{
		if (!m_failed) {
			m_failed = true;
			m_diagnostic = PlistParseDiagnostic{code, m_pos, std::move(message)};
		}
		return false;
	}

	[[nodiscard]] TextMatePlistGrammarLoader::ParseResult MakeFailure() const
	{
		return TextMatePlistGrammarLoader::ParseResult{std::nullopt, m_diagnostic};
	}

	//! Skips any run of whitespace, `<!-- comments -->`, `<? processing
	//! instructions ?>`, and `<!DOCTYPE ...>` declarations. Used both before
	//! the root element and between sibling elements inside `<dict>`/`<array>`
	//! (a `<!DOCTYPE>` cannot legally appear in the latter position, but
	//! accepting it there too costs nothing and keeps this one function).
	[[nodiscard]] bool SkipInterElementNoise()
	{
		for (;;) {
			SkipAsciiWhitespace();
			if (StartsWith("<!--")) {
				if (!SkipComment()) return false;
				continue;
			}
			if (StartsWith("<!DOCTYPE")) {
				if (!SkipDoctype()) return false;
				continue;
			}
			if (StartsWith("<?")) {
				if (!SkipProcessingInstruction()) return false;
				continue;
			}
			return true;
		}
	}

	[[nodiscard]] bool SkipComment()
	{
		const std::size_t start = m_pos;
		m_pos += 4; // "<!--"
		const std::size_t closeAt = m_input.find("-->", m_pos);
		if (closeAt == std::string_view::npos) {
			m_pos = start;
			return Fail(EPlistDiagnosticCode::MalformedXml, L"unterminated <!-- comment -->");
		}
		m_pos = closeAt + 3;
		return true;
	}

	[[nodiscard]] bool SkipProcessingInstruction()
	{
		const std::size_t start = m_pos;
		m_pos += 2; // "<?"
		const std::size_t closeAt = m_input.find("?>", m_pos);
		if (closeAt == std::string_view::npos) {
			m_pos = start;
			return Fail(EPlistDiagnosticCode::MalformedXml, L"unterminated <? processing instruction ?>");
		}
		m_pos = closeAt + 2;
		return true;
	}

	//! Skips `<!DOCTYPE ... >`, including a bracketed internal subset
	//! (`<!DOCTYPE plist [ <!ENTITY ...> ]>`), as opaque bytes. This is the
	//! XXE defense: the internal subset's `<!ENTITY>` declarations are never
	//! parsed out or registered anywhere, so nothing later in this file can
	//! ever expand a custom entity, whether it names a local file, a network
	//! URL, or another entity. `subsetDepth` tracks `[`/`]` nesting so that a
	//! `>` appearing *inside* the internal subset (e.g. terminating one
	//! `<!ENTITY ...>` declaration) does not get mistaken for the end of the
	//! whole `<!DOCTYPE ...>`.
	[[nodiscard]] bool SkipDoctype()
	{
		const std::size_t start = m_pos;
		m_pos += 9; // "<!DOCTYPE"
		int subsetDepth = 0;
		while (m_pos < m_input.size()) {
			const char c = m_input[m_pos];
			if (c == '[') {
				++subsetDepth;
				++m_pos;
			} else if (c == ']') {
				if (subsetDepth > 0) --subsetDepth;
				++m_pos;
			} else if (c == '>' && subsetDepth == 0) {
				++m_pos;
				return true;
			} else {
				++m_pos;
			}
		}
		m_pos = start;
		return Fail(EPlistDiagnosticCode::MalformedXml, L"unterminated <!DOCTYPE ...> declaration");
	}

	//! Assumes `m_input[m_pos] == '<'`. Consumes through the tag's closing
	//! `>` (or `/>`), discarding any attributes (plist elements carry no
	//! grammar-relevant attributes; the only one real files use is
	//! `<plist version="1.0">`).
	[[nodiscard]] bool ParseStartTag(std::wstring& outName, bool& outSelfClosing)
	{
		++m_pos; // '<'
		const std::size_t nameStart = m_pos;
		while (m_pos < m_input.size() && IsNameChar(m_input[m_pos])) ++m_pos;
		if (m_pos == nameStart) return Fail(EPlistDiagnosticCode::MalformedXml, L"expected an element name after '<'");
		outName = DecodeUtf8ToWide(m_input.substr(nameStart, m_pos - nameStart));

		for (;;) {
			SkipAsciiWhitespace();
			if (m_pos >= m_input.size()) return Fail(EPlistDiagnosticCode::MalformedXml, L"unterminated start tag <" + outName);
			const char c = m_input[m_pos];
			if (c == '/') {
				++m_pos;
				if (m_pos >= m_input.size() || m_input[m_pos] != '>') {
					return Fail(EPlistDiagnosticCode::MalformedXml, L"malformed self-closing tag <" + outName + L"/>");
				}
				++m_pos;
				outSelfClosing = true;
				return true;
			}
			if (c == '>') {
				++m_pos;
				outSelfClosing = false;
				return true;
			}
			if (!IsNameStartChar(c)) return Fail(EPlistDiagnosticCode::MalformedXml, L"malformed attribute in <" + outName + L">");
			while (m_pos < m_input.size() && IsNameChar(m_input[m_pos])) ++m_pos;
			SkipAsciiWhitespace();
			if (m_pos < m_input.size() && m_input[m_pos] == '=') {
				++m_pos;
				SkipAsciiWhitespace();
				if (m_pos >= m_input.size() || (m_input[m_pos] != '"' && m_input[m_pos] != '\'')) {
					return Fail(EPlistDiagnosticCode::MalformedXml, L"malformed attribute value in <" + outName + L">");
				}
				const char quote = m_input[m_pos];
				++m_pos;
				// Attribute values are discarded verbatim (not entity-decoded):
				// no plist element this loader understands carries
				// grammar-relevant data in an attribute.
				while (m_pos < m_input.size() && m_input[m_pos] != quote) ++m_pos;
				if (m_pos >= m_input.size()) {
					return Fail(EPlistDiagnosticCode::MalformedXml, L"unterminated attribute value in <" + outName + L">");
				}
				++m_pos; // closing quote
			}
		}
	}

	[[nodiscard]] bool ParseEndTag(const std::wstring& expectedName)
	{
		if (m_pos + 1 >= m_input.size() || m_input[m_pos] != '<' || m_input[m_pos + 1] != '/') {
			return Fail(EPlistDiagnosticCode::MalformedXml, L"expected </" + expectedName + L">");
		}
		m_pos += 2;
		const std::size_t nameStart = m_pos;
		while (m_pos < m_input.size() && IsNameChar(m_input[m_pos])) ++m_pos;
		const std::wstring name = DecodeUtf8ToWide(m_input.substr(nameStart, m_pos - nameStart));
		SkipAsciiWhitespace();
		if (m_pos >= m_input.size() || m_input[m_pos] != '>') {
			return Fail(EPlistDiagnosticCode::MalformedXml, L"malformed end tag </" + name + L">");
		}
		++m_pos;
		if (name != expectedName) {
			return Fail(EPlistDiagnosticCode::MalformedXml, L"mismatched end tag: expected </" + expectedName + L">, found </" + name + L">");
		}
		return true;
	}

	//! Assumes `m_input[m_pos] == '&'`. Decodes exactly one entity or
	//! numeric character reference and appends its UTF-8 encoding to `accum`.
	[[nodiscard]] bool DecodeEntityInto(std::string& accum)
	{
		const std::size_t ampPos = m_pos;
		++m_pos; // '&'
		const std::size_t nameStart = m_pos;
		while (m_pos < m_input.size() && m_input[m_pos] != ';' && (m_pos - nameStart) < kMaximumEntityScan) ++m_pos;
		if (m_pos >= m_input.size() || m_input[m_pos] != ';') {
			m_pos = ampPos;
			return Fail(EPlistDiagnosticCode::MalformedXml, L"malformed or unterminated entity reference");
		}
		const std::string_view body = m_input.substr(nameStart, m_pos - nameStart);
		++m_pos; // ';'

		if (body == "amp") { accum.push_back('&'); return true; }
		if (body == "lt") { accum.push_back('<'); return true; }
		if (body == "gt") { accum.push_back('>'); return true; }
		if (body == "quot") { accum.push_back('"'); return true; }
		if (body == "apos") { accum.push_back('\''); return true; }

		if (!body.empty() && body[0] == '#') {
			char32_t codepoint = 0;
			const bool ok = (body.size() > 1 && (body[1] == 'x' || body[1] == 'X'))
				? ParseHexDigits(body.substr(2), codepoint)
				: (body.size() > 1 && ParseDecimalDigits(body.substr(1), codepoint));
			const bool inSurrogateRange = codepoint >= 0xD800u && codepoint <= 0xDFFFu;
			if (!ok || codepoint == 0 || codepoint > 0x10FFFFu || inSurrogateRange) {
				m_pos = ampPos;
				return Fail(EPlistDiagnosticCode::MalformedXml, L"malformed numeric character reference");
			}
			AppendUtf8(accum, codepoint);
			return true;
		}

		// Any other named entity is rejected outright -- see the XXE-safety
		// comment on `SkipDoctype` and on the `TextMatePlistGrammarLoader`
		// class: no `<!ENTITY>` declaration is ever parsed or registered, so
		// there is no table this could ever successfully look up against.
		m_pos = ampPos;
		return Fail(EPlistDiagnosticCode::UnknownEntityReference, L"unknown entity reference '&" + DecodeUtf8ToWide(body) + L";'");
	}

	//! Reads plain character content up to (not including) the next `<`,
	//! decoding entities along the way. Used for `<key>`/`<string>`/`<data>`/
	//! `<date>` content, none of which may contain nested elements.
	[[nodiscard]] std::optional<std::wstring> ParseTextContent()
	{
		std::string utf8Accum;
		for (;;) {
			if (m_pos >= m_input.size()) {
				Fail(EPlistDiagnosticCode::MalformedXml, L"unexpected end of input while reading element text content");
				return std::nullopt;
			}
			const char c = m_input[m_pos];
			if (c == '<') break;
			if (c == '&') {
				if (!DecodeEntityInto(utf8Accum)) return std::nullopt;
				continue;
			}
			utf8Accum.push_back(c);
			++m_pos;
		}
		return DecodeUtf8ToWide(utf8Accum);
	}

	[[nodiscard]] std::optional<std::wstring> ParseKeyElement()
	{
		if (m_pos >= m_input.size() || m_input[m_pos] != '<') {
			Fail(EPlistDiagnosticCode::MalformedXml, L"expected <key> in <dict>");
			return std::nullopt;
		}
		std::wstring name;
		bool selfClosing = false;
		if (!ParseStartTag(name, selfClosing)) return std::nullopt;
		if (name != L"key") {
			Fail(EPlistDiagnosticCode::MalformedXml, L"expected <key>, found <" + name + L">");
			return std::nullopt;
		}
		if (!BumpNodeCount()) return std::nullopt;
		if (selfClosing) return std::wstring();
		std::optional<std::wstring> text = ParseTextContent();
		if (!text.has_value()) return std::nullopt;
		if (!ParseEndTag(name)) return std::nullopt;
		return text;
	}

	[[nodiscard]] std::optional<TextMateGrammarValue> ParseNumericLeafElement(const std::wstring& name, bool isInteger)
	{
		std::optional<std::wstring> text = ParseTextContent();
		if (!text.has_value()) return std::nullopt;
		if (!ParseEndTag(name)) return std::nullopt;

		try {
			std::size_t consumed = 0;
			if (isInteger) {
				const long long parsed = std::stoll(*text, &consumed);
				if (consumed != text->size()) throw std::invalid_argument("trailing characters");
				return TextMateGrammarValue(static_cast<std::int64_t>(parsed));
			}
			const double parsed = std::stod(*text, &consumed);
			if (consumed != text->size()) throw std::invalid_argument("trailing characters");
			return TextMateGrammarValue(parsed);
		} catch (const std::exception&) {
			Fail(EPlistDiagnosticCode::MalformedXml, L"malformed <" + name + L"> content: " + *text);
			return std::nullopt;
		}
	}

	[[nodiscard]] bool BumpNodeCount()
	{
		++m_nodeCount;
		if (m_nodeCount > kMaximumNodes) return Fail(EPlistDiagnosticCode::MaximumNodesExceeded, L"plist document exceeds the maximum accepted element count");
		return true;
	}

	[[nodiscard]] std::optional<TextMateGrammarValue> ParseValueElement(std::size_t depth)
	{
		if (depth > kMaximumDepth) {
			Fail(EPlistDiagnosticCode::MaximumDepthExceeded, L"plist document exceeds the maximum accepted nesting depth");
			return std::nullopt;
		}
		if (m_pos >= m_input.size() || m_input[m_pos] != '<') {
			Fail(EPlistDiagnosticCode::MalformedXml, L"expected a plist value element");
			return std::nullopt;
		}

		std::wstring name;
		bool selfClosing = false;
		if (!ParseStartTag(name, selfClosing)) return std::nullopt;
		if (!BumpNodeCount()) return std::nullopt;

		if (name == L"true" || name == L"false") {
			if (!selfClosing && !ParseEndTag(name)) return std::nullopt;
			return TextMateGrammarValue(name == L"true");
		}
		if (name == L"integer") return ParseNumericLeafElement(name, /*isInteger=*/true);
		if (name == L"real") return ParseNumericLeafElement(name, /*isInteger=*/false);
		if (name == L"string" || name == L"data" || name == L"date") {
			// `<data>` (base64) and `<date>` (ISO 8601) content is carried
			// through as a plain decoded string rather than actually decoded to
			// bytes/a timestamp: no `.tmLanguage` grammar element this loader
			// understands (`patterns`/`repository`/`match`/`begin`/`end`/
			// `captures`/`name`/...) ever uses either type, so this is a
			// best-effort passthrough, not a spec-complete plist reader.
			if (selfClosing) return TextMateGrammarValue(std::wstring());
			std::optional<std::wstring> text = ParseTextContent();
			if (!text.has_value()) return std::nullopt;
			if (!ParseEndTag(name)) return std::nullopt;
			return TextMateGrammarValue(std::move(*text));
		}
		if (name == L"array") {
			TextMateGrammarValue::Array array;
			if (!selfClosing) {
				for (;;) {
					if (!SkipInterElementNoise()) return std::nullopt;
					if (m_pos + 1 < m_input.size() && m_input[m_pos] == '<' && m_input[m_pos + 1] == '/') break;
					if (m_pos >= m_input.size()) {
						Fail(EPlistDiagnosticCode::MalformedXml, L"unterminated <array>");
						return std::nullopt;
					}
					std::optional<TextMateGrammarValue> element = ParseValueElement(depth + 1);
					if (!element.has_value()) return std::nullopt;
					array.push_back(std::move(*element));
				}
				if (!ParseEndTag(name)) return std::nullopt;
			}
			return TextMateGrammarValue(std::move(array));
		}
		if (name == L"dict") {
			TextMateGrammarValue::Object object;
			if (!selfClosing) {
				for (;;) {
					if (!SkipInterElementNoise()) return std::nullopt;
					if (m_pos + 1 < m_input.size() && m_input[m_pos] == '<' && m_input[m_pos + 1] == '/') break;
					if (m_pos >= m_input.size()) {
						Fail(EPlistDiagnosticCode::MalformedXml, L"unterminated <dict>");
						return std::nullopt;
					}
					std::optional<std::wstring> key = ParseKeyElement();
					if (!key.has_value()) return std::nullopt;
					if (!SkipInterElementNoise()) return std::nullopt;
					std::optional<TextMateGrammarValue> value = ParseValueElement(depth + 1);
					if (!value.has_value()) return std::nullopt;
					object.emplace_back(std::move(*key), std::move(*value));
				}
				if (!ParseEndTag(name)) return std::nullopt;
			}
			return TextMateGrammarValue(std::move(object));
		}

		Fail(EPlistDiagnosticCode::UnsupportedRootShape, L"unsupported plist element <" + name + L">");
		return std::nullopt;
	}

	std::string_view m_input;
	std::size_t m_pos = 0;
	std::size_t m_nodeCount = 0;
	bool m_failed = false;
	PlistParseDiagnostic m_diagnostic;
};

} // namespace

TextMatePlistGrammarLoader::ParseResult TextMatePlistGrammarLoader::Parse(std::string_view utf8Source)
{
	PlistXmlParser parser(utf8Source);
	return parser.ParseDocument();
}

GrammarCompileResult TextMatePlistGrammarLoader::Load(std::string_view utf8Source)
{
	const ParseResult parseResult = Parse(utf8Source);
	if (!parseResult.Succeeded()) {
		GrammarCompileResult result;
		GrammarCompileDiagnostic diagnostic;
		if (parseResult.diagnostic.has_value()) {
			diagnostic.message = L"Plist parse failed at byte offset " + std::to_wstring(parseResult.diagnostic->byteOffset) + L": " +
				parseResult.diagnostic->message;
		} else {
			diagnostic.message = L"Plist parse failed with no diagnostic detail (unexpected).";
		}
		result.diagnostics.push_back(std::move(diagnostic));
		return result;
	}
	return TextMateGrammarCompiler::Compile(*parseResult.value);
}

} // namespace textmate
