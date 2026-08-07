/*! @file
	@brief 拡張マニフェストの contribution points を保持する所有権付きモデル
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

/*!
	@brief ビューコンテナの置き場所

	VS Code は `viewsContainers` を
	`{ activitybar: [...], panel: [...], secondarySidebar: [...] }` で受け取る。
	3 つは別の Part なので、どれに属するかは失わせない。ここに無い location は
	VS Code でも contribution エラーとして無視されるため、既定値へ丸めずに落とす。
*/
enum class EExtensionViewContainerLocation : std::uint8_t {
	ActivityBar,
	Panel,
	SecondarySidebar,
};

//! ビューの実体。`"type": "webview"` を落とすとツリーとして登録され、永久に空のまま出る。
enum class EExtensionViewKind : std::uint8_t {
	Tree,
	Webview,
};

//! 所有者。世代（generation）まで含めないと、再起動した拡張ホストの残骸を消せない。
struct SExtensionContributionOwner {
	std::wstring	extensionId;
	std::uint64_t	generation = 0;
};

/*!
	@brief マニフェスト由来のビューコンテナ宣言（転送用）

	identity（ID・表示名・所属エリア・並び・重複拒否・所有者世代）は
	workbench::layout::WorkbenchContributionRegistry が唯一の出所なので、
	このレジストリは保持しない。ディスパッチャがブリッジ経由でそちらへ渡す。
*/
struct SExtensionViewContainerDeclaration {
	std::wstring	id;
	std::wstring	title;
	EExtensionViewContainerLocation location = EExtensionViewContainerLocation::ActivityBar;
};

//! マニフェスト由来のビュー宣言（転送用）。identity の出所は上と同じ。
struct SExtensionViewDeclaration {
	std::wstring	id;
	std::wstring	containerId;
	std::wstring	title;
};

/*!
	@brief レイアウトレジストリが持たない、拡張由来のビュー表示属性

	アイコンや `when`、ツリー/Webview の別はワークベンチのレイアウトモデルの関心事ではない。
	一方でこれらを捨てると Claude Code のアクティビティバー項目は無地になり、
	サイドバーは空のツリーになる。identity と重複させずに横へ持つ。
*/
struct SExtensionViewPresentation {
	//! レイアウトレジストリ側の identity と同じ値（コンテナ ID またはビュー ID）
	std::wstring	id;
	//! 拡張ルート相対を解決した絶対パス。codicon 指定なら空
	std::wstring	iconPath;
	//! `$(name)` 形式で指定された codicon 名。画像指定なら空
	std::wstring	codicon;
	/*!
		@brief ビュー・コンテナ双方の `when` 句

		拡張は排他的な複数コンテナを宣言し、`when` で 1 つだけ見せる
		（Claude Code の Primary/Secondary Side Bar 切り替えなど）。
		コンテナ側を捨てると排他が壊れて全部が同時に並ぶ。
		評価は投影時に行い、登録自体は落とさない（後から context key が
		変わったときにコンテナを出せるようにするため）。
	*/
	std::wstring	whenClause;
	//! ビューのみ。コンテナでは常に空
	std::wstring	contextualTitle;
	//! ビューのみ。コンテナでは既定値のまま
	EExtensionViewKind kind = EExtensionViewKind::Tree;
	std::wstring	extensionId;
	std::uint64_t	generation = 0;
};

/*!
	@brief メニュー 1 項目

	`group` は VS Code では `"navigation@1"` のように「グループ名@並び順」で書かれる。
	描画側が毎回この文字列を解釈し直すのは無駄で、しかも解釈がぶれる。
	登録時に一度だけ分解して持つ。
*/
struct SExtensionMenuItem {
	std::wstring	location;			//!< `editor/title` などのメニュー面
	std::wstring	commandId;
	std::wstring	submenuId;			//!< 入れ子メニューを開く項目のときのみ
	std::wstring	altCommandId;		//!< Alt 押下時に差し替わるコマンド
	std::wstring	whenClause;
	std::wstring	groupName;			//!< `navigation`。無指定なら空
	int				groupOrder = 0;		//!< `@` の後ろの数値。無指定なら 0
	std::wstring	extensionId;
	std::uint64_t	generation = 0;
};

struct SExtensionSubmenuDescriptor {
	std::wstring	id;
	std::wstring	label;
	std::wstring	iconPath;
	std::wstring	extensionId;
	std::uint64_t	generation = 0;
};

struct SExtensionKeybinding {
	std::wstring	commandId;
	std::wstring	keyChord;			//!< `ctrl+shift+p` / `ctrl+k ctrl+i`
	std::wstring	whenClause;
	std::string		argumentsJson;		//!< `args` の生 JSON。無指定なら空
	std::wstring	extensionId;
	std::uint64_t	generation = 0;
};

struct SExtensionLanguageDescriptor {
	std::wstring				id;
	std::vector<std::wstring>	aliases;
	std::vector<std::wstring>	extensions;			//!< `.ts` のように先頭のドットを含む
	std::vector<std::wstring>	filenames;
	std::vector<std::wstring>	filenamePatterns;
	std::vector<std::wstring>	mimetypes;
	std::wstring				firstLinePattern;
	std::wstring				configurationPath;	//!< 解決済みの絶対パス。無ければ空
	std::wstring				extensionId;
	std::uint64_t				generation = 0;
};

struct SExtensionSnippetFile {
	std::wstring	languageId;
	std::wstring	path;				//!< 解決済みの絶対パス
	std::wstring	extensionId;
	std::uint64_t	generation = 0;
};

//! 1 回の登録で運ばれる contribution 一式。所有者はレジストリ側が刻む。
struct SExtensionContributions {
	std::vector<SExtensionViewPresentation>		containerPresentations;
	std::vector<SExtensionViewPresentation>		viewPresentations;
	std::vector<SExtensionMenuItem>				menuItems;
	std::vector<SExtensionSubmenuDescriptor>	submenus;
	std::vector<SExtensionKeybinding>			keybindings;
	std::vector<SExtensionLanguageDescriptor>	languages;
	std::vector<SExtensionSnippetFile>			snippets;
	//! 「宣言は受理したが、このホストではまだ実装していない」contribution の名前
	std::vector<std::wstring>					acknowledged;
};

/*!
	@brief contribution points の所有権付きモデル

	CExtensionWorkbenchDispatcher から切り離してあるのは、ディスパッチャが
	「RPC を model へ写す」責務だけを持つべきで、contribution の保持と検索は
	別の関心事だから。UI 側（アクティビティバー・メニュー・キーマップ）は
	ディスパッチャを知らずにこのレジストリだけを読む。

	ビューコンテナとビューの identity はここには無い。それは
	workbench::layout::WorkbenchContributionRegistry の役目で、二重に持つと
	どちらが正かが決まらなくなる。ここが持つのはあちらが持たない表示属性だけ。

	RPC 所有スレッドが書き、UI スレッドがスナップショットを読むので thread-safe。
*/
class CExtensionContributionRegistry final {
public:
	/*!
		@brief 1 拡張ぶんの contribution をまとめて差し替える

		同じ拡張から 2 度登録が来た場合（拡張ホスト再接続時の再送）に重複させないよう、
		同一所有者の既存項目を消してから入れる。差分を取らないのは、マニフェストが
		途中で変わり得る（更新後の再登録）以上、全置換の方が状態がずれないため。
	*/
	void Register(const SExtensionContributionOwner& owner, SExtensionContributions contributions);

	void RemoveOwnedBy(std::wstring_view extensionId, std::uint64_t generation);
	void Clear();

	//! 面を指定してメニュー項目を得る。group 名・group 順・拡張 ID の順で安定に並ぶ。
	[[nodiscard]] std::vector<SExtensionMenuItem> MenuItems(std::wstring_view location) const;
	//! 登録されているメニュー面の名前一覧
	[[nodiscard]] std::vector<std::wstring> MenuLocations() const;
	//! コンテナ ID に対応する表示属性。未登録なら id が空のものを返す。
	[[nodiscard]] SExtensionViewPresentation ContainerPresentation(std::wstring_view containerId) const;
	//! ビュー ID に対応する表示属性。未登録なら id が空のものを返す。
	[[nodiscard]] SExtensionViewPresentation ViewPresentation(std::wstring_view viewId) const;
	[[nodiscard]] std::vector<SExtensionViewPresentation> ContainerPresentations() const;
	[[nodiscard]] std::vector<SExtensionViewPresentation> ViewPresentations() const;
	[[nodiscard]] std::vector<SExtensionSubmenuDescriptor> Submenus() const;
	[[nodiscard]] std::vector<SExtensionKeybinding> Keybindings() const;
	[[nodiscard]] std::vector<SExtensionLanguageDescriptor> Languages() const;
	[[nodiscard]] std::vector<SExtensionSnippetFile> SnippetFiles(std::wstring_view languageId = {}) const;
	[[nodiscard]] std::vector<std::wstring> AcknowledgedContributions(std::wstring_view extensionId) const;

	/*!
		@brief ファイル名から languageId を決める

		VS Code の優先順位に合わせる: 完全なファイル名 > 拡張子 > 何も無い。
		同じ規則を複数の拡張が主張した場合は、先に登録された方が勝つ（VS Code も同様に
		後勝ちにはしない）。該当が無ければ空文字列。
	*/
	[[nodiscard]] std::wstring ResolveLanguageId(std::wstring_view fileName) const;

private:
	//! 所有者一致判定。generation まで見ないと、前世代の残骸を巻き込んで消す/残す。
	static bool OwnedBy(
		const std::wstring& ownerExtensionId,
		std::uint64_t ownerGeneration,
		std::wstring_view extensionId,
		std::uint64_t generation) noexcept;

	//! 診断表示用の「受理したが未実装」記録。所有者ごとに 1 件。
	struct AcknowledgedRecord {
		std::wstring				extensionId;
		std::uint64_t				generation = 0;
		std::vector<std::wstring>	names;
	};

	mutable std::shared_mutex					m_mutex;
	std::vector<SExtensionViewPresentation>		m_containerPresentations;
	std::vector<SExtensionViewPresentation>		m_viewPresentations;
	std::vector<SExtensionMenuItem>				m_menuItems;
	std::vector<SExtensionSubmenuDescriptor>	m_submenus;
	std::vector<SExtensionKeybinding>			m_keybindings;
	std::vector<SExtensionLanguageDescriptor>	m_languages;
	std::vector<SExtensionSnippetFile>			m_snippets;
	std::vector<AcknowledgedRecord>				m_acknowledged;
};
