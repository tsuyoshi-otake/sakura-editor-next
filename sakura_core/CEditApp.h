/*! @file */
/*
	Copyright (C) 2007, kobake
	Copyright (C) 2018-2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_CEDITAPP_421797BC_DD8E_4209_AAF7_6BDC4D1CAAE9_H_
#define SAKURA_CEDITAPP_421797BC_DD8E_4209_AAF7_6BDC4D1CAAE9_H_
#pragma once

//2007.10.23 kobake 作成

#include "util/design_template.h"
#include "uiparts/CSoundSet.h"
#include "uiparts/CImageListMgr.h"
#include "types/CType.h"

#include <filesystem>
#include <memory>

namespace workbench::editor {
	class CEditDocLegacyEditorBackend;
	class CEditDocWorkingCopyBackend;
	class CEditorServiceLegacyAdapter;
	class EditorCoreService;
	class EditorWorkingCopyCoordinator;
}

namespace workbench::editor::persistence {
	class CEditDocWorkingCopyPersistenceAdapter;
	class EditorCoreRecoveredInputAdopter;
	class EditorCoreWorkingCopyCaptureContextSource;
	class EditorWorkingCopyLifecycle;
	class EditorWorkingCopyLifecycleBridge;
	class IWorkingCopyPersistenceStore;
	struct EditorWorkingCopyLifecycleResult;
	struct EditorWorkingCopyRestorePolicy;
	struct WorkingCopyPersistenceScope;
}

namespace workbench {
class CWorkbenchRuntime;
class WorkbenchBootstrapContext;
struct WorkbenchRuntimeDependencies;
}

class CEditDoc;
class CEditWnd;
class CLoadAgent;
class CSaveAgent;
class CVisualProgress;
class CMruListener;
class CSMacroMgr;
class CPropertyManager;
class CGrepAgent;
class IExtensionSecretSessionStorage;
enum EFunctionCode;

//!エディタ部分アプリケーションクラス。CNormalProcess1個につき、1個存在。
class CEditApp final : public TSakuraSingleton<CEditApp> {
public:
	CEditApp();
	~CEditApp() noexcept;

	//! Creates the process-local workbench services before the native window.
	//! A false result is terminal for this editor-process startup attempt.
	[[nodiscard]] bool Create(
		HINSTANCE hInst,
		int groupId,
		workbench::WorkbenchBootstrapContext bootstrap,
		workbench::WorkbenchRuntimeDependencies dependencies,
		std::unique_ptr<workbench::editor::persistence::IWorkingCopyPersistenceStore> workingCopyStore,
		workbench::editor::persistence::WorkingCopyPersistenceScope workingCopyScope,
		std::filesystem::path profileDirectory,
		std::unique_ptr<IExtensionSecretSessionStorage> extensionSecretStorage);
	//! Restore is invoked after the native layout/group exists and startup policy is known.
	[[nodiscard]] workbench::editor::persistence::EditorWorkingCopyLifecycleResult RestoreWorkingCopies(
		const workbench::editor::persistence::EditorWorkingCopyRestorePolicy& policy);

	//モジュール情報
	HINSTANCE GetAppInstance() const{ return m_hInst; }	//!< インスタンスハンドル取得

	//ウィンドウ情報
	CEditWnd* GetEditWindow(){ return m_pcEditWnd; }		//!< ウィンドウ取得

	CEditDoc*		GetDocument(){ return m_pcEditDoc; }
	CImageListMgr&	GetIcons(){ return m_cIcons; }
	[[nodiscard]] workbench::editor::EditorCoreService* GetEditorCoreService() noexcept;
	[[nodiscard]] const workbench::editor::EditorCoreService* GetEditorCoreService() const noexcept;

	bool OpenPropertySheet( int nPageNum );
	bool OpenPropertySheetTypes( int nPageNum, CTypeConfig nSettingType );

public:
	HINSTANCE			m_hInst = nullptr;

	//ドキュメント
	CEditDoc*			m_pcEditDoc = nullptr;

	//ウィンドウ
	CEditWnd*			m_pcEditWnd = nullptr;

	//IO管理
	CLoadAgent*			m_pcLoadAgent = nullptr;
	CSaveAgent*			m_pcSaveAgent = nullptr;
	CVisualProgress*	m_pcVisualProgress = nullptr;

	//その他ヘルパ
	CMruListener*		m_pcMruListener = nullptr;		//!< MRU管理
	CSMacroMgr*			m_pcSMacroMgr = nullptr;		//!< マクロ管理

	CPropertyManager*	m_pcPropertyManager = nullptr;	//!< プロパティ管理

	CGrepAgent*			m_pcGrepAgent = nullptr;		//!< GREPモード
	CSoundSet			m_cSoundSet;					//!< サウンド管理

	//GUIオブジェクト
	CImageListMgr		m_cIcons;					//!< Image List

private:
	// The authoritative process-local editor model. CEditDoc may remain allocated
	// while this service legitimately contains zero open inputs.
	std::unique_ptr<workbench::editor::EditorCoreService> m_editorCoreService;
	// Composition-root owned migration seam. The window borrows these objects;
	// the adapter borrows both the core and the explicitly prepared legacy backend.
	std::unique_ptr<workbench::editor::CEditDocLegacyEditorBackend> m_legacyEditorBackend;
	std::unique_ptr<workbench::editor::CEditorServiceLegacyAdapter> m_editorServiceLegacyAdapter;
	// Imperative working-copy effects are separate from the read-only adoption
	// backend.  The coordinator borrows the core and this backend, and both
	// outlive the native window that dispatches the stable workbench commands.
	std::unique_ptr<workbench::editor::CEditDocWorkingCopyBackend> m_workingCopyBackend;
	std::unique_ptr<workbench::editor::EditorWorkingCopyCoordinator> m_workingCopyCoordinator;
	// Hot Exit composition. The bridge and lifecycle are destroyed before every
	// borrowed adapter, store, document, core service, and control runtime.
	std::unique_ptr<workbench::editor::persistence::IWorkingCopyPersistenceStore> m_workingCopyPersistenceStore;
	std::unique_ptr<workbench::editor::persistence::EditorCoreWorkingCopyCaptureContextSource> m_workingCopyContextSource;
	std::unique_ptr<workbench::editor::persistence::CEditDocWorkingCopyPersistenceAdapter> m_workingCopyPersistenceAdapter;
	std::unique_ptr<workbench::editor::persistence::EditorCoreRecoveredInputAdopter> m_workingCopyRecoveredInputAdopter;
	std::unique_ptr<workbench::editor::persistence::EditorWorkingCopyLifecycle> m_workingCopyLifecycle;
	std::unique_ptr<workbench::editor::persistence::EditorWorkingCopyLifecycleBridge> m_workingCopyLifecycleBridge;
	// Owns profile/configuration/workspace truth for this editor window. The
	// native window borrows it and is destroyed before this runtime is stopped.
	std::unique_ptr<workbench::CWorkbenchRuntime> m_workbenchRuntime;
};

//WM_QUIT検出例外
class CAppExitException : public std::exception{
public:
	const char* what() const throw() override{ return "CAppExitException"; }
};

#endif /* SAKURA_CEDITAPP_421797BC_DD8E_4209_AAF7_6BDC4D1CAAE9_H_ */
