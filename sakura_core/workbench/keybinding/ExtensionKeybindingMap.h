/*! @file
	@brief 拡張由来のキーバインドを打鍵へ突き合わせる解決器
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/keybinding/KeybindingChord.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace workbench::keybinding {

//! 解決済みの 1 バインド。キー式は登録時に一度だけ解釈し、打鍵ごとに解釈し直さない。
struct ExtensionKeybinding {
	std::vector<KeyStroke>	sequence;
	std::wstring			commandId;
	std::wstring			whenClause;
	std::string				argumentsJson;		//!< 空なら実行時に `[]` を送る
	std::wstring			extensionId;
};

//! 1 打鍵を食わせた結果、このホストがその打鍵をどう扱うべきか
enum class EKeybindingResolution : std::uint8_t {
	//! 拡張の割り当てではない。ほかのハンドラとアクセラレータへ流す。
	NoMatch,
	//! 2 打鍵バインドの 1 打鍵目。打鍵は消費し、次の打鍵を待つ。
	ChordPending,
	//! 完全一致。打鍵を消費してコマンドを実行する。
	Execute,
	/*!
		@brief 待機中の chord がこの打鍵で崩れた

		打鍵は消費する。VS Code も chord 中の不一致キーはエディタへ落とさない。
		ここで通してしまうと `ctrl+k` の次に押した `x` が本文に入る。
	*/
	ChordCancelled,
};

struct KeybindingResolveResult {
	EKeybindingResolution	resolution = EKeybindingResolution::NoMatch;
	std::wstring			commandId;
	std::string				argumentsJson;
};

/*!
	@brief 拡張のキーバインド表と、その chord 待機状態

	`when` の評価器を外から受け取るので、context key ストアにも HWND にも依存しない。
	組み込みのキー割り当てとの優先順位は**このクラスの外**（呼び出し側）で決める。
	ここは「拡張が何を主張しているか」だけを答え、「誰が勝つか」は答えない。
*/
class ExtensionKeybindingMap final
{
public:
	//! VS Code の chord 待ち時間と同じ 5 秒。
	static constexpr std::uint64_t ChordTimeoutMs = 5000;

	/*!
		@brief 表を丸ごと差し替える

		差分更新にしないのは、拡張の再登録でマニフェストごと入れ替わり得るため。
		同じ打鍵列を複数の拡張が主張した場合は**先に登録された方が勝つ**。
		後勝ちにすると、無関係な拡張を 1 つ入れただけで既存の割り当てが黙って
		すり替わる。解釈できないキー式の項目は呼び出し側で落としてから渡す。
	*/
	void SetBindings(std::vector<ExtensionKeybinding> bindings);

	void Clear() noexcept;
	[[nodiscard]] bool Empty() const noexcept { return m_bindings.empty(); }
	[[nodiscard]] std::size_t Size() const noexcept { return m_bindings.size(); }
	[[nodiscard]] bool IsChordPending() const noexcept { return m_chordPending; }

	//! 待機中の chord を時間切れで捨てる。捨てたら true。
	[[nodiscard]] bool ExpireIfNeeded(std::uint64_t now) noexcept
	{
		if (!m_chordPending || now - m_chordStartedAt < ChordTimeoutMs) return false;
		ClearChord();
		return true;
	}

	void ClearChord() noexcept
	{
		m_chordPending = false;
		m_pendingStroke = {};
		m_chordStartedAt = 0;
	}

	/*!
		@brief 1 打鍵を解決する

		`evaluateWhen` は `when` 節 1 本を受け取り真偽を返す。空の節は真として
		呼び出し側に渡す（VS Code の「節が無ければ常に有効」と同じ）。
	*/
	template <class EvaluateWhen>
	[[nodiscard]] KeybindingResolveResult Resolve(
		const KeyStroke stroke, EvaluateWhen&& evaluateWhen) const
	{
		if (m_bindings.empty()) {
			// 表が空でも chord だけ残っている状態は作らない。
			return {};
		}
		if (m_chordPending) {
			for (const auto& binding : m_bindings) {
				if (binding.sequence.size() != 2) continue;
				if (binding.sequence[0] != m_pendingStroke || binding.sequence[1] != stroke) continue;
				if (!evaluateWhen(binding.whenClause)) continue;
				return { EKeybindingResolution::Execute, binding.commandId, binding.argumentsJson };
			}
			return { EKeybindingResolution::ChordCancelled, {}, {} };
		}

		// 1 打鍵で完結する割り当てを先に見る。同じ打鍵が chord の先頭でもあるとき、
		// VS Code は短い方を優先しない――が、そこは拡張作者が自分で衝突させた場合だけ
		// 起きる。先に決まる方を選び、待ち状態に落ちて無反応になるのを避ける。
		for (const auto& binding : m_bindings) {
			if (binding.sequence.size() != 1 || binding.sequence[0] != stroke) continue;
			if (!evaluateWhen(binding.whenClause)) continue;
			return { EKeybindingResolution::Execute, binding.commandId, binding.argumentsJson };
		}
		for (const auto& binding : m_bindings) {
			if (binding.sequence.size() != 2 || binding.sequence[0] != stroke) continue;
			if (!evaluateWhen(binding.whenClause)) continue;
			return { EKeybindingResolution::ChordPending, {}, {} };
		}
		return {};
	}

	//! `Resolve` が ChordPending を返したときに、待機状態を確定させる。
	void BeginChord(const KeyStroke stroke, const std::uint64_t now) noexcept
	{
		m_chordPending = true;
		m_pendingStroke = stroke;
		m_chordStartedAt = now;
	}

	//! 呼び出し側が「拡張に渡す前に組み込みが勝つか」を判定するために使う。
	[[nodiscard]] const std::vector<ExtensionKeybinding>& Bindings() const noexcept { return m_bindings; }

private:
	std::vector<ExtensionKeybinding>	m_bindings;
	bool								m_chordPending = false;
	KeyStroke							m_pendingStroke;
	std::uint64_t						m_chordStartedAt = 0;
};

} // namespace workbench::keybinding
