/*!	@file
	@brief 拡張（Open VSX）の検索と導入を行うサイドバー

*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_CEXTENSIONPANE_5B1D8C74_0A93_4E62_9F27_6D48B0C5E31A_H_
#define SAKURA_CEXTENSIONPANE_5B1D8C74_0A93_4E62_9F27_6D48B0C5E31A_H_
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "extension/CExtensionManager.h"
#include "extension/openvsx/OpenVsxProductionClient.h"
#include "window/CWnd.h"

/*!
	@brief 拡張のサイドバー

	Open VSX を検索し、選んだ拡張を導入・削除する。

	@par 非同期の境界
	通信は数十秒かかり得るので、必ずワーカースレッドで行う。
	ワーカーとの受け渡しは SJob を std::shared_ptr で共有し、完了時に
	通し番号だけを PostMessage する。ヒープを LPARAM に載せないので、
	ウィンドウが先に消えて投函が失敗しても漏れない。

	@par ウィンドウが先に閉じた場合
	OnDestroy では SJob::bAbandoned と bCancelled を立てて共有を解放する。
	ワーカーはネットワーク、展開、削除の境界で bCancelled を確認して副作用を止める。
	完了通知の PostMessage が失敗しても、ウィンドウ存続中はタイマーが共有ジョブを終端状態へ回収する。

	@note 拡張を「実行」する仕組み（拡張ホスト）はまだ無い。
		このサイドバーができるのは取得と配置までである。
 */
class CExtensionPane final : public CWnd
{
	using Me = CExtensionPane;

public:
	//! ジョブ完了通知。このウィンドウの内部だけで使う
	static constexpr UINT kJobDoneMessage = WM_APP + 1600;
	static constexpr UINT_PTR kJobPollTimerId = 1601;
	static constexpr UINT kJobPollIntervalMs = 250;

	//! ペインの既定幅（DPI 拡大前の論理ピクセル）
	static constexpr int kDefaultWidth = 280;

	//! ペインの最小幅（DPI 拡大前の論理ピクセル）
	static constexpr int kMinWidth = 160;

	CExtensionPane(
		config::IConfigurationService& configurationService,
		std::wstring userDataProfileId,
		HWND controlProcessWindow);
	~CExtensionPane() override;

	/*!
		@brief ウィンドウを作成する
		@param[in] hInstance	アプリケーションインスタンス
		@param[in] hwndParent	親（CEditWnd）
		@retval nullptr 作成に失敗した
	*/
	HWND Open(HINSTANCE hInstance, HWND hwndParent);

	//! ドックが占める幅（ピクセル）。非表示なら 0
	int GetDockWidth() const;

	//! 検索欄へフォーカスを移す
	void SetFocusToSearchBox();

private:
	//! ジョブの種類
	enum class EJobKind {
		Search,		//!< レジストリの検索
		Install,	//!< 導入
		Uninstall,	//!< 削除
	};

	/*!
		@brief ワーカースレッドとの受け渡し

		UI スレッドが書くのは投入前だけ、ワーカーが書くのは bDone を立てる前だけ、
		という取り決めで排他を不要にしている。bDone の release/acquire が
		結果の可視性を保証する。
	*/
	struct SJob {
		// -- 投入時に UI スレッドが設定する（以後 UI スレッドは触らない） -- //
		EJobKind			eKind = EJobKind::Search;
		int					nSerial = 0;			//!< 世代。古い結果を捨てるために使う
		std::wstring		sQuery;					//!< Search の検索文字列
		SOpenVsxExtension	ext;					//!< Install の対象
		std::wstring		sUniqueId;				//!< Uninstall の対象
		std::wstring		sTargetName;			//!< 進捗表示に使う名前
		// Search/Install only.  This client owns its network composition and has no runtime/config reference.
		std::shared_ptr<extension::openvsx::IOpenVsxRegistryClient> registryClient;

		// -- ワーカーが設定する -- //
		bool					bSucceeded = false;
		std::wstring			sErrorMsg;
		SOpenVsxSearchResult	result;

		//! 結果が書き終わったか。これが結果の可視性の境界になる
		std::atomic<bool>	bDone{ false };

		//! UI 側がもう結果を必要としていない
		std::atomic<bool>	bAbandoned{ false };

		//! 取消し後はネットワーク・展開・削除の各境界で副作用を止める
		std::atomic<bool>	bCancelled{ false };
	};

	//! 一覧の 1 行
	struct SRow {
		SOpenVsxExtension	ext;				//!< sDownloadUrl が空なら導入操作はできない
		std::wstring		sInstalledVersion;	//!< 空なら未導入
	};

	// CWnd
	LRESULT OnSize( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp ) override;
	LRESULT OnCommand( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp ) override;
	LRESULT OnNotify( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp ) override;
	LRESULT OnDestroy( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp ) override;
	LRESULT OnTimer( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp ) override;
	LRESULT DispatchEvent_WM_APP( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp ) override;

	//! 子コントロールを作る
	bool CreateChildren();

	//! 子コントロールを並べる
	void LayoutChildren( int cx, int cy );

	//! 導入済み一覧を読み直して一覧に出す（通信しない）
	void ShowInstalledList();

	//! 各行の導入状態を最新にする（通信しない）
	void RefreshInstalledState();

	//! 行の内容を一覧に流し込む
	void UpdateListView();

	//! 選択状態に応じてボタンの有効・無効を切り替える
	void UpdateButtons();

	//! 状態表示欄に文字列を設定する
	void SetStatusText( const std::wstring& sText );

	//! 選択されている行。無選択なら -1
	int GetSelectedRow() const;

	// -- 操作 -- //
	void StartSearch();
	void StartInstall();
	void StartUninstall();

	//! ジョブをワーカースレッドへ投入する
	void StartJob( std::shared_ptr<SJob> pJob );

	//! A single active job makes a reset after INT_MAX safe: no prior completion can collide.
	int AllocateJobSerial() noexcept;

	//! ジョブの完了を受け取る
	void FinishJob( int nSerial );

	//! ワーカースレッドの本体。UI に触れてはならない
	static void RunJob( std::shared_ptr<SJob> pJob, HWND hwndNotify );

	HWND	m_hwndSearchEdit    = nullptr;
	HWND	m_hwndSearchButton  = nullptr;
	HWND	m_hwndList          = nullptr;
	HWND	m_hwndInstallButton = nullptr;
	HWND	m_hwndRemoveButton  = nullptr;
	HWND	m_hwndStatus        = nullptr;
	HFONT	m_hFont             = nullptr;

	CExtensionManager		m_cManager;		//!< 導入済みの列挙にのみ使う（UI スレッド専用）
	config::IConfigurationService& m_configurationService;	//!< Used only before a worker starts.
	std::wstring			m_userDataProfileId;	//!< Selected user-data profile id (opaque, not the control authority id) for the OpenVSX factory.
	HWND					m_controlProcessWindow = nullptr;	//!< Control-owned extension host broker window.
	std::vector<SRow>		m_rows;			//!< 一覧の内容
	std::shared_ptr<SJob>	m_pJob;			//!< 実行中のジョブ。無ければ空
	int						m_nNextSerial = 1;
	bool					m_bSearchResultShown = false;	//!< 一覧が検索結果か（false なら導入済み一覧）
};

#endif /* SAKURA_CEXTENSIONPANE_5B1D8C74_0A93_4E62_9F27_6D48B0C5E31A_H_ */
