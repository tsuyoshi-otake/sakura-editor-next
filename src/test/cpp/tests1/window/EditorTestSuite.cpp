/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
 */
#include "pch.h"
#include "window/EditorTestSuite.hpp"

#include "_main/CAppMode.h"

namespace window {

/*!
 * テストスイートの開始前に1回だけ呼ばれる関数
 */
/* static */ void EditorTestSuite::SetUpEditor()
{
	SetUpShareData();

	// CanBeMoveリージョンをテストケースに分割する。（すぐ対応できないのでコメント残し）

	// ドキュメントの初期化前に文字幅キャッシュの生成が必要
	SelectCharWidthCache(CWM_FONT_EDIT, CWM_CACHE_SHARE);
	InitCharWidthCache(GetDllShareData().m_Common.m_sView.m_lf);

#pragma region CanBeMove
	// ドキュメントがなくてもエラーにならない
	EXPECT_THAT(GetDocument(), IsNull());

	// ドキュメントがないのでエラー
	EXPECT_ANY_THROW(GetEditDoc());

#pragma endregion CanBeMove

	const auto hInst = G_AppInstance();

	auto* const app = CEditApp::getInstance();
	app->SetAppInstanceForTesting(hInst);

	//ヘルパ作成
	CEditApp::getInstance()->m_cIcons.Create(hInst);

	// CEditViewをインスタンス化するにはドキュメントのインスタンスが必要
	pcEditDoc = app->AdoptDocumentForTesting(std::make_unique<CEditDoc>(nullptr));

#pragma region CanBeMove
	// ドキュメントがあるので値を返す
	EXPECT_THAT(GetDocument(), pcEditDoc);

	// ドキュメントがあるのでエラーにならない
	EXPECT_NO_THROW([] { GetEditDoc(); });

	// 編集ウインドウがなくてもエラーにならない
	EXPECT_THAT(GetEditWndPtr(), IsNull());

	// 編集ウインドウがないのでエラー
	EXPECT_ANY_THROW(GetEditWnd());

#pragma endregion CanBeMove

	//IO管理
	pcLoadAgent = app->AdoptLoadAgentForTesting(std::make_unique<CLoadAgent>());
	pcSaveAgent = app->AdoptSaveAgentForTesting(std::make_unique<CSaveAgent>());
	pcVisualProgress = app->AdoptVisualProgressForTesting(std::make_unique<CVisualProgress>());

	//GREPモード管理
	pcGrepAgent = app->AdoptGrepAgentForTesting(std::make_unique<CGrepAgent>());

	//編集モード
	CAppMode::getInstance();	//ウィンドウよりも前にイベントを受け取るためにここでインスタンス作成

	// SMacroMgrを用意する
	pcSMacroMgr = app->AdoptMacroManagerForTesting(std::make_unique<CSMacroMgr>());

	//ドキュメントの作成
	pcEditDoc->Create();

	// CEditWndを用意する
	pcEditWnd = app->AdoptEditWindowForTesting(std::make_unique<CEditWnd>());

#pragma region CanBeMove
	// 編集ウインドウがあるので値を返す
	EXPECT_THAT(GetEditWndPtr(), pcEditWnd);

	// 編集ウインドウがあるのでエラーにならない
	EXPECT_NO_THROW([] { GetEditWnd(); });

#pragma endregion CanBeMove

	//MRU管理
	pcMruListener = app->AdoptMruListenerForTesting(std::make_unique<CMruListener>());

	//プロパティ管理
	pcPropertyManager = app->AdoptPropertyManagerForTesting(std::make_unique<CPropertyManager>());
	app->GetPropertyManager()->Create(
		pcEditWnd->GetHwnd(),
		&CEditApp::getInstance()->m_cIcons,
		&pcEditWnd->GetMenuDrawer()
	);
}

/*!
 * テストスイートの終了後に1回だけ呼ばれる関数
 */
/* static */ void EditorTestSuite::TearDownEditor()
{
	CEditApp::resetInstance();

	pcPropertyManager = nullptr;
	pcMruListener = nullptr;
	pcGrepAgent = nullptr;
	pcVisualProgress = nullptr;
	pcSaveAgent = nullptr;
	pcLoadAgent = nullptr;

	pcSMacroMgr = nullptr;

	pcEditWnd = nullptr;

	pcEditDoc = nullptr;

	TearDownShareData();
}

} // namespace window
