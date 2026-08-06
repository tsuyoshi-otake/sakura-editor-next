/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/keybinding/ExtensionKeybindingMap.h"
#include "workbench/keybinding/KeybindingChord.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using workbench::keybinding::EKeybindingResolution;
using workbench::keybinding::ExtensionKeybinding;
using workbench::keybinding::ExtensionKeybindingMap;
using workbench::keybinding::KeyStroke;
using workbench::keybinding::ParseKeybinding;

//! 仮想キーコードは Windows.h の VK_* と同値。テスト側も数値で書いて依存を増やさない。
constexpr std::uint32_t kVkK = 0x4B;
constexpr std::uint32_t kVkP = 0x50;
constexpr std::uint32_t kVkI = 0x49;
constexpr std::uint32_t kVkA = 0x41;
constexpr std::uint32_t kVkF12 = 0x7B;
constexpr std::uint32_t kVkOem1 = 0xBA;

[[nodiscard]] KeyStroke Ctrl(const std::uint32_t virtualKey)
{
	return KeyStroke{ .virtualKey = virtualKey, .control = true };
}

//! `when` を見ない評価器。節そのものの扱いを試さないケースで使う。
[[nodiscard]] auto AlwaysTrue()
{
	return [](std::wstring_view) { return true; };
}

[[nodiscard]] ExtensionKeybinding MakeBinding(
	std::wstring_view expression, std::wstring_view commandId, std::wstring_view whenClause = {})
{
	return ExtensionKeybinding{
		.sequence = ParseKeybinding(expression),
		.commandId = std::wstring(commandId),
		.whenClause = std::wstring(whenClause),
		.argumentsJson = {},
		.extensionId = L"test.extension",
	};
}

} // namespace

/*!
	VS Code の一般的な式が、そのまま打鍵へ落ちること。
	ここが崩れると拡張のキーバインドは「登録されているのに効かない」になる。
*/
TEST(ExtensionKeybindingParse, ReadsASingleStrokeWithItsModifiers)
{
	const auto strokes = ParseKeybinding(L"ctrl+shift+p");
	ASSERT_EQ(1u, strokes.size());
	EXPECT_EQ(kVkP, strokes[0].virtualKey);
	EXPECT_TRUE(strokes[0].control);
	EXPECT_TRUE(strokes[0].shift);
	EXPECT_FALSE(strokes[0].alt);
	EXPECT_FALSE(strokes[0].win);
}

TEST(ExtensionKeybindingParse, ReadsATwoStrokeChord)
{
	const auto strokes = ParseKeybinding(L"ctrl+k ctrl+i");
	ASSERT_EQ(2u, strokes.size());
	EXPECT_EQ(Ctrl(kVkK), strokes[0]);
	EXPECT_EQ(Ctrl(kVkI), strokes[1]);
}

TEST(ExtensionKeybindingParse, IsCaseInsensitiveOverModifiersAndKeyNames)
{
	EXPECT_EQ(ParseKeybinding(L"ctrl+shift+p"), ParseKeybinding(L"CTRL+Shift+P"));
}

TEST(ExtensionKeybindingParse, AcceptsFunctionKeysButRejectsZeroPaddedOnes)
{
	const auto f12 = ParseKeybinding(L"f12");
	ASSERT_EQ(1u, f12.size());
	EXPECT_EQ(kVkF12, f12[0].virtualKey);

	// `f01` は VS Code 自身が受け付けない綴り。ここで通すと F1 と二重登録になる。
	EXPECT_TRUE(ParseKeybinding(L"f01").empty());
	EXPECT_TRUE(ParseKeybinding(L"f25").empty());
}

TEST(ExtensionKeybindingParse, NormalizesScanCodeSpellingsOntoTheSameStroke)
{
	EXPECT_EQ(ParseKeybinding(L"ctrl+a"), ParseKeybinding(L"ctrl+[KeyA]"));

	const auto semicolon = ParseKeybinding(L"ctrl+[Semicolon]");
	ASSERT_EQ(1u, semicolon.size());
	EXPECT_EQ(kVkOem1, semicolon[0].virtualKey);
	EXPECT_EQ(ParseKeybinding(L"ctrl+;"), semicolon);
}

TEST(ExtensionKeybindingParse, TreatsMetaSpellingsAsTheWindowsKey)
{
	const auto strokes = ParseKeybinding(L"cmd+p");
	ASSERT_EQ(1u, strokes.size());
	EXPECT_TRUE(strokes[0].win);
	EXPECT_EQ(ParseKeybinding(L"win+p"), strokes);
	EXPECT_EQ(ParseKeybinding(L"meta+p"), strokes);
}

/*!
	読めない式は「部分的に読めたぶん」ではなく式ごと捨てること。

	`ctrl+k ctrl+bogus` を 1 打鍵の `ctrl+k` として登録してしまうと、拡張が
	意図しないキーを丸ごと占有する。ここは必ず fail-closed でなければならない。
*/
TEST(ExtensionKeybindingParse, DiscardsTheWholeExpressionWhenAnyStrokeIsUnreadable)
{
	EXPECT_TRUE(ParseKeybinding(L"ctrl+k ctrl+bogus").empty());
	EXPECT_TRUE(ParseKeybinding(L"ctrl+nope").empty());
	EXPECT_TRUE(ParseKeybinding(L"ctrl+").empty());
	EXPECT_TRUE(ParseKeybinding(L"").empty());
}

TEST(ExtensionKeybindingParse, RejectsSequencesLongerThanVsCodeAllows)
{
	// VS Code の chord は 2 打鍵まで。3 打鍵目を黙って切り落とすと別物になる。
	EXPECT_TRUE(ParseKeybinding(L"ctrl+a ctrl+b ctrl+c").empty());
}

TEST(ExtensionKeybindingMapTest, DropsBindingsThatCannotEverFire)
{
	ExtensionKeybindingMap map;
	map.SetBindings({
		MakeBinding(L"ctrl+nope", L"test.unreadable"),	// 打鍵列が空
		MakeBinding(L"ctrl+shift+p", L""),				// コマンド ID が空
		MakeBinding(L"ctrl+shift+p", L"test.valid"),
	});
	EXPECT_EQ(1u, map.Size());
	EXPECT_FALSE(map.Empty());
}

/*!
	同じ打鍵列かつ同じ `when` は先勝ち。後から入った拡張に既存の割り当てを
	黙って奪わせない（VS Code の後勝ちとは意図的に違える）。
*/
TEST(ExtensionKeybindingMapTest, KeepsTheFirstOfTwoIdenticalBindings)
{
	ExtensionKeybindingMap map;
	map.SetBindings({
		MakeBinding(L"ctrl+shift+p", L"first.command"),
		MakeBinding(L"ctrl+shift+p", L"second.command"),
	});
	ASSERT_EQ(1u, map.Size());

	const auto resolved = map.Resolve(
		KeyStroke{ .virtualKey = kVkP, .control = true, .shift = true }, AlwaysTrue());
	EXPECT_EQ(EKeybindingResolution::Execute, resolved.resolution);
	EXPECT_EQ(L"first.command", resolved.commandId);
}

TEST(ExtensionKeybindingMapTest, KeepsBothBindingsWhenTheirWhenClausesDiffer)
{
	ExtensionKeybindingMap map;
	map.SetBindings({
		MakeBinding(L"ctrl+shift+p", L"editor.command", L"editorTextFocus"),
		MakeBinding(L"ctrl+shift+p", L"terminal.command", L"terminalFocus"),
	});
	ASSERT_EQ(2u, map.Size());

	const auto resolved = map.Resolve(
		KeyStroke{ .virtualKey = kVkP, .control = true, .shift = true },
		[](std::wstring_view clause) { return clause == L"terminalFocus"; });
	EXPECT_EQ(EKeybindingResolution::Execute, resolved.resolution);
	EXPECT_EQ(L"terminal.command", resolved.commandId);
}

TEST(ExtensionKeybindingMapTest, DoesNotMatchWhenTheClauseIsFalse)
{
	ExtensionKeybindingMap map;
	map.SetBindings({ MakeBinding(L"ctrl+shift+p", L"editor.command", L"editorTextFocus") });

	const auto resolved = map.Resolve(
		KeyStroke{ .virtualKey = kVkP, .control = true, .shift = true },
		[](std::wstring_view) { return false; });
	EXPECT_EQ(EKeybindingResolution::NoMatch, resolved.resolution);
	EXPECT_TRUE(resolved.commandId.empty());
}

TEST(ExtensionKeybindingMapTest, CarriesTheDeclaredArgumentsThroughToTheResult)
{
	auto binding = MakeBinding(L"ctrl+shift+p", L"test.withArgs");
	binding.argumentsJson = R"([{"value":1}])";

	ExtensionKeybindingMap map;
	map.SetBindings({ std::move(binding) });

	const auto resolved = map.Resolve(
		KeyStroke{ .virtualKey = kVkP, .control = true, .shift = true }, AlwaysTrue());
	ASSERT_EQ(EKeybindingResolution::Execute, resolved.resolution);
	EXPECT_EQ(R"([{"value":1}])", resolved.argumentsJson);
}

TEST(ExtensionKeybindingMapTest, HoldsTheFirstStrokeOfAChordAndFiresOnTheSecond)
{
	ExtensionKeybindingMap map;
	map.SetBindings({ MakeBinding(L"ctrl+k ctrl+i", L"test.chord") });

	const auto first = map.Resolve(Ctrl(kVkK), AlwaysTrue());
	ASSERT_EQ(EKeybindingResolution::ChordPending, first.resolution);
	EXPECT_TRUE(first.commandId.empty());

	map.BeginChord(Ctrl(kVkK), 1000);
	EXPECT_TRUE(map.IsChordPending());

	const auto second = map.Resolve(Ctrl(kVkI), AlwaysTrue());
	EXPECT_EQ(EKeybindingResolution::Execute, second.resolution);
	EXPECT_EQ(L"test.chord", second.commandId);
}

/*!
	待機中に一致しない打鍵が来たら、その打鍵は捨てて chord を解く。
	素通しすると `ctrl+k` の後の無関係なキーが本文へ入る。
*/
TEST(ExtensionKeybindingMapTest, CancelsThePendingChordOnAnUnrelatedSecondStroke)
{
	ExtensionKeybindingMap map;
	map.SetBindings({ MakeBinding(L"ctrl+k ctrl+i", L"test.chord") });
	map.BeginChord(Ctrl(kVkK), 1000);

	const auto resolved = map.Resolve(Ctrl(kVkA), AlwaysTrue());
	EXPECT_EQ(EKeybindingResolution::ChordCancelled, resolved.resolution);
	EXPECT_TRUE(resolved.commandId.empty());
}

TEST(ExtensionKeybindingMapTest, DoesNotLetASingleStrokeBindingStealAPendingChord)
{
	ExtensionKeybindingMap map;
	map.SetBindings({
		MakeBinding(L"ctrl+k ctrl+i", L"test.chord"),
		MakeBinding(L"ctrl+i", L"test.single"),
	});
	map.BeginChord(Ctrl(kVkK), 1000);

	const auto resolved = map.Resolve(Ctrl(kVkI), AlwaysTrue());
	ASSERT_EQ(EKeybindingResolution::Execute, resolved.resolution);
	EXPECT_EQ(L"test.chord", resolved.commandId);
}

TEST(ExtensionKeybindingMapTest, PrefersASingleStrokeBindingOverStartingAChord)
{
	ExtensionKeybindingMap map;
	map.SetBindings({
		MakeBinding(L"ctrl+k ctrl+i", L"test.chord"),
		MakeBinding(L"ctrl+k", L"test.single"),
	});

	const auto resolved = map.Resolve(Ctrl(kVkK), AlwaysTrue());
	ASSERT_EQ(EKeybindingResolution::Execute, resolved.resolution);
	EXPECT_EQ(L"test.single", resolved.commandId);
}

TEST(ExtensionKeybindingMapTest, ExpiresAPendingChordAfterTheTimeout)
{
	ExtensionKeybindingMap map;
	map.SetBindings({ MakeBinding(L"ctrl+k ctrl+i", L"test.chord") });
	map.BeginChord(Ctrl(kVkK), 1000);

	EXPECT_FALSE(map.ExpireIfNeeded(1000 + ExtensionKeybindingMap::ChordTimeoutMs - 1));
	EXPECT_TRUE(map.IsChordPending());

	EXPECT_TRUE(map.ExpireIfNeeded(1000 + ExtensionKeybindingMap::ChordTimeoutMs));
	EXPECT_FALSE(map.IsChordPending());
	// 期限切れは 1 度だけ報告する。毎回 true を返すと呼び出し側が空振りし続ける。
	EXPECT_FALSE(map.ExpireIfNeeded(1000 + ExtensionKeybindingMap::ChordTimeoutMs));
}

TEST(ExtensionKeybindingMapTest, ForgetsAPendingChordWhenTheTableIsReplaced)
{
	ExtensionKeybindingMap map;
	map.SetBindings({ MakeBinding(L"ctrl+k ctrl+i", L"test.chord") });
	map.BeginChord(Ctrl(kVkK), 1000);
	ASSERT_TRUE(map.IsChordPending());

	// 新しい表に同じ 1 打鍵目がある保証は無い。押しっぱなしの状態を持ち越さない。
	map.SetBindings({ MakeBinding(L"ctrl+shift+p", L"test.other") });
	EXPECT_FALSE(map.IsChordPending());
}

TEST(ExtensionKeybindingMapTest, ResolvesNothingOnceCleared)
{
	ExtensionKeybindingMap map;
	map.SetBindings({ MakeBinding(L"ctrl+shift+p", L"test.command") });
	map.Clear();

	EXPECT_TRUE(map.Empty());
	EXPECT_EQ(EKeybindingResolution::NoMatch,
		map.Resolve(KeyStroke{ .virtualKey = kVkP, .control = true, .shift = true }, AlwaysTrue())
			.resolution);
}

TEST(ExtensionKeybindingMapTest, DistinguishesStrokesThatDifferOnlyByModifier)
{
	ExtensionKeybindingMap map;
	map.SetBindings({ MakeBinding(L"ctrl+shift+p", L"test.command") });

	// Shift 抜きの ctrl+p は別の打鍵。ここが一致すると無関係なキーを奪う。
	EXPECT_EQ(EKeybindingResolution::NoMatch,
		map.Resolve(Ctrl(kVkP), AlwaysTrue()).resolution);
	EXPECT_EQ(EKeybindingResolution::Execute,
		map.Resolve(KeyStroke{ .virtualKey = kVkP, .control = true, .shift = true }, AlwaysTrue())
			.resolution);
}
