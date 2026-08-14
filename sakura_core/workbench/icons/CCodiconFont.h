/*! @file
	@brief 同梱した codicon.ttf をプロセス private フォントとして登録する

	実 VS Code は codicon.ttf を丸ごと同梱し、`$(name)` をその 1 書体のグリフとして
	描く。この製品も同じ経路にする。ベクターパスを 1 つずつ取り込む方式では、
	取り込まなかった名前が別のアイコン（代替の点）になってしまい、「見た目が似ている」
	ではなく「別のアイコンが出る」という非互換になるためである。

	フォントの実体は sakura_rc.rc2 が `CODICONFONT RCDATA` として実行ファイルへ
	埋め込む。外部ファイルにしないのは、配布物から 1 ファイル欠けただけで全ての
	組み込みアイコンが黙って別物になる、という壊れ方を避けるためである。

	登録に失敗した場合は FaceName() が空を返し、呼び出し側は既存のフォールバック
	（CodiconsActivityIcons.h の取り込み済みベクター）へ落ちる。偽のグリフを作って
	見た目を取り繕うことはしない。
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_CCODICONFONT_4F7A9C21_6E58_4B0D_8C33_5A9E2D7B1F64_H_
#define SAKURA_CCODICONFONT_4F7A9C21_6E58_4B0D_8C33_5A9E2D7B1F64_H_
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::icons {

/*!
	@brief 埋め込みリソースの codicon.ttf を 1 回だけ登録して書体名を配るシングルトン

	GDI のフォント登録はプロセス単位の資源なので、所有者もプロセス単位に 1 つで
	よい。`Instance()` は最初の呼び出しで登録を試み、以後は同じ結果を返す。

	@note 書体名は決め打ちせず、登録したフォント自身の name テーブルから取り出す
	      （detail::ExtractFamilyName）。表と実体が食い違ったまま別書体で代替描画
	      されるのを避けるため。
*/
class CCodiconFont {
	using Me = CCodiconFont;

public:
	//! プロセスに 1 つのインスタンス。初回呼び出しでリソース読み込みと登録を行う。
	[[nodiscard]] static const CCodiconFont& Instance() noexcept;

	//! 登録に成功していれば真
	[[nodiscard]] bool IsAvailable() const noexcept { return !m_faceName.empty(); }

	//! 登録できていれば書体名（実測値は L"codicon"）、できていなければ空
	[[nodiscard]] std::wstring_view FaceName() const noexcept { return m_faceName; }

	CCodiconFont(const Me&) = delete;
	Me& operator=(const Me&) = delete;
	CCodiconFont(Me&&) = delete;
	Me& operator=(Me&&) = delete;

private:
	CCodiconFont() noexcept;
	~CCodiconFont();

	std::vector<std::byte> m_fontBytes;
	void* m_fontResourceHandle = nullptr;
	std::wstring m_faceName;
};

} // namespace workbench::icons

#endif /* SAKURA_CCODICONFONT_4F7A9C21_6E58_4B0D_8C33_5A9E2D7B1F64_H_ */
