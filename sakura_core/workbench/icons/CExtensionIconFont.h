/*! @file
	@brief 拡張機能の contributes.icons を native なアイコンフォントとして登録する
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_CEXTENSIONICONFONT_2D8B6E3B_7B4B_4C1E_9F2B_2C6C6B7C7B0E_H_
#define SAKURA_CEXTENSIONICONFONT_2D8B6E3B_7B4B_4C1E_9F2B_2C6C6B7C7B0E_H_
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::icons {

/*!
	@brief 拡張機能が contributes.icons で提供する 1 個のアイコンの解決結果

	呼び出し側（例: CMainStatusBar）はこの構造体を直接 LOGFONTW / DrawTextW に
	渡すだけでよい。フォントの実体・生存管理はすべて CExtensionIconFontRegistry
	側が担う。
*/
struct SExtensionContributedIcon {
	std::wstring faceName;   //!< LOGFONTW::lfFaceName に入れる書体名
	std::wstring glyph;      //!< DrawTextW に渡す文字列（BMP 外はサロゲートペア）
};

//! WOFF1/sfnt デコード・fontCharacter 解析・パス検証の内部実装。
//! CExtensionIconFontRegistry 自体の公開面は狭く保つが、この下回りのロジックは
//! ユニットテストから直接検証できるようにするため、型付きの失敗理由を伴って
//! ここに公開する（実装は CExtensionIconFont.cpp）。
namespace detail {

//! フォントファイル（WOFF1/TTF/OTF/TTC）を sfnt へ正規化する際の失敗理由
enum class EFontDecodeError {
	None,                       //!< 成功
	FileEmpty,                  //!< 入力が空
	FileTooLarge,                //!< kMaxFontFileBytes を超える
	HeaderTooShort,              //!< 先頭マジックすら読めない
	UnsupportedWoff2,            //!< WOFF2 (wOF2)。WOFF1 とは区別できる専用の理由として扱う
	UnrecognizedFormat,          //!< wOFF/wOF2/sfnt/ttcf のいずれのマジックでもない
	MalformedHeader,             //!< WOFF ヘッダーやテーブルディレクトリの各フィールドが読めない
	TableCountOutOfRange,        //!< numTables が 0、または上限を超える
	TableOutOfRange,             //!< テーブルの offset/length が入力バッファの範囲外
	TableTooLarge,                //!< 1 テーブルの origLength が上限を超える
	ReconstructedFontTooLarge,   //!< 再構築後の sfnt 全体サイズが上限を超える
	ZlibInflateFailed,            //!< CZipFile::InflateZlibStream が失敗した
};

//! WOFF1/TTF/OTF/TTC の生バイト列を sfnt（TrueType/OpenType の物理形式）へ正規化する。
//! WOFF1 は展開・再構築する。TTF/OTF/TTC はマジックだけ検証してそのまま返す。
//! WOFF2 (wOF2) は明示的に非対応として区別できる理由で失敗する（デコードは試みない）。
[[nodiscard]] EFontDecodeError LoadFontAsSfnt(
	std::span<const std::byte> rawFileBytes,
	std::vector<std::byte>& outSfntBytes);

//! EnsureUniqueFontIdentifier の結果
enum class ENameRepairResult {
	NotNeeded, //!< nameID 3 が既にある。バッファは変更していない
	Repaired,  //!< nameID 3 を合成し、sfnt を組み直した
	Skipped,   //!< TTC や解析できない name テーブルなど。バッファは変更していない
};

/*!
	@brief GDI が要求する nameID 3（Unique font identifier）を必要なら合成する

	`AddFontMemResourceEx` は name テーブルに nameID 3 のレコードが 1 件も無い
	sfnt を無条件で拒否する（このマシンの Windows 11 26200 で実測: nameID 1/2/4/6
	だけのフォントは handle=0、nameID 3 を 1 件足しただけで登録が成功する）。
	一方で Chromium は同じフォントを問題なく描くため、実 VS Code では表示できて
	この製品でだけアイコンが出ない、という非互換になる。IcoMoon 等が生成する
	アイコンフォント（例: odangoo.otak-usage の otak-usage-icons.woff）は
	nameID 1/2/4/6 しか持たないので、これは例外ではなく普通に踏む経路である。

	合成する値はそのフォント自身が名乗っている名前（nameID 1 → 6 → 4、Windows
	レコード優先）の複製に限り、こちらで文字列を作り出すことはしない。名乗る
	名前が 1 つも無い、TTC である、name テーブルが解析できない場合は
	`Skipped` を返して**バッファを一切変更しない**（fail closed のまま
	`AddFontMemResourceEx` の判断に委ねる）。

	@note 合成した場合はテーブル配置が変わるため、ディレクトリ・各テーブルの
	      4 バイト整列・name のチェックサム・head.checkSumAdjustment を作り直す。
*/
[[nodiscard]] ENameRepairResult EnsureUniqueFontIdentifier(std::vector<std::byte>& sfntBytes);

//! sfnt の name テーブルからファミリー名を取り出す際の失敗理由
enum class EFamilyNameError {
	None,                //!< 成功
	TableNotFound,       //!< name テーブルが見つからない
	MalformedNameTable,  //!< name テーブルの構造が壊れている
	NameMissing,         //!< 使えるレコードが 1 つもない
	NameTooLong,         //!< LF_FACESIZE - 1 文字を超える（黙って切り詰めない）
};

/*!
	@brief sfnt の name テーブルからフォントファミリー名を抽出する

	優先順位は platformID=3(Windows)/encodingID=1(Unicode BMP)/nameID=16
	（Typographic Family）→ 同 nameID=1（Font Family）→ platformID=1
	（Macintosh、Latin-1 として単純に decode）。
*/
[[nodiscard]] EFamilyNameError ExtractFamilyName(
	std::span<const std::byte> sfntBytes,
	std::wstring& outFamilyName);

//! fontCharacter 文字列の解析に失敗した理由
enum class EFontCharacterError {
	None,          //!< 成功
	Empty,         //!< 空文字列
	InvalidEscape, //!< "\" の後ろが 1〜6 桁の16進数でない、または不正なコードポイント
	InvalidUtf8,   //!< 単一文字表記のはずが UTF-8 として不正、または2文字以上ある
};

/*!
	@brief fontCharacter の文字列表現を glyph 文字列へ変換する

	"\E901" のようなバックスラッシュ + 16進数表記か、素の単一文字（UTF-8 の
	1 コードポイント分）のどちらかだけを受理する。それ以外は拒否する。
	結果は astral（BMP 外）ならサロゲートペアにして glyph に格納する。
*/
[[nodiscard]] EFontCharacterError ParseFontCharacter(
	std::string_view rawUtf8,
	std::wstring& outGlyph);

/*!
	@brief extensionRoot 配下に収まるように fontPath を解決する

	fontPath が絶対パスであるケース（std::filesystem::path::operator/ は右辺が
	絶対パスだと左辺を丸ごと捨てて置き換えてしまうため、結合前に弾く必要が
	ある）や、正規化した結果が extensionRoot の外を指すケース（".." での
	脱出等）は失敗として拒否する。未信頼な拡張データに対する境界チェックであり、
	weakly_canonical による正規化後の文字列比較で判定する。
*/
[[nodiscard]] bool ResolveExtensionIconFontPath(
	const std::filesystem::path& extensionRoot,
	std::string_view fontPathUtf8,
	std::filesystem::path& outResolved);

//! AddFontMemResourceEx で登録した 1 個のフォントの所有権を表す RAII。
//! non-copyable かつ non-movable（二重 RemoveFontMemResourceEx を避けるため、
//! 移動も明示的に禁止する）。
class CRegisteredMemoryFont {
	using Me = CRegisteredMemoryFont;

public:
	//! sfntBytes を private フォントとして登録する。失敗しても例外は投げず、
	//! IsValid() が false のままになる。
	explicit CRegisteredMemoryFont(std::vector<std::byte> sfntBytes) noexcept;
	~CRegisteredMemoryFont();

	CRegisteredMemoryFont(const Me&) = delete;
	Me& operator=(const Me&) = delete;
	CRegisteredMemoryFont(Me&&) noexcept = delete;
	Me& operator=(Me&&) noexcept = delete;

	[[nodiscard]] bool IsValid() const noexcept { return m_fontResourceHandle != nullptr; }

private:
	// AddFontMemResourceEx はこのバッファをハンドルの生存中ずっと参照し続けるため、
	// RemoveFontMemResourceEx を呼ぶまで解放してはならない。
	std::vector<std::byte> m_fontBytes;
	void* m_fontResourceHandle = nullptr; // 実体は HANDLE（Windows.h への依存をヘッダーに persist させないため void* で保持）
};

} // namespace detail

/*!
	@brief 拡張機能の contributes.icons を native に登録するレジストリ

	実 VS Code の contributes.icons 相当。拡張のマニフェスト（package.json）から
	`contributes.icons.<id>.default.fontPath` / `fontCharacter` を読み、
	WOFF1/TTF/OTF/TTC を sfnt としてプロセス private に登録し、
	(extensionId, iconId) から書体名とグリフ文字列を引けるようにする。

	公開する API はこの 5 メソッドに絞る（narrow）。フォントの読み込み・
	デコード・登録・重複排除といった内部実装はすべて private/PIMPL に隠す。

	Find には名前空間つき（拡張ごと）とグローバル（アイコン id だけ）の
	2 種類がある。実 VS Code の contributes.icons はグローバルな
	IconRegistry に登録されるため、"$(id)" 記法を解決する呼び出し側は
	Find(iconId) 側を使うこと。名前空間つき Find(extensionId, iconId) は
	特定の拡張が実際に何を宣言したかを検査したい呼び出し側のために残す。

	失敗はすべて fail closed: フォントが読めない・登録できない場合は
	Find() が std::nullopt を返すだけで、呼び出し側は自分の既存のフォールバック
	（コード内蔵のアイコン等）を使い続けられる。偽のグリフを作って見た目を
	取り繕うことはしない。

	非対応: WOFF2（wOF2 マジック）、contributes.iconThemes（プロダクトアイコン
	テーマ）。詳細は workbench/icons/CLAUDE.md を参照。
*/
class CExtensionIconFontRegistry {
	using Me = CExtensionIconFontRegistry;

public:
	CExtensionIconFontRegistry();
	~CExtensionIconFontRegistry();

	CExtensionIconFontRegistry(const Me&) = delete;
	Me& operator=(const Me&) = delete;
	CExtensionIconFontRegistry(Me&&) = delete;
	Me& operator=(Me&&) = delete;

	/*!
		@brief 拡張の contributes.icons を読み、フォントを登録する

		同じ extensionId を再登録すると、読み直す前にまず既存分を破棄してから
		登録し直す（フォントハンドルの多重リークを防ぐ）。

		contributes.icons を持たない、または contributes 自体を持たない
		マニフェストは「アイコン 0 件」の成功として扱う（失敗ではない）。
		個々のアイコン定義が壊れている（fontCharacter が不正、fontPath が
		extensionRoot の外を指す、フォントが読めない等）場合は、その
		アイコンだけを黙って登録対象から外し、拡張全体は成功のまま進める。

		@retval false extensionRoot にマニフェスト（package.json）が無い、
			読めない、または JSON として不正な場合のみ
	*/
	bool RegisterExtension(std::wstring_view extensionId, const std::filesystem::path& extensionRoot);

	//! (extensionId, iconId) から書体名とグリフを引く。見つからなければ nullopt
	[[nodiscard]] std::optional<SExtensionContributedIcon> Find(
		std::wstring_view extensionId,
		std::wstring_view iconId) const;

	/*!
		@brief アイコン id だけで引く（実 VS Code の IconRegistry と同じグローバル解決）

		実 VS Code では contributes.icons は拡張ごとに名前空間分離されず、
		`platform/theme/common/iconRegistry.ts` の registerIcon() が単一の
		グローバルレジストリに id だけで登録する。`ThemeIcon.fromString("$(id)")`
		はどの拡張がレンダリングしているかに関わらずこの id を解決するため、
		ある拡張が別の拡張の contributes.icons で宣言されたアイコンを使うのは
		仕様上正当な状態である。StatusBarItem.text の "$(name)" 記法はこの
		グローバル解決を前提にしており、呼び出し側（例: ステータスバー）は
		どの拡張が id を宣言したかを気にせずこちらを呼べばよい。

		id が複数拡張から宣言された場合、実 VS Code の registerIcon() と同じ
		「最初の登録を保持し、以降はエラーとして無視する（上書きしない）」
		挙動に従う。ある拡張の登録が UnregisterExtension/Clear で解除された
		場合、その拡張が保持していた id は自動的に次に古い登録へ引き継がれる
		（他に生きている登録が無ければ nullopt）。

		名前空間つきの Find(extensionId, iconId) はこのグローバル解決とは独立
		した別 API として引き続き提供する。
	*/
	[[nodiscard]] std::optional<SExtensionContributedIcon> Find(std::wstring_view iconId) const;

	//! 指定拡張のフォント登録・アイコンをすべて破棄する
	void UnregisterExtension(std::wstring_view extensionId);

	//! すべての拡張の登録を破棄する
	void Clear();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace workbench::icons

#endif /* SAKURA_CEXTENSIONICONFONT_2D8B6E3B_7B4B_4C1E_9F2B_2C6C6B7C7B0E_H_ */
