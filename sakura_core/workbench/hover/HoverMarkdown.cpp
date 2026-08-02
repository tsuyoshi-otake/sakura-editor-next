/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/hover/HoverMarkdown.h"

#include <algorithm>
#include <array>
#include <cwctype>

namespace workbench::hover {
namespace {

//! 入力原文の上限。拡張機能から渡される信頼できない文字列に対する最初の関門。
constexpr std::size_t kMaxInputChars = 16384;
//! ブロック数の上限。
constexpr std::size_t kMaxBlocks = 128;
//! テーブル 1 つあたりの行数・列数の上限。
constexpr std::size_t kMaxTableRows = 64;
constexpr std::size_t kMaxTableColumns = 16;
//! インライン記法の閉じ記号を探す先読み幅。
constexpr std::size_t kMaxSpanLookahead = 512;
//! インライン記法のネスト深度上限。
constexpr int kMaxNestingDepth = 24;
//! ドキュメント全体で表示する文字数の上限。超過ぶんは可視の "..." で打ち切る。
constexpr std::size_t kMaxDisplayChars = 4096;
//! `$(name)` のアイコン名として許す長さ。
constexpr std::size_t kMaxIconIdChars = 64;

//! std::iswspace は C ロケールで U+00A0 を空白と見なさないため、明示的に加える。
[[nodiscard]] bool IsTrimmableWhitespace(wchar_t ch) noexcept
{
	if (ch == 0x00A0) return true;
	return std::iswspace(static_cast<std::wint_t>(ch)) != 0;
}

[[nodiscard]] std::wstring TrimCopy(std::wstring_view text)
{
	std::size_t begin = 0;
	while (begin < text.size() && IsTrimmableWhitespace(text[begin])) ++begin;
	std::size_t end = text.size();
	while (end > begin && IsTrimmableWhitespace(text[end - 1])) --end;
	return std::wstring(text.substr(begin, end - begin));
}

struct SHtmlEntity {
	std::wstring_view name;
	wchar_t value;
};

/*!
	HTML 名前付き実体参照のテーブル。

	実 VS Code は tooltip の MarkdownString を HTML として描画するため、拡張機能が
	埋めるべき値の無い列へ流し込む `&nbsp;` は不可視の非改行スペースになる。ここは
	WHATWG の完全な名前付き参照リスト（2000 名以上）ではなく、実際に観測された範囲に
	限定した固定表である。表に無い名前はデコードせずリテラルとして残す。
*/
constexpr std::array<SHtmlEntity, 15> kHtmlEntityTable{ {
	{ L"nbsp", 0x00A0 },
	{ L"amp", L'&' },
	{ L"lt", L'<' },
	{ L"gt", L'>' },
	{ L"quot", L'"' },
	{ L"apos", L'\'' },
	{ L"ensp", 0x2002 },
	{ L"emsp", 0x2003 },
	{ L"thinsp", 0x2009 },
	{ L"middot", 0x00B7 },
	{ L"hellip", 0x2026 },
	{ L"ndash", 0x2013 },
	{ L"mdash", 0x2014 },
	{ L"times", 0x00D7 },
	{ L"vert", L'|' },
} };

//! "&" から ";" までを探す最大幅。
constexpr std::size_t kHtmlEntityMaxLookahead = 32;

void AppendCodePoint(std::wstring& out, unsigned long codePoint)
{
	if (codePoint <= 0xFFFF) {
		out.push_back(static_cast<wchar_t>(codePoint));
		return;
	}
	const unsigned long value = codePoint - 0x10000;
	out.push_back(static_cast<wchar_t>(0xD800 + (value >> 10)));
	out.push_back(static_cast<wchar_t>(0xDC00 + (value & 0x3FF)));
}

/*!
	HTML 実体参照をデコードする。

	テーブルのセル分割が終わったあとにだけ呼ぶこと。これより前で呼ぶと `&#124;` が
	実際の "|" に化けてセルを 1 つ増やしてしまう。
*/
[[nodiscard]] std::wstring DecodeHtmlEntities(std::wstring_view text)
{
	std::wstring result;
	result.reserve(text.size());
	for (std::size_t cursor = 0; cursor < text.size();) {
		if (text[cursor] != L'&') {
			result.push_back(text[cursor]);
			++cursor;
			continue;
		}
		const std::size_t limit = std::min(text.size(), cursor + 1 + kHtmlEntityMaxLookahead);
		std::size_t semicolon = std::wstring_view::npos;
		for (std::size_t scan = cursor + 1; scan < limit; ++scan) {
			if (text[scan] == L';') { semicolon = scan; break; }
			if (text[scan] == L'&' || IsTrimmableWhitespace(text[scan])) break;
		}
		if (semicolon == std::wstring_view::npos || semicolon == cursor + 1) {
			result.push_back(L'&');
			++cursor;
			continue;
		}
		const std::wstring_view body = text.substr(cursor + 1, semicolon - cursor - 1);
		bool decoded = false;
		if (body[0] == L'#') {
			const bool hex = body.size() > 1 && (body[1] == L'x' || body[1] == L'X');
			const std::wstring_view digits = body.substr(hex ? 2 : 1);
			if (!digits.empty() && digits.size() <= 8) {
				unsigned long value = 0;
				bool valid = true;
				for (const wchar_t digit : digits) {
					unsigned long weight = 0;
					if (digit >= L'0' && digit <= L'9') weight = static_cast<unsigned long>(digit - L'0');
					else if (hex && digit >= L'a' && digit <= L'f') weight = static_cast<unsigned long>(digit - L'a') + 10;
					else if (hex && digit >= L'A' && digit <= L'F') weight = static_cast<unsigned long>(digit - L'A') + 10;
					else { valid = false; break; }
					value = value * (hex ? 16 : 10) + weight;
					if (value > 0x10FFFF) { valid = false; break; }
				}
				// 単独サロゲートは UTF-16 として不正なので採用しない。
				if (valid && value != 0 && !(value >= 0xD800 && value <= 0xDFFF)) {
					AppendCodePoint(result, value);
					decoded = true;
				}
			}
		}
		else {
			for (const auto& entity : kHtmlEntityTable) {
				if (entity.name == body) {
					result.push_back(entity.value);
					decoded = true;
					break;
				}
			}
		}
		if (!decoded) {
			result.push_back(L'&');
			++cursor;
			continue;
		}
		cursor = semicolon + 1;
	}
	return result;
}

//! Markdown リンクの destination だけを取り出す。タイトルや山括弧は表示しない。
[[nodiscard]] std::wstring ExtractLinkTarget(std::wstring_view raw)
{
	const std::wstring decoded = DecodeHtmlEntities(raw);
	const std::wstring trimmed = TrimCopy(decoded);
	if (trimmed.size() >= 2 && trimmed.front() == L'<') {
		if (const std::size_t close = trimmed.find(L'>', 1); close != std::wstring::npos) {
			return TrimCopy(std::wstring_view(trimmed).substr(1, close - 1));
		}
	}
	std::size_t end = 0;
	while (end < trimmed.size() && !IsTrimmableWhitespace(trimmed[end])) ++end;
	return trimmed.substr(0, end);
}

/*!
	インライン HTML を落とす。`<br>` だけは改行へ変換する。

	VS Code の hover は `supportHtml` が無い限り HTML を実体としては描かないが、
	`<br>` は markdown-it の既定で改行として通る。閉じられていない "<" は
	文字としてそのまま残し、残りの文字列を飲み込まない。
*/
[[nodiscard]] std::wstring StripInlineHtml(std::wstring_view text)
{
	std::wstring result;
	result.reserve(text.size());
	for (std::size_t cursor = 0; cursor < text.size();) {
		if (text[cursor] != L'<') {
			result.push_back(text[cursor]);
			++cursor;
			continue;
		}
		const std::size_t close = text.find(L'>', cursor + 1);
		if (close == std::wstring_view::npos) {
			result.push_back(L'<');
			++cursor;
			continue;
		}
		std::wstring name;
		for (std::size_t scan = cursor + 1; scan < close && name.size() < 8; ++scan) {
			const wchar_t ch = text[scan];
			if (ch == L'/' ) continue;
			if (!std::iswalpha(static_cast<std::wint_t>(ch))) break;
			name.push_back(static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(ch))));
		}
		if (name == L"br") result.push_back(L'\n');
		cursor = close + 1;
	}
	return result;
}

[[nodiscard]] bool IsHorizontalRuleLine(std::wstring_view line)
{
	const std::wstring trimmed = TrimCopy(line);
	if (trimmed.size() < 3) return false;
	const wchar_t marker = trimmed[0];
	if (marker != L'-' && marker != L'*' && marker != L'_') return false;
	for (const wchar_t ch : trimmed) {
		if (ch != marker && !IsTrimmableWhitespace(ch)) return false;
	}
	return true;
}

//! `| :--- | :---: | ---: |` 形式の区切り行かどうか。
[[nodiscard]] bool IsTableDelimiterLine(std::wstring_view line)
{
	const std::wstring trimmed = TrimCopy(line);
	if (trimmed.empty()) return false;
	bool sawDash = false;
	for (const wchar_t ch : trimmed) {
		if (ch == L'-') { sawDash = true; continue; }
		if (ch == L'|' || ch == L':' || IsTrimmableWhitespace(ch)) continue;
		return false;
	}
	return sawDash;
}

/*!
	テーブル行をセルへ分割する。

	先頭と末尾の "|" を 1 つずつ落としたうえで、エスケープされていない "|" で切る。
	`\|` はセル内のリテラルなパイプなので分割点にしない（そのままセル本文に残し、
	インライン解析側でエスケープを解く）。
*/
[[nodiscard]] std::vector<std::wstring_view> SplitTableCells(std::wstring_view line)
{
	std::wstring_view body = line;
	while (!body.empty() && IsTrimmableWhitespace(body.front())) body.remove_prefix(1);
	while (!body.empty() && IsTrimmableWhitespace(body.back())) body.remove_suffix(1);
	if (!body.empty() && body.front() == L'|') body.remove_prefix(1);
	if (!body.empty() && body.back() == L'|' && (body.size() < 2 || body[body.size() - 2] != L'\\')) {
		body.remove_suffix(1);
	}

	std::vector<std::wstring_view> cells;
	std::size_t start = 0;
	for (std::size_t cursor = 0; cursor < body.size(); ++cursor) {
		if (body[cursor] == L'\\') { ++cursor; continue; }
		if (body[cursor] != L'|') continue;
		if (cells.size() >= kMaxTableColumns) break;
		cells.push_back(body.substr(start, cursor - start));
		start = cursor + 1;
	}
	if (cells.size() < kMaxTableColumns) cells.push_back(body.substr(start));
	return cells;
}

[[nodiscard]] std::vector<EColumnAlign> ParseColumnAlignments(std::wstring_view delimiterLine)
{
	std::vector<EColumnAlign> alignments;
	for (const std::wstring_view cell : SplitTableCells(delimiterLine)) {
		const std::wstring trimmed = TrimCopy(cell);
		const bool leadingColon = !trimmed.empty() && trimmed.front() == L':';
		const bool trailingColon = trimmed.size() > 1 && trimmed.back() == L':';
		if (leadingColon && trailingColon) alignments.push_back(EColumnAlign::Center);
		else if (trailingColon) alignments.push_back(EColumnAlign::Right);
		else alignments.push_back(EColumnAlign::Left);
	}
	return alignments;
}

[[nodiscard]] std::size_t RunLength(std::wstring_view text, std::size_t from, wchar_t marker) noexcept
{
	std::size_t length = 0;
	while (from + length < text.size() && text[from + length] == marker) ++length;
	return length;
}

//! `from` 以降、先読み幅の範囲内で、ちょうど `marker` が `length` 個並ぶ位置を探す。
[[nodiscard]] std::size_t FindClosingRun(std::wstring_view text, std::size_t from, wchar_t marker, std::size_t length)
{
	const std::size_t limit = std::min(text.size(), from + kMaxSpanLookahead);
	for (std::size_t cursor = from; cursor < limit; ++cursor) {
		if (text[cursor] == L'\\') { ++cursor; continue; }
		if (text[cursor] != marker) continue;
		const std::size_t run = RunLength(text, cursor, marker);
		if (run >= length) return cursor;
		cursor += run - 1;
	}
	return std::wstring_view::npos;
}

//! Markdown がバックスラッシュでのエスケープを認める ASCII 約物。
[[nodiscard]] bool IsEscapablePunctuation(wchar_t ch) noexcept
{
	constexpr std::wstring_view punctuation = LR"(!"#$%&'()*+,-./:;<=>?@[\]^_`{|}~)";
	return punctuation.find(ch) != std::wstring_view::npos;
}

[[nodiscard]] bool IsIconIdChar(wchar_t ch) noexcept
{
	if (ch >= L'a' && ch <= L'z') return true;
	if (ch >= L'A' && ch <= L'Z') return true;
	if (ch >= L'0' && ch <= L'9') return true;
	return ch == L'-' || ch == L'_' || ch == L'.' || ch == L'~';
}

//! ドキュメント全体で共有する解析状態。
struct SParseState {
	std::size_t remainingChars = kMaxDisplayChars;
	bool truncated = false;
	bool supportThemeIcons = false;
};

void AppendRun(SInlineText& out, SInlineRun run)
{
	if (run.text.empty() && run.iconId.empty()) return;
	if (!out.empty() && run.iconId.empty() && out.back().iconId.empty() &&
		out.back().bold == run.bold && out.back().italic == run.italic &&
		out.back().code == run.code && out.back().link == run.link &&
		out.back().linkTarget == run.linkTarget) {
		out.back().text.append(run.text);
		return;
	}
	out.push_back(std::move(run));
}

//! 予算内に収まるぶんだけテキストを積む。サロゲートペアの途中では切らない。
void AppendBounded(SInlineText& out, const SInlineRun& style, std::wstring text, SParseState& state)
{
	if (text.empty()) return;
	if (state.remainingChars == 0) { state.truncated = true; return; }
	if (text.size() > state.remainingChars) {
		std::size_t cut = state.remainingChars;
		if (cut > 0 && text[cut - 1] >= 0xD800 && text[cut - 1] <= 0xDBFF) --cut;
		text.resize(cut);
		state.truncated = true;
	}
	state.remainingChars -= text.size();
	SInlineRun run = style;
	run.text = std::move(text);
	run.iconId.clear();
	AppendRun(out, std::move(run));
}

void ParseInline(std::wstring_view text, const SInlineRun& style, int depth, SInlineText& out, SParseState& state);

//! 未解決のリテラル片を実体参照デコードのうえで積む。
void FlushLiteral(std::wstring& literal, const SInlineRun& style, SInlineText& out, SParseState& state)
{
	if (literal.empty()) return;
	AppendBounded(out, style, DecodeHtmlEntities(literal), state);
	literal.clear();
}

void ParseInline(std::wstring_view text, const SInlineRun& style, int depth, SInlineText& out, SParseState& state)
{
	std::wstring literal;
	for (std::size_t cursor = 0; cursor < text.size();) {
		if (state.remainingChars == 0) { state.truncated = true; break; }
		const wchar_t ch = text[cursor];

		// バックスラッシュエスケープ。MarkdownString.appendText() が生成するため、
		// 実際のペイロードに頻出する。
		if (ch == L'\\' && cursor + 1 < text.size() && IsEscapablePunctuation(text[cursor + 1])) {
			literal.push_back(text[cursor + 1]);
			cursor += 2;
			continue;
		}

		// インラインコード。中身はエスケープも実体参照も解釈しない（CommonMark 準拠）。
		if (depth < kMaxNestingDepth && ch == L'`') {
			const std::size_t openRun = RunLength(text, cursor, L'`');
			const std::size_t close = FindClosingRun(text, cursor + openRun, L'`', openRun);
			if (close != std::wstring_view::npos) {
				FlushLiteral(literal, style, out, state);
				SInlineRun codeStyle = style;
				codeStyle.code = true;
				AppendBounded(out, codeStyle,
					std::wstring(text.substr(cursor + openRun, close - cursor - openRun)), state);
				cursor = close + openRun;
				continue;
			}
		}

		// 強調。"**"/"__" は太字、"*"/"_" は斜体。
		if (depth < kMaxNestingDepth && (ch == L'*' || ch == L'_')) {
			const std::size_t openRun = RunLength(text, cursor, ch);
			const std::size_t matchLen = std::min<std::size_t>(openRun, 2);
			const std::size_t close = FindClosingRun(text, cursor + openRun, ch, matchLen);
			if (close != std::wstring_view::npos) {
				FlushLiteral(literal, style, out, state);
				SInlineRun nested = style;
				if (matchLen >= 2) nested.bold = true; else nested.italic = true;
				ParseInline(text.substr(cursor + openRun, close - cursor - openRun), nested, depth + 1, out, state);
				cursor = close + matchLen;
				continue;
			}
		}

		// リンク。ラベルだけを描き、URI とタイトルは落とす。
		if (depth < kMaxNestingDepth && ch == L'[') {
			const std::size_t labelSearchEnd = std::min(text.size(), cursor + 1 + kMaxSpanLookahead);
			const std::size_t labelClose = text.find(L']', cursor + 1);
			if (labelClose != std::wstring_view::npos && labelClose < labelSearchEnd &&
				labelClose + 1 < text.size() && text[labelClose + 1] == L'(') {
				const std::size_t targetSearchEnd = std::min(text.size(), labelClose + 2 + kMaxSpanLookahead);
				const std::size_t targetClose = text.find(L')', labelClose + 2);
				if (targetClose != std::wstring_view::npos && targetClose < targetSearchEnd) {
					FlushLiteral(literal, style, out, state);
					SInlineRun nested = style;
					nested.link = true;
					nested.linkTarget = ExtractLinkTarget(
						text.substr(labelClose + 2, targetClose - labelClose - 2));
					ParseInline(text.substr(cursor + 1, labelClose - cursor - 1), nested, depth + 1, out, state);
					cursor = targetClose + 1;
					continue;
				}
			}
		}

		// テーマアイコン。supportThemeIcons が真のときだけアイコンとして解釈する。
		if (ch == L'$' && cursor + 1 < text.size() && text[cursor + 1] == L'(') {
			const std::size_t close = text.find(L')', cursor + 2);
			if (close != std::wstring_view::npos && close - cursor - 2 > 0 &&
				close - cursor - 2 <= kMaxIconIdChars) {
				const std::wstring_view name = text.substr(cursor + 2, close - cursor - 2);
				if (std::all_of(name.begin(), name.end(), IsIconIdChar)) {
					if (state.supportThemeIcons) {
						FlushLiteral(literal, style, out, state);
						// VS Code の ThemeIcon.fromString と同じく "~spin" 等の
						// modifier はアイコン名から切り離す。
						std::wstring_view iconId = name;
						if (const std::size_t modifier = iconId.find(L'~'); modifier != std::wstring_view::npos) {
							iconId = iconId.substr(0, modifier);
						}
						if (!iconId.empty() && state.remainingChars > 0) {
							--state.remainingChars;
							SInlineRun run = style;
							run.iconId.assign(iconId);
							AppendRun(out, std::move(run));
						}
						cursor = close + 1;
						continue;
					}
					// supportThemeIcons が偽なら VS Code はそのまま文字として描く。
				}
			}
		}

		literal.push_back(ch);
		++cursor;
	}
	FlushLiteral(literal, style, out, state);
}

[[nodiscard]] SInlineText ParseInlineText(std::wstring_view text, SParseState& state)
{
	SInlineText runs;
	ParseInline(text, SInlineRun{}, 0, runs, state);
	return runs;
}

[[nodiscard]] bool IsBlank(const SInlineText& runs) noexcept
{
	return std::all_of(runs.begin(), runs.end(), [](const SInlineRun& run) {
		if (!run.iconId.empty()) return false;
		return std::all_of(run.text.begin(), run.text.end(), IsTrimmableWhitespace);
	});
}

[[nodiscard]] std::wstring RunsToPlainText(const SInlineText& runs)
{
	std::wstring text;
	for (const SInlineRun& run : runs) text.append(run.text);
	return text;
}

} // namespace

SDocument Parse(std::wstring_view markdown, const SParseOptions& options)
{
	SDocument document;
	if (markdown.empty()) return document;
	bool inputTruncated = false;
	if (markdown.size() > kMaxInputChars) {
		markdown = markdown.substr(0, kMaxInputChars);
		inputTruncated = true;
	}

	// 改行コードを正規化する（"\r\n" と単独の "\r" を "\n" にする）。
	std::wstring normalized;
	normalized.reserve(markdown.size());
	for (std::size_t index = 0; index < markdown.size(); ++index) {
		const wchar_t ch = markdown[index];
		if (ch == L'\r') {
			if (index + 1 >= markdown.size() || markdown[index + 1] != L'\n') normalized.push_back(L'\n');
			continue;
		}
		normalized.push_back(ch);
	}

	const std::wstring source = StripInlineHtml(normalized);

	std::vector<std::wstring_view> lines;
	{
		std::size_t lineStart = 0;
		for (std::size_t index = 0; index <= source.size(); ++index) {
			if (index == source.size() || source[index] == L'\n') {
				lines.push_back(std::wstring_view(source).substr(lineStart, index - lineStart));
				lineStart = index + 1;
			}
		}
	}

	SParseState state;
	state.supportThemeIcons = options.supportThemeIcons;
	state.truncated = inputTruncated;

	SBlock paragraph;
	const auto flushParagraph = [&document, &paragraph, &state]() {
		if (paragraph.lines.empty()) return;
		if (document.blocks.size() < kMaxBlocks) document.blocks.push_back(std::move(paragraph));
		else state.truncated = true; // 捨てた事実を握り潰さない
		paragraph = SBlock{};
	};

	// index はループの外で持つ。ブロック数の上限で抜けたとき、残りの行を黙って
	// 落としたのではなく打ち切ったのだと呼び出し側が判別できるようにするため。
	std::size_t index = 0;
	for (; index < lines.size() && document.blocks.size() < kMaxBlocks; ++index) {
		const std::wstring_view line = lines[index];
		const std::wstring trimmed = TrimCopy(line);

		if (trimmed.empty()) {
			flushParagraph();
			continue;
		}

		// フェンス付きコードブロック。
		if (trimmed.rfind(L"```", 0) == 0) {
			flushParagraph();
			SBlock code;
			code.kind = EBlockKind::CodeBlock;
			++index;
			for (; index < lines.size(); ++index) {
				if (TrimCopy(lines[index]).rfind(L"```", 0) == 0) break;
				SInlineRun run;
				run.code = true;
				code.lines.push_back(SInlineText{});
				AppendBounded(code.lines.back(), run, std::wstring(lines[index]), state);
			}
			if (!code.lines.empty()) document.blocks.push_back(std::move(code));
			continue;
		}

		if (IsHorizontalRuleLine(line)) {
			flushParagraph();
			SBlock rule;
			rule.kind = EBlockKind::HorizontalRule;
			document.blocks.push_back(std::move(rule));
			continue;
		}

		// 見出し。
		if (trimmed.front() == L'#') {
			std::size_t level = 0;
			while (level < trimmed.size() && trimmed[level] == L'#' && level < 6) ++level;
			if (level < trimmed.size() && IsTrimmableWhitespace(trimmed[level])) {
				flushParagraph();
				std::wstring_view body(trimmed);
				body.remove_prefix(level);
				while (!body.empty() && body.back() == L'#') body.remove_suffix(1);
				SBlock heading;
				heading.kind = EBlockKind::Heading;
				heading.level = static_cast<int>(level);
				heading.lines.push_back(ParseInlineText(TrimCopy(body), state));
				document.blocks.push_back(std::move(heading));
				continue;
			}
		}

		// テーブル。見出し行の次が区切り行のときだけテーブルとして扱う。
		const bool looksLikeTableRow = line.find(L'|') != std::wstring_view::npos;
		if (looksLikeTableRow && index + 1 < lines.size() &&
			lines[index + 1].find(L'|') != std::wstring_view::npos &&
			IsTableDelimiterLine(lines[index + 1])) {
			flushParagraph();
			SBlock table;
			table.kind = EBlockKind::Table;
			table.alignments = ParseColumnAlignments(lines[index + 1]);

			const auto appendRow = [&table, &state](std::wstring_view rowLine, bool header) {
				STableRow row;
				row.header = header;
				for (const std::wstring_view cell : SplitTableCells(rowLine)) {
					row.cells.push_back(ParseInlineText(TrimCopy(cell), state));
				}
				table.rows.push_back(std::move(row));
			};

			appendRow(line, true);
			index += 1; // 区切り行そのものは描かない
			while (index + 1 < lines.size() && table.rows.size() < kMaxTableRows &&
				lines[index + 1].find(L'|') != std::wstring_view::npos) {
				++index;
				appendRow(lines[index], false);
			}
			std::size_t columns = table.alignments.size();
			for (const STableRow& row : table.rows) columns = std::max(columns, row.cells.size());
			table.alignments.resize(columns, EColumnAlign::Left);
			document.blocks.push_back(std::move(table));
			continue;
		}

		// 箇条書き / 番号付きリスト。
		{
			std::size_t indent = 0;
			while (indent < line.size() && (line[indent] == L' ' || line[indent] == L'\t')) ++indent;
			std::wstring_view body = line.substr(indent);
			std::wstring marker;
			if (body.size() >= 2 && (body[0] == L'-' || body[0] == L'*' || body[0] == L'+') &&
				IsTrimmableWhitespace(body[1])) {
				marker = L"\x2022"; // BULLET
				body.remove_prefix(2);
			}
			else {
				std::size_t digits = 0;
				while (digits < body.size() && body[digits] >= L'0' && body[digits] <= L'9') ++digits;
				if (digits > 0 && digits + 1 < body.size() && (body[digits] == L'.' || body[digits] == L')') &&
					IsTrimmableWhitespace(body[digits + 1])) {
					marker.assign(body.substr(0, digits + 1));
					body.remove_prefix(digits + 2);
				}
			}
			if (!marker.empty()) {
				flushParagraph();
				SBlock item;
				item.kind = EBlockKind::ListItem;
				item.level = static_cast<int>(indent / 2);
				item.marker = std::move(marker);
				item.lines.push_back(ParseInlineText(TrimCopy(body), state));
				document.blocks.push_back(std::move(item));
				continue;
			}
		}

		paragraph.kind = EBlockKind::Paragraph;
		paragraph.lines.push_back(ParseInlineText(trimmed, state));
	}
	if (index < lines.size()) state.truncated = true;
	flushParagraph();

	// 打ち切りが起きたことは必ず見えるようにする。黙って切らない。
	if (state.truncated && document.blocks.size() <= kMaxBlocks) {
		SBlock ellipsis;
		ellipsis.kind = EBlockKind::Paragraph;
		SInlineRun run;
		run.text = L"...";
		ellipsis.lines.push_back(SInlineText{ std::move(run) });
		document.blocks.push_back(std::move(ellipsis));
	}

	// 先頭・末尾の水平線は VS Code でも見た目の区切りにならないため落とす。
	while (!document.blocks.empty() && document.blocks.front().kind == EBlockKind::HorizontalRule) {
		document.blocks.erase(document.blocks.begin());
	}
	while (!document.blocks.empty() && document.blocks.back().kind == EBlockKind::HorizontalRule) {
		document.blocks.pop_back();
	}
	return document;
}

std::wstring ToPlainText(const SDocument& document)
{
	std::vector<std::wstring> outputLines;
	for (const SBlock& block : document.blocks) {
		switch (block.kind) {
		case EBlockKind::HorizontalRule:
			outputLines.emplace_back();
			break;
		case EBlockKind::ListItem:
			for (const SInlineText& line : block.lines) {
				outputLines.push_back(block.marker + L" " + RunsToPlainText(line));
			}
			break;
		case EBlockKind::Table:
			for (const STableRow& row : block.rows) {
				std::wstring text;
				for (const SInlineText& cell : row.cells) {
					// 空のセル（`&nbsp;` だけのセルを含む）は連結に加えない。
					// 実 VS Code の表でも視覚的に空欄になるため、平文でも区切りを残さない。
					if (IsBlank(cell)) continue;
					if (!text.empty()) text.append(L" | ");
					text.append(TrimCopy(RunsToPlainText(cell)));
				}
				outputLines.push_back(std::move(text));
			}
			break;
		default:
			for (const SInlineText& line : block.lines) {
				outputLines.push_back(TrimCopy(RunsToPlainText(line)));
			}
			break;
		}
	}

	std::size_t first = 0;
	while (first < outputLines.size() && outputLines[first].empty()) ++first;
	std::size_t last = outputLines.size();
	while (last > first && outputLines[last - 1].empty()) --last;

	std::wstring result;
	bool previousBlank = false;
	bool wroteAny = false;
	for (std::size_t index = first; index < last; ++index) {
		const bool blank = outputLines[index].empty();
		if (blank && previousBlank) continue;
		if (wroteAny) result.push_back(L'\n');
		result.append(outputLines[index]);
		wroteAny = true;
		previousBlank = blank;
	}
	return result;
}

} // namespace workbench::hover
