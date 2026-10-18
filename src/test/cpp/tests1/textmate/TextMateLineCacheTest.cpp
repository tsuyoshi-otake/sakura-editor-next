/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "agent/CSearchAgent.h"
#include "doc/logic/CDocLineMgr.h"
#include "textmate/TextMateJsonGrammarLoader.h"
#include "textmate/TextMateLineCache.h"
#include "textmate/TextMateTokenizer.h"

namespace {

using textmate::RuleStackHandle;
using textmate::TextMateJsonGrammarLoader;
using textmate::TextMateLineCache;
using textmate::TextMateToken;
using textmate::TextMateTokenizer;

//! Same upward-walk idiom as `SenpEffectProtocolTest.cpp::FixturePath`: finds a
//! checked-in file relative to the repository root from either the repo-root
//! entry point or a configuration-directory test run.
std::filesystem::path RepoFilePath(const wchar_t* relative)
{
	auto directory = std::filesystem::current_path();
	for (std::size_t level = 0; level < 8; ++level) {
		const auto path = directory / relative;
		if (std::filesystem::is_regular_file(path)) return path;
		const auto parent = directory.parent_path();
		if (parent == directory) break;
		directory = parent;
	}
	return {};
}

std::string ReadFileUtf8(const std::filesystem::path& path)
{
	std::ifstream input(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

//! Minimal grammar for the counting/shape tests: everything the scenarios
//! need (`int` keyword, numbers, a multi-line begin/end block comment so end
//! states change, a string rule) at a fraction of the bundled C grammar's
//! per-line cost. One Debug TokenizeLine on a short line is ~6-8 ms with the
//! real C grammar; this one keeps 10,000-line tests inside the CI budget.
const char* const kMinimalGrammarJson = R"json(
{
	"scopeName": "source.minic",
	"patterns": [
		{ "match": "\\bint\\b", "name": "keyword.type.minic" },
		{ "match": "\\b\\d+\\b", "name": "constant.numeric.minic" },
		{ "begin": "/\\*", "end": "\\*/", "name": "comment.block.minic" },
		{ "begin": "\"", "end": "\"", "name": "string.quoted.double.minic" }
	]
}
)json";

std::unique_ptr<textmate::Grammar> LoadMinimalGrammar()
{
	auto result = TextMateJsonGrammarLoader::Load(kMinimalGrammarJson);
	if (!result.Succeeded()) return nullptr;
	return std::move(result.grammar);
}

//! Counting driver: wraps `TextMateTokenizer::TokenizeLine` so tests can
//! measure how many lines a policy re-tokenized.
struct CountingHarness final {
	explicit CountingHarness(const textmate::Grammar& grammar) : tokenizer(grammar) {}

	std::vector<TextMateToken> Tokenize(std::wstring_view text, const RuleStackHandle& prev, RuleStackHandle& next)
	{
		++calls;
		auto result = tokenizer.TokenizeLine(text, prev, next);
		return std::move(result.tokens);
	}

	std::vector<std::wstring> lines;
	TextMateTokenizer tokenizer;
	std::size_t calls = 0;
};

TextMateLineCache::LineTextFn LineText(const std::vector<std::wstring>& lines)
{
	return [&lines](std::size_t i) -> std::wstring_view {
		return i < lines.size() ? std::wstring_view(lines[i]) : std::wstring_view{};
	};
}

TextMateLineCache::TokenizeFn Tokenizer(CountingHarness& harness)
{
	return [&harness](std::wstring_view text, const RuleStackHandle& prev, RuleStackHandle& next) {
		return harness.Tokenize(text, prev, next);
	};
}

std::vector<std::wstring> MakeCLines(std::size_t count)
{
	std::vector<std::wstring> lines;
	lines.reserve(count);
	for (std::size_t i = 0; i < count; ++i) {
		lines.push_back(L"int value_" + std::to_wstring(i) + L" = " + std::to_wstring(i) + L";");
	}
	return lines;
}

//! Empty filler lines for the 10,000-line counting tests: the assertions are
//! call counts and range shapes, not token content, and blank lines cut the
//! per-line allocation cost roughly in half in Debug.
std::vector<std::wstring> MakeBlankLines(std::size_t count)
{
	return std::vector<std::wstring>(count, L"");
}

//! The straightforward top-down tokenization loop, independent of the cache:
//! the from-scratch oracle for the property tests.
std::vector<std::pair<std::vector<TextMateToken>, RuleStackHandle>> TokenizeFromScratch(
	TextMateTokenizer& tokenizer,
	const std::vector<std::wstring>& lines)
{
	std::vector<std::pair<std::vector<TextMateToken>, RuleStackHandle>> result;
	result.reserve(lines.size());
	RuleStackHandle state = tokenizer.InitialState();
	for (const auto& line : lines) {
		RuleStackHandle next;
		auto r = tokenizer.TokenizeLine(line, state, next);
		result.emplace_back(std::move(r.tokens), next);
		state = next;
	}
	return result;
}

bool TokensEqual(const std::vector<TextMateToken>& a, const std::vector<TextMateToken>& b)
{
	if (a.size() != b.size()) return false;
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (a[i].utf16Start != b[i].utf16Start
			|| a[i].utf16End != b[i].utf16End
			|| a[i].scopes != b[i].scopes) return false;
	}
	return true;
}

class TextMateLineCacheTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		// The minimal grammar is self-contained; load it first so a missing
		// file below can never skip a test that does not need the file.
		m_mini = LoadMinimalGrammar();
		ASSERT_NE(nullptr, m_mini);
		const auto path = RepoFilePath(
			L"rust/senp/extensions/sakura_core_language_basics/assets/syntaxes/cpp/c.tmLanguage.json");
		// Never substitute empty data: a missing bundled grammar is a failure.
		ASSERT_FALSE(path.empty()) << "bundled C grammar not found relative to the working directory";
		const auto source = ReadFileUtf8(path);
		ASSERT_FALSE(source.empty()) << "bundled C grammar unreadable: " << path;
		auto result = TextMateJsonGrammarLoader::Load(source);
		ASSERT_TRUE(result.Succeeded()) << "bundled C grammar failed to parse";
		m_grammar = std::move(result.grammar);
	}

	std::unique_ptr<textmate::Grammar> m_grammar; //!< real bundled C grammar (bounded uses only)
	std::unique_ptr<textmate::Grammar> m_mini;    //!< minimal in-test grammar (default)
};

// ---- invalidation / convergence ----

TEST_F(TextMateLineCacheTest, EditAtLine5000RetokenizesOnlyOneLine)
{
	CountingHarness harness(*m_mini);
	harness.lines = MakeBlankLines(10000);

	TextMateLineCache cache;
	cache.Reset(harness.lines.size());
	ASSERT_EQ(cache.EnsureResolved(9999, 100000, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness)).tokenized, 10000u);
	ASSERT_EQ(harness.calls, 10000u);
	const std::size_t baseline = harness.calls; // legacy policy: this many calls after every edit

	// Edit line 5000 without changing its end state.
	harness.lines[5000] = L"int renamed = 1;";
	harness.calls = 0;
	cache.AcceptChange(5000, 1, 1);
	EXPECT_TRUE(cache.IsInvalid(5000));
	EXPECT_EQ(cache.EnsureResolved(9999, 100000, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness)).tokenized, 1u);
	EXPECT_EQ(harness.calls, 1u);
	EXPECT_EQ(cache.FirstInvalidLine(), cache.LineCount());
	EXPECT_LT(harness.calls, baseline);
}


TEST_F(TextMateLineCacheTest, UnclosedBlockCommentDoesNotConvergeFalsely)
{
	CountingHarness harness(*m_mini);
	harness.lines = MakeBlankLines(1000);
	TextMateLineCache cache;
	cache.Reset(harness.lines.size());
	cache.EnsureResolved(999, 100000, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness));

	harness.lines[500] = L"/* unclosed comment start";
	harness.calls = 0;
	cache.AcceptChange(500, 1, 1);
	// The comment opens a frame: every line below has a different end state, so
	// re-tokenization must run past the edit to end of document (no false
	// convergence at line 501).
	auto r = cache.EnsureResolved(999, 100000, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness));
	EXPECT_EQ(harness.calls, 500u);
	EXPECT_EQ(r.firstNewlyInvalidatedLine, std::optional<std::size_t>(501));
	EXPECT_EQ(cache.FirstInvalidLine(), cache.LineCount());

	// Close the comment at 600. Every stored tail state is still the
	// inside-comment state, so the new plain states differ all the way down:
	// VS Code's model re-tokenizes those 400 lines too (their colors really
	// change back).
	harness.lines[600] = L"*/ int after = 2;";
	harness.calls = 0;
	cache.AcceptChange(600, 1, 1);
	cache.EnsureResolved(999, 100000, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness));
	EXPECT_EQ(harness.calls, 400u);
	EXPECT_EQ(cache.FirstInvalidLine(), cache.LineCount());
}

TEST_F(TextMateLineCacheTest, TwoPendingEditsBothRetokenize)
{
	CountingHarness harness(*m_mini);
	harness.lines = MakeBlankLines(10000);
	TextMateLineCache cache;
	cache.Reset(harness.lines.size());
	cache.EnsureResolved(9999, 100000, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness));

	harness.lines[5000] = L"int a = 1;";
	harness.lines[3000] = L"int b = 2;";
	cache.AcceptChange(5000, 1, 1);
	cache.AcceptChange(3000, 1, 1);
	harness.calls = 0;
	cache.EnsureResolved(9999, 100000, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness));
	EXPECT_EQ(harness.calls, 2u);
	EXPECT_FALSE(cache.IsInvalid(3000));
	EXPECT_FALSE(cache.IsInvalid(5000));
	// Line 5000's tokens must be for the NEW text (guards the frontier=size bug).
	const auto* tokens = cache.GetTokens(5000);
	ASSERT_NE(nullptr, tokens);
	ASSERT_FALSE(tokens->empty());
}

TEST_F(TextMateLineCacheTest, MultiLineSameCountReplaceRetokenizesAllLines)
{
	CountingHarness harness(*m_mini);
	harness.lines = MakeCLines(600);
	TextMateLineCache cache;
	cache.Reset(harness.lines.size());
	cache.EnsureResolved(599, 100000, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness));

	harness.lines[500] = L"int x = 1;";
	harness.lines[501] = L"/* opened */ int y = 2;"; // opens AND closes a comment
	harness.lines[502] = L"int z = 3;";
	cache.AcceptChange(500, 3, 3); // delta == 0
	harness.calls = 0;
	cache.EnsureResolved(599, 100000, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness));
	EXPECT_EQ(harness.calls, 3u);
	EXPECT_EQ(cache.FirstInvalidLine(), cache.LineCount());
}

TEST_F(TextMateLineCacheTest, InsertAndDeleteLinesRealignTail)
{
	CountingHarness harness(*m_mini);
	harness.lines = MakeCLines(600);
	TextMateLineCache cache;
	cache.Reset(harness.lines.size());
	cache.EnsureResolved(599, 100000, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness));

	// Insert 3 lines at 400.
	harness.lines.insert(harness.lines.begin() + 400, { L"int i1 = 0;", L"int i2 = 0;", L"int i3 = 0;" });
	cache.AcceptChange(400, 0, 3);
	EXPECT_EQ(cache.LineCount(), 603u);
	harness.calls = 0;
	cache.EnsureResolved(602, 100000, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness));
	// 3 new lines + the first old tail line proving convergence.
	EXPECT_EQ(harness.calls, 4u);
	EXPECT_EQ(cache.FirstInvalidLine(), cache.LineCount());

	// Delete them again. The first shifted-up tail line is re-validated once
	// and the stored state proves convergence for everything below.
	harness.lines.erase(harness.lines.begin() + 400, harness.lines.begin() + 403);
	cache.AcceptChange(400, 3, 0);
	EXPECT_EQ(cache.LineCount(), 600u);
	harness.calls = 0;
	cache.EnsureResolved(599, 100000, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness));
	EXPECT_EQ(harness.calls, 1u);
	EXPECT_EQ(cache.FirstInvalidLine(), cache.LineCount());
}

TEST_F(TextMateLineCacheTest, InvalidLineKeepsPaintingOldTokens)
{
	CountingHarness harness(*m_mini);
	harness.lines = MakeCLines(460);
	TextMateLineCache cache;
	cache.Reset(harness.lines.size());
	cache.EnsureResolved(459, 100000, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness));

	const auto* before = cache.GetTokens(451);
	ASSERT_NE(nullptr, before);
	const auto snapshot = *before;

	// An unclosed comment at 100 propagates invalidity as resolution advances:
	// stop at 450 so 451 is marked invalid but not yet re-tokenized.
	harness.lines[100] = L"/* unclosed";
	cache.AcceptChange(100, 1, 1);
	cache.EnsureResolved(450, 100000, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness));
	EXPECT_TRUE(cache.IsInvalid(451));
	const auto* after = cache.GetTokens(451);
	ASSERT_NE(nullptr, after);
	EXPECT_EQ(snapshot.size(), after->size());
	// Never-tokenized lines still return nothing.
	TextMateLineCache fresh;
	fresh.Reset(10);
	EXPECT_EQ(nullptr, fresh.GetTokens(3));
}

TEST_F(TextMateLineCacheTest, BudgetSplitsResolutionAcrossCalls)
{
	CountingHarness harness(*m_mini);
	harness.lines = MakeCLines(300);
	TextMateLineCache cache;
	cache.Reset(harness.lines.size());
	EXPECT_EQ(cache.EnsureResolved(299, 128, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness)).tokenized, 128u);
	EXPECT_LT(cache.FirstInvalidLine(), cache.LineCount());
	EXPECT_EQ(cache.EnsureResolved(299, 100000, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness)).tokenized, 172u);
	EXPECT_EQ(cache.FirstInvalidLine(), cache.LineCount());
}

TEST_F(TextMateLineCacheTest, TypingInsideLineLeavesNextLineValid)
{
	// R8: a 1->1 in-line edit whose end state is unchanged must tokenize
	// exactly one line; the next line keeps its valid flag.
	CountingHarness harness(*m_mini);
	harness.lines = MakeCLines(300);
	TextMateLineCache cache;
	cache.Reset(harness.lines.size());
	cache.EnsureResolved(299, 100000, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness));

	harness.lines[200] = L"int value_200 = 777;";
	harness.calls = 0;
	cache.AcceptChange(200, 1, 1);
	cache.EnsureResolved(299, 100000, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness));
	EXPECT_EQ(harness.calls, 1u);
	EXPECT_FALSE(cache.IsInvalid(201));
}

TEST_F(TextMateLineCacheTest, CommentOpenMarksNextLineInvalidAndRepaintReported)
{
	// R8/R14: typing `/*` changes the line's end state, so the next line is
	// invalidated in the same call and EnsureResolved reports the transition.
	CountingHarness harness(*m_mini);
	harness.lines = MakeCLines(300);
	TextMateLineCache cache;
	cache.Reset(harness.lines.size());
	cache.EnsureResolved(299, 100000, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness));

	harness.lines[200] = L"/* comment begins";
	harness.calls = 0;
	cache.AcceptChange(200, 1, 1);
	auto r = cache.EnsureResolved(200, 100000, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness));
	EXPECT_EQ(harness.calls, 1u);
	EXPECT_TRUE(cache.IsInvalid(201));
	EXPECT_EQ(r.firstNewlyInvalidatedLine, std::optional<std::size_t>(201));
}

// ---- ApplyEdits ----

struct SimpleEdit {
	bool full = false;
	CLogicInt firstLine = CLogicInt(0);
	CLogicInt oldLineCount = CLogicInt(0);
	CLogicInt newLineCount = CLogicInt(0);
};

TEST(TextMateLineCacheEdits, FullRecordResetsToCurrentLineCount)
{
	TextMateLineCache cache;
	cache.Reset(10);
	std::vector<SimpleEdit> edits{ SimpleEdit{false, CLogicInt(2), CLogicInt(1), CLogicInt(1)},
		SimpleEdit{true, CLogicInt(0), CLogicInt(0), CLogicInt(0)} };
	// A full record discards earlier records and resizes to the real document.
	cache.ApplyEdits(edits, 7);
	EXPECT_EQ(cache.LineCount(), 7u);
	EXPECT_EQ(cache.FirstInvalidLine(), 0u);
}

// ---- F1/F2: invalid-range bookkeeping ----

//! Public invariant: a non-invalid line must be fully resolved -- it has
//! tokens and a stored end state. `FirstInvalidLine` never exceeds the size.
void AssertCacheInvariants(const TextMateLineCache& cache)
{
	EXPECT_LE(cache.FirstInvalidLine(), cache.LineCount());
	for (std::size_t i = 0; i < cache.LineCount(); ++i) {
		if (!cache.IsInvalid(i)) {
			EXPECT_TRUE(cache.HasTokens(i)) << "valid line without tokens: " << i;
			EXPECT_TRUE(cache.Entry(i).stateAfter != nullptr)
				<< "valid line without stateAfter: " << i;
		}
	}
}

// Reviewer trace: a pending range ending exactly at the newly marked line
// must merge, not be dropped. Unfixed code loses lines 3-4: they keep
// hasTokens == false while IsInvalid reports false forever.
TEST_F(TextMateLineCacheTest, PendingRangeBeforeNewMarkIsMergedNotDropped)
{
	CountingHarness harness(*m_mini);
	harness.lines = MakeCLines(10);
	TextMateLineCache cache;
	cache.Reset(10);
	cache.EnsureResolved(2, 100000, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness));
	// invalid == { [3,10) }
	cache.AcceptChange(5, 1, 1); // -> { [3,5), [6,10) }
	cache.AcceptChange(5, 1, 1); // marks [5,6) -- must merge back to { [3,10) }
	EXPECT_TRUE(cache.IsInvalid(3));
	EXPECT_TRUE(cache.IsInvalid(4));
	EXPECT_TRUE(cache.IsInvalid(5));
	AssertCacheInvariants(cache);
	cache.EnsureResolved(9, 100000, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness));
	EXPECT_EQ(cache.FirstInvalidLine(), cache.LineCount());
	const auto oracle = TokenizeFromScratch(harness.tokenizer, harness.lines);
	for (std::size_t i = 0; i < 10; ++i) {
		ASSERT_NE(nullptr, cache.GetTokens(i));
		EXPECT_TRUE(TokensEqual(*cache.GetTokens(i), oracle[i].first)) << i;
		EXPECT_TRUE(textmate::Equivalent(cache.Entry(i).stateAfter, oracle[i].second)) << i;
	}
}

// Range shapes through the public API: adjacent before, adjacent after,
// contained, and a mark spanning two disjoint ranges.
TEST_F(TextMateLineCacheTest, InvalidRangeMergeShapes)
{
	CountingHarness harness(*m_mini);
	harness.lines = MakeCLines(10);
	TextMateLineCache cache;
	cache.Reset(10);
	cache.EnsureResolved(9, 100000, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness));

	cache.AcceptChange(2, 1, 1); // {2}
	cache.AcceptChange(5, 1, 1); // {2},{5}
	cache.AcceptChange(8, 1, 1); // {2},{5},{8}
	EXPECT_TRUE(cache.IsInvalid(2));
	EXPECT_FALSE(cache.IsInvalid(3));
	EXPECT_TRUE(cache.IsInvalid(5));
	EXPECT_TRUE(cache.IsInvalid(8));

	// adjacent before: mark {4} merges with {5} -> {4,5}
	cache.AcceptChange(4, 1, 1);
	EXPECT_TRUE(cache.IsInvalid(4));
	EXPECT_TRUE(cache.IsInvalid(5));

	// bridge: mark {3} touches {2} and {4,5} -> {2..5}
	cache.AcceptChange(3, 1, 1);
	for (std::size_t i = 2; i <= 5; ++i) EXPECT_TRUE(cache.IsInvalid(i)) << i;

	// contained: marking inside {2..5} keeps the range
	cache.AcceptChange(4, 1, 1);
	for (std::size_t i = 2; i <= 5; ++i) EXPECT_TRUE(cache.IsInvalid(i)) << i;
	EXPECT_EQ(cache.FirstInvalidLine(), 2u);

	// adjacent after: mark {9} merges with {8} -> {8,9}
	cache.AcceptChange(9, 1, 1);
	EXPECT_TRUE(cache.IsInvalid(8));
	EXPECT_TRUE(cache.IsInvalid(9));

	// span two ranges: {6} then {7} joins {2..5} and {8,9} -> {2..9}
	cache.AcceptChange(6, 1, 1);
	cache.AcceptChange(7, 1, 1);
	for (std::size_t i = 2; i <= 9; ++i) EXPECT_TRUE(cache.IsInvalid(i)) << i;
	EXPECT_EQ(cache.FirstInvalidLine(), 2u);
	AssertCacheInvariants(cache);

	cache.EnsureResolved(9, 100000, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness));
	EXPECT_EQ(cache.FirstInvalidLine(), cache.LineCount());
	const auto oracle = TokenizeFromScratch(harness.tokenizer, harness.lines);
	for (std::size_t i = 0; i < 10; ++i) {
		ASSERT_NE(nullptr, cache.GetTokens(i));
		EXPECT_TRUE(TokensEqual(*cache.GetTokens(i), oracle[i].first)) << i;
	}
}

TEST(TextMateLineCacheEdits, InconsistentDeltaResetsAndFailsClosed)
{
	TextMateLineCache cache;
	cache.Reset(10);
	// The records' net delta (+5) does not match the real line count (10->4),
	// so the cache cannot reconstruct: it resets to the reported count.
	std::vector<SimpleEdit> edits{ SimpleEdit{false, CLogicInt(1), CLogicInt(0), CLogicInt(5)} };
	cache.ApplyEdits(edits, 4);
	EXPECT_EQ(cache.LineCount(), 4u);
	EXPECT_EQ(cache.FirstInvalidLine(), 0u);
}

// ---- Equivalent ----

TEST(TextMateStateEquivalent, IndependentlyBuiltSameContextAreEqual)
{
	static textmate::TextMateRule ruleStub;
	auto makeFrame = [] {
		auto frame = std::make_shared<textmate::TextMateRuleStackFrame>();
		frame->rule = &ruleStub;
		frame->name = L"comment.block.c";
		frame->endOrWhileSource = L"\\*/";
		return frame;
	};
	auto root1 = std::make_shared<textmate::TextMateRuleStackFrame>();
	auto root2 = std::make_shared<textmate::TextMateRuleStackFrame>();
	auto f1 = makeFrame(); f1->parent = root1;
	auto f2 = makeFrame(); f2->parent = root2;
	EXPECT_TRUE(textmate::Equivalent(f1, f2));
}

TEST(TextMateStateEquivalent, DifferentEndSourceDiffers)
{
	static textmate::TextMateRule ruleStub;
	auto mk = [](std::wstring endSource) {
		auto f = std::make_shared<textmate::TextMateRuleStackFrame>();
		f->rule = &ruleStub; f->endOrWhileSource = std::move(endSource);
		return f;
	};
	EXPECT_FALSE(textmate::Equivalent(mk(L"<<-EOF"), mk(L"<<-END"))); // two heredocs
}

TEST(TextMateStateEquivalent, DifferentNameOrParentDiffers)
{
	static textmate::TextMateRule ruleStub;
	auto mk = [] {
		auto f = std::make_shared<textmate::TextMateRuleStackFrame>();
		f->rule = &ruleStub; f->name = L"n"; return f;
	};
	auto a = mk(); auto b = mk();
	EXPECT_TRUE(textmate::Equivalent(a, b));
	b->name = L"other";
	EXPECT_FALSE(textmate::Equivalent(a, b));
	auto c = mk(); c->parent = a;
	EXPECT_FALSE(textmate::Equivalent(a, c));
}

// M1: every compared field distinguishes frames. Two frames equal in all
// fields except the named one must compare unequal -- killing mutants that
// drop any single comparison in `Equivalent`.
TEST(TextMateStateEquivalent, EachComparedFieldDistinguishes)
{
	static textmate::TextMateRule ruleA, ruleB;
	static textmate::Grammar grammarA, grammarB;
	auto mk = [] {
		auto f = std::make_shared<textmate::TextMateRuleStackFrame>();
		f->rule = &ruleA;
		f->owningGrammar = &grammarA;
		f->name = L"same.name";
		f->contentName = L"same.content";
		f->isWhileRule = false;
		f->endOrWhileSource = L"same-end";
		return f;
	};
	const auto base = mk();
	EXPECT_TRUE(textmate::Equivalent(base, mk())); // control: all fields equal

	{
		auto f = mk(); f->rule = &ruleB;
		EXPECT_FALSE(textmate::Equivalent(base, f)) << "field: rule";
	}
	{
		auto f = mk(); f->owningGrammar = &grammarB;
		EXPECT_FALSE(textmate::Equivalent(base, f)) << "field: owningGrammar";
	}
	{
		auto f = mk(); f->isWhileRule = true;
		EXPECT_FALSE(textmate::Equivalent(base, f)) << "field: isWhileRule";
	}
	{
		auto f = mk(); f->contentName = L"other.content";
		EXPECT_FALSE(textmate::Equivalent(base, f)) << "field: contentName";
	}
}

// ---- ApplyEdits `full` record ----

// M2: a `full` record must reset even when the net delta happens to match --
// here there is no delta at all, so only `full` handling can explain the
// reset. A mutant that ignores `full` would keep every line tokenized.
TEST(TextMateLineCacheEdits, FullRecordResetsWithoutLineCountChange)
{
	TextMateLineCache cache;
	cache.Reset(10);
	auto mini = LoadMinimalGrammar();
	ASSERT_NE(nullptr, mini);
	TextMateTokenizer tokenizer(*mini);
	std::vector<std::wstring> lines = MakeBlankLines(10);
	auto lineText = LineText(lines);
	auto tokenize = [&tokenizer](std::wstring_view text, const RuleStackHandle& prev,
		RuleStackHandle& next) {
		auto r = tokenizer.TokenizeLine(text, prev, next);
		return std::move(r.tokens);
	};
	cache.EnsureResolved(9, 100000, tokenizer.InitialState(), lineText, tokenize);
	ASSERT_EQ(cache.FirstInvalidLine(), 10u); // fully resolved

	std::vector<SimpleEdit> edits{ SimpleEdit{true, CLogicInt(0), CLogicInt(0), CLogicInt(0)} };
	cache.ApplyEdits(edits, 10); // same line count -- delta cannot hide `full`

	EXPECT_EQ(cache.FirstInvalidLine(), 0u);
	EXPECT_TRUE(cache.IsInvalid(9));
	EXPECT_FALSE(cache.HasTokens(9)); // Reset drops stored tokens
}

// ---- R5 end-to-end: real ReplaceData -> CollectTextEdits -> ApplyEdits ----

namespace {

void FillDoc(CDocLineMgr& mgr, const std::vector<std::wstring>& lines)
{
	for (const auto& text : lines) {
		CDocLine* line = mgr.AddNewLine();
		line->SetDocLineString(text.c_str(), static_cast<int>(text.size()), false);
	}
}

void Replace(CDocLineMgr& mgr, CLogicPoint from, CLogicPoint to,
	std::initializer_list<const wchar_t*> insLines)
{
	COpeLineData ins;
	for (const wchar_t* s : insLines) {
		CLineData line;
		line.cmemLine = s;
		line.nSeq = 1;
		ins.push_back(line);
	}
	DocLineReplaceArg arg{};
	arg.sDelRange = CLogicRange(from, to);
	arg.pcmemDeleted = nullptr;
	arg.pInsData = ins.empty() ? nullptr : &ins;
	arg.nDelSeq = 1;
	arg.nInsSeq = -1;
	CSearchAgent(&mgr).ReplaceData(&arg, false);
}

std::vector<std::wstring> DocLines(const CDocLineMgr& mgr)
{
	std::vector<std::wstring> out;
	for (int i = 0; i < mgr.GetLineCount(); ++i) {
		const CDocLine* line = mgr.GetLine(CLogicInt(i));
		out.emplace_back(line->GetPtr(), static_cast<size_t>(static_cast<int>(line->GetLengthWithoutEOL())));
	}
	return out;
}

} // namespace

TEST_F(TextMateLineCacheTest, ApplyEditsFromRealReplaceDataMarksExpectedLines)
{
	CountingHarness harness(*m_mini);
	CDocLineMgr mgr;
	FillDoc(mgr, MakeCLines(50));
	harness.lines = DocLines(mgr);

	TextMateLineCache cache;
	cache.Reset(harness.lines.size());
	cache.EnsureResolved(49, 100000, harness.tokenizer.InitialState(),
		LineText(harness.lines), Tokenizer(harness));
	EXPECT_EQ(cache.FirstInvalidLine(), cache.LineCount());

	// A real in-line ReplaceData on line 20: records one precise edit, applies
	// it, and only line 20 becomes invalid.
	const std::uint64_t seen = mgr.GetTextRevision();
	Replace(mgr, CLogicPoint(0, 20), CLogicPoint(3, 20), { L"int edited_20 = 5" });
	harness.lines = DocLines(mgr);
	std::vector<DocTextEdit> edits;
	ASSERT_TRUE(mgr.CollectTextEdits(seen, edits));
	ASSERT_EQ(edits.size(), 1u);
	EXPECT_EQ(static_cast<int>(edits[0].firstLine), 20);
	EXPECT_EQ(static_cast<int>(edits[0].oldLineCount), 1);
	EXPECT_EQ(static_cast<int>(edits[0].newLineCount), 1);
	EXPECT_FALSE(edits[0].full);
	cache.ApplyEdits(edits, harness.lines.size());
	EXPECT_EQ(cache.LineCount(), harness.lines.size());
	EXPECT_TRUE(cache.IsInvalid(20));
	EXPECT_FALSE(cache.IsInvalid(19));
	EXPECT_FALSE(cache.IsInvalid(21));
}

// ---- R13 property test: seeded random real edits vs from-scratch oracle ----

//! Shared body of the property test: `iterations` seeded random real
//! ReplaceData edits replayed through CollectTextEdits -> ApplyEdits ->
//! EnsureResolved, checked against a from-scratch oracle. Used with both the
//! minimal grammar (full iteration count) and the real C grammar (bounded).
void RunRandomRealEdits(const textmate::Grammar& grammar, int iterations)
{
	for (std::uint32_t kSeed : { 20261018u, 1u, 0xC0FFEEu }) {
	SCOPED_TRACE(::testing::Message() << "seed=" << kSeed);
	std::mt19937 rng(kSeed);

	CDocLineMgr mgr;
	FillDoc(mgr, MakeCLines(16));
	std::vector<std::wstring> lines = DocLines(mgr);
	TextMateTokenizer tokenizer(grammar);

	TextMateLineCache cache;
	cache.Reset(lines.size());
	auto lineText = [&lines](std::size_t i) -> std::wstring_view {
		return i < lines.size() ? std::wstring_view(lines[i]) : std::wstring_view{};
	};
	auto tokenize = [&tokenizer](std::wstring_view text, const RuleStackHandle& prev, RuleStackHandle& next) {
		auto result = tokenizer.TokenizeLine(text, prev, next);
		return std::move(result.tokens);
	};
	cache.EnsureResolved(lines.size() - 1, 100000, tokenizer.InitialState(), lineText, tokenize);

	std::uint64_t seen = mgr.GetTextRevision();
	auto lineLen = [&](CLogicInt y) {
		const CDocLine* l = mgr.GetLine(y);
		return l == nullptr ? 0 : static_cast<int>(l->GetLengthWithoutEOL());
	};

	for (int iteration = 0; iteration < iterations; ++iteration) {
		SCOPED_TRACE(::testing::Message() << "iter=" << iteration);
		const int count = static_cast<int>(mgr.GetLineCount());
		ASSERT_GT(count, 0);
		std::uniform_int_distribution<int> pickLine(0, count); // includes phantom line
		const CLogicInt y(pickLine(rng));
		const int kind = static_cast<int>(rng() % 8);
		const int len = lineLen(y);

		switch (kind) {
		case 0: { // in-line insert of short text (bInsOneLine exit)
			const CLogicInt x(len == 0 ? 0 : static_cast<int>(rng() % (len + 1)));
			Replace(mgr, CLogicPoint(x, y), CLogicPoint(x, y), { L"zz" });
			break;
		}
		case 1: { // in-line delete only (delete-only exit)
			if (len < 2) { Replace(mgr, CLogicPoint(0, y), CLogicPoint(0, y), { L"q" }); break; }
			const int x1 = static_cast<int>(rng() % len);
			const int x2 = x1 + 1 + static_cast<int>(rng() % (len - x1));
			Replace(mgr, CLogicPoint(x1, y), CLogicPoint(x2, y), {});
			break;
		}
		case 2: { // delete this line's EOL -> merges the next line
			if (static_cast<int>(y) >= count - 1) { Replace(mgr, CLogicPoint(0, y), CLogicPoint(0, y), { L"t" }); break; }
			const int eolLen = static_cast<int>(mgr.GetLine(y)->GetLengthWithEOL()) - len;
			Replace(mgr, CLogicPoint(len, y), CLogicPoint(len + eolLen, y), {});
			break;
		}
		case 3: { // multi-line delete spanning up to 3 lines
			if (count < 2) { Replace(mgr, CLogicPoint(0, y), CLogicPoint(0, y), { L"s" }); break; }
			const int y2 = std::max(static_cast<int>(y),
				std::min(count - 1, static_cast<int>(y) + static_cast<int>(rng() % 3)));
			const int l2 = lineLen(CLogicInt(y2));
			Replace(mgr, CLogicPoint(0, y), CLogicPoint(l2, CLogicInt(y2)), {});
			break;
		}
		case 4: { // insert a whole line plus mid-line text (multi-line insert)
			Replace(mgr, CLogicPoint(0, y), CLogicPoint(0, y), { L"int ins = 1;\n", L"int tail = 2" });
			break;
		}
		case 5: { // open a comment
			const CLogicInt x(len == 0 ? 0 : static_cast<int>(rng() % (len + 1)));
			Replace(mgr, CLogicPoint(x, y), CLogicPoint(x, y), { L"/*" });
			break;
		}
		case 6: { // close a comment
			const CLogicInt x(len == 0 ? 0 : static_cast<int>(rng() % (len + 1)));
			Replace(mgr, CLogicPoint(x, y), CLogicPoint(x, y), { L"*/" });
			break;
		}
		default: { // replace a one- to two-line range with fresh text
			const int y2 = std::max(static_cast<int>(y),
				std::min(count - 1, static_cast<int>(y) + static_cast<int>(rng() % 2)));
			const int l2 = lineLen(CLogicInt(y2));
			const CLogicInt toX = (static_cast<int>(y2) == static_cast<int>(y)) ? CLogicInt(std::min(l2, len)) : CLogicInt(l2);
			Replace(mgr, CLogicPoint(0, y), CLogicPoint(toX, CLogicInt(y2)),
				{ L"int rep = 3;\n", L"int rep2 = 4" });
			break;
		}
		}

		// Keep the document from shrinking to zero lines.
		if (mgr.GetLineCount() == 0) {
			Replace(mgr, CLogicPoint(0, 0), CLogicPoint(0, 0), { L"int restart = 0" });
		}

		std::vector<DocTextEdit> edits;
		ASSERT_TRUE(mgr.CollectTextEdits(seen, edits)) << "seed=" << kSeed << " iter=" << iteration;
		seen = mgr.GetTextRevision();
		lines = DocLines(mgr);
		cache.ApplyEdits(edits, lines.size());
		ASSERT_EQ(cache.LineCount(), lines.size()) << "seed=" << kSeed << " iter=" << iteration;
		AssertCacheInvariants(cache);
		// Like the view's requested visible line: resolve only up to a random
		// line, so pending ranges stay both above and below the next edit.
		std::uniform_int_distribution<std::size_t> pickUpto(0, lines.size() - 1);
		cache.EnsureResolved(pickUpto(rng), 100000, tokenizer.InitialState(), lineText, tokenize);
		AssertCacheInvariants(cache);

		if (iteration % 10 != 9) continue;
		cache.EnsureResolved(lines.size() - 1, 100000, tokenizer.InitialState(), lineText, tokenize);
		const auto oracle = TokenizeFromScratch(tokenizer, lines);
		ASSERT_EQ(oracle.size(), lines.size());
		for (std::size_t i = 0; i < lines.size(); ++i) {
			const auto& entry = cache.Entry(i);
			const bool stateOk = textmate::Equivalent(entry.stateAfter, oracle[i].second);
			const auto* tokens = cache.GetTokens(i);
			const bool tokensOk = tokens != nullptr && TokensEqual(*tokens, oracle[i].first);
			if (!stateOk || !tokensOk) {
				std::fprintf(stderr, "FAIL iter=%d line=%zu stateOk=%d tokensOk=%d\n",
					iteration, i, (int)stateOk, (int)tokensOk);
				std::fprintf(stderr, "doc (%d lines):\n", (int)lines.size());
				for (std::size_t j = 0; j < lines.size(); ++j)
					std::fprintf(stderr, "  %3zu: %ls\n", j, lines[j].c_str());
				std::fprintf(stderr, "edits applied this iteration:\n");
				for (const auto& e : edits)
					std::fprintf(stderr, "  rev=%llu full=%d first=%d old=%d new=%d\n",
						(unsigned long long)e.revision, (int)e.full,
						(int)e.firstLine, (int)e.oldLineCount, (int)e.newLineCount);
				std::fprintf(stderr, "cache tokens (%zu):", tokens ? tokens->size() : SIZE_MAX);
				if (tokens) for (const auto& t : *tokens) {
					std::fprintf(stderr, " [%zu,%zu)", t.utf16Start, t.utf16End);
				}
				std::fprintf(stderr, "\noracle tokens (%zu):", oracle[i].first.size());
				for (const auto& t : oracle[i].first)
					std::fprintf(stderr, " [%zu,%zu)", t.utf16Start, t.utf16End);
				std::fprintf(stderr, "\n");
				FAIL();
			}
		}
		EXPECT_EQ(cache.FirstInvalidLine(), cache.LineCount()) << "seed=" << kSeed << " iter=" << iteration;
	}
	// End with the whole document resolved for the next seed.
	}
}

TEST_F(TextMateLineCacheTest, RandomRealEditsMatchFromScratchOracle)
{
	// R13: three fixed seeds, 1000 edits each on the minimal grammar.
	RunRandomRealEdits(*m_mini, 1000);
}

// Same seeds, edit kinds and checks on the real bundled C grammar, but a
// bounded iteration count to keep Debug runtime under ~10 s. The F1
// fail-before fired at seed=20261018 iter=19 -- 40 iterations still covers it.
TEST_F(TextMateLineCacheTest, RandomRealEditsMatchFromScratchOracleRealGrammar)
{
	RunRandomRealEdits(*m_grammar, 40);
}

// Same property, but resolution advances under a budget of 1 between edits so
// pending invalid ranges from several edits overlap before they resolve.
TEST_F(TextMateLineCacheTest, RandomRealEditsWithBudgetOneMatchOracle)
{
	for (std::uint32_t kSeed : { 20261018u, 1u, 0xC0FFEEu }) {
	SCOPED_TRACE(::testing::Message() << "seed=" << kSeed);
	std::mt19937 rng(kSeed);

	CDocLineMgr mgr;
	FillDoc(mgr, MakeCLines(16));
	std::vector<std::wstring> lines = DocLines(mgr);
	TextMateTokenizer tokenizer(*m_mini);

	TextMateLineCache cache;
	cache.Reset(lines.size());
	auto lineText = [&lines](std::size_t i) -> std::wstring_view {
		return i < lines.size() ? std::wstring_view(lines[i]) : std::wstring_view{};
	};
	auto tokenize = [&tokenizer](std::wstring_view text, const RuleStackHandle& prev, RuleStackHandle& next) {
		auto result = tokenizer.TokenizeLine(text, prev, next);
		return std::move(result.tokens);
	};
	cache.EnsureResolved(lines.size() - 1, 100000, tokenizer.InitialState(), lineText, tokenize);

	std::uint64_t seen = mgr.GetTextRevision();
	for (int iteration = 0; iteration < 200; ++iteration) {
		SCOPED_TRACE(::testing::Message() << "iter=" << iteration);
		const int count = static_cast<int>(mgr.GetLineCount());
		std::uniform_int_distribution<int> pickLine(0, count - 1);
		const CLogicInt y(pickLine(rng));
		const int len = [&] {
			const CDocLine* l = mgr.GetLine(y);
			return l == nullptr ? 0 : static_cast<int>(l->GetLengthWithoutEOL());
		}();
		const CLogicInt x(len == 0 ? 0 : static_cast<int>(rng() % (len + 1)));
		const wchar_t* pieces[] = { L"a", L"/*", L"*/", L"\"", L"xx\n", L"int w = 0" };
		Replace(mgr, CLogicPoint(x, y), CLogicPoint(x, y), { pieces[rng() % 6] });

		std::vector<DocTextEdit> edits;
		ASSERT_TRUE(mgr.CollectTextEdits(seen, edits)) << "seed=" << kSeed << " iter=" << iteration;
		seen = mgr.GetTextRevision();
		lines = DocLines(mgr);
		cache.ApplyEdits(edits, lines.size());
		ASSERT_EQ(cache.LineCount(), lines.size()) << "seed=" << kSeed << " iter=" << iteration;
		AssertCacheInvariants(cache);
		// Budget 1, several calls, random requested line: pending invalid
		// ranges from multiple edits overlap and resolve out of order.
		std::uniform_int_distribution<std::size_t> pickUpto(0, lines.size() - 1);
		const std::size_t upto = pickUpto(rng);
		for (int k = 0; k < 3; ++k) {
			cache.EnsureResolved(upto, 1, tokenizer.InitialState(), lineText, tokenize);
			AssertCacheInvariants(cache);
		}
		if (iteration % 10 != 9) continue;
		cache.EnsureResolved(lines.size() - 1, 100000, tokenizer.InitialState(), lineText, tokenize);
		const auto oracle = TokenizeFromScratch(tokenizer, lines);
		for (std::size_t i = 0; i < lines.size(); ++i) {
			EXPECT_TRUE(textmate::Equivalent(cache.Entry(i).stateAfter, oracle[i].second))
				<< "seed=" << kSeed << " iter=" << iteration << " line=" << i;
			ASSERT_NE(nullptr, cache.GetTokens(i));
			EXPECT_TRUE(TokensEqual(*cache.GetTokens(i), oracle[i].first))
				<< "seed=" << kSeed << " iter=" << iteration << " line=" << i;
		}
		EXPECT_EQ(cache.FirstInvalidLine(), cache.LineCount())
			<< "seed=" << kSeed << " iter=" << iteration;
	}
	// Final full resolution must still reach the oracle.
	cache.EnsureResolved(lines.size() - 1, 100000, tokenizer.InitialState(), lineText, tokenize);
	const auto oracle = TokenizeFromScratch(tokenizer, lines);
	for (std::size_t i = 0; i < lines.size(); ++i) {
		EXPECT_TRUE(textmate::Equivalent(cache.Entry(i).stateAfter, oracle[i].second))
			<< "seed=" << kSeed << " line=" << i;
		ASSERT_NE(nullptr, cache.GetTokens(i));
		EXPECT_TRUE(TokensEqual(*cache.GetTokens(i), oracle[i].first)) << "seed=" << kSeed << " line=" << i;
	}
	}
}

} // namespace
