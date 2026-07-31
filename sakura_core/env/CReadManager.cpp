/*! @file */
/*
	Copyright (C) 2008, kobake
	Copyright (C) 2018-2022, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "env/CReadManager.h"
#include "CEditApp.h"	// CAppExitException
#include "window/CEditWnd.h"
#include "charset/CCodeMediator.h"
#include "io/CFileLoad.h"
#include "util/window.h"
#include "CSelectLang.h"
#include "debug/StartupTrace.h"
#include <atomic>
#include <algorithm>
#include <exception>
#include <future>

namespace {

constexpr std::size_t kMinimumBytesPerReadPartition = 1024 * 1024;
constexpr int kMaximumReadThreadCount = 8;

class CStartupReadMetrics final
{
public:
	explicit CStartupReadMetrics(bool worker) noexcept
		: m_worker(worker)
		, m_enabled(CStartupTrace::IsCollectingStartupDocumentMetrics())
	{
		if (m_enabled && m_worker) {
			::QueryPerformanceCounter(&m_workerStart);
		}
	}

	~CStartupReadMetrics()
	{
		if (!m_enabled) {
			return;
		}
		CStartupTrace::AccumulateStartupDocumentSubphase(
			CStartupTrace::StartupDocumentSubphase::Decode, m_decodeTicks, m_decodedLines);
		CStartupTrace::AccumulateStartupDocumentSubphase(
			CStartupTrace::StartupDocumentSubphase::LineBuild, m_lineBuildTicks, m_movedLines);
		CStartupTrace::AccumulateStartupReadTransfer(0, m_movedLines);
		if (m_worker) {
			LARGE_INTEGER end{};
			::QueryPerformanceCounter(&end);
			CStartupTrace::AccumulateStartupReadWorker(end.QuadPart - m_workerStart.QuadPart);
		}
	}

	bool IsEnabled() const noexcept { return m_enabled; }
	void AddDecode(std::int64_t ticks, bool producedLine) noexcept
	{
		m_decodeTicks += ticks;
		if (producedLine) {
			++m_decodedLines;
		}
	}
	void AddLineBuild(std::int64_t ticks) noexcept
	{
		m_lineBuildTicks += ticks;
		++m_movedLines;
	}

	CStartupReadMetrics(const CStartupReadMetrics&) = delete;
	CStartupReadMetrics& operator=(const CStartupReadMetrics&) = delete;

private:
	LARGE_INTEGER m_workerStart{};
	std::int64_t m_decodeTicks{};
	std::int64_t m_decodedLines{};
	std::int64_t m_lineBuildTicks{};
	std::int64_t m_movedLines{};
	bool m_worker{};
	bool m_enabled{};
};

}

int CReadManager::SelectReadThreadCount(
	std::size_t fileSize, unsigned int hardwareConcurrency) noexcept
{
	const auto availableThreads = (std::max)(1u, hardwareConcurrency);
	const std::size_t sizeLimitedThreads = fileSize == 0
		? 1
		: 1 + (fileSize - 1) / kMinimumBytesPerReadPartition;
	return static_cast<int>((std::min)({
		static_cast<std::size_t>(availableThreads),
		sizeLimitedThreads,
		static_cast<std::size_t>(kMaximumReadThreadCount),
	}));
}

/*!
	ファイルを読み込んで格納する（分割読み込みテスト版）
	@version	2.0
	@note	Windows用にコーディングしてある
	@retval	TRUE	正常読み込み
	@retval	FALSE	エラー(またはユーザーによるキャンセル?)
	@date	2002/08/30 Moca 旧ReadFileを元に作成 ファイルアクセスに関する部分をCFileLoadで行う
	@date	2003/07/26 ryoji BOMの状態の取得を追加
*/
EConvertResult CReadManager::ReadFile_To_CDocLineMgr(
	CDocLineMgr*		pcDocLineMgr,	//!< [out]
	const SLoadInfo&	sLoadInfo,		//!< [in]
	SFileInfo*			pFileInfo		//!< [out]
)
{
	LPCWSTR pszPath = sLoadInfo.cFilePath.c_str();
	std::int64_t readWorkersStarted = 0;
	std::int64_t readWorkersCollected = 0;
	const auto recordReadResult = [&](EConvertResult result) noexcept {
		CStartupTrace::SetStartupReadWorkerLifecycle(readWorkersStarted, readWorkersCollected);
		CStartupTrace::SetStartupReadResult(
			static_cast<std::int64_t>(pcDocLineMgr->GetLineCount()),
			static_cast<std::int64_t>(result));
	};

	// 文字コード種別
	const STypeConfigMini* type = nullptr;
	if( !CDocTypeManager().GetTypeConfigMini( sLoadInfo.nType, &type ) ){
		recordReadResult(RESULT_FAILURE);
		return RESULT_FAILURE;
	}
	ECodeType	eCharCode = sLoadInfo.eCharCode;
	if (CODE_AUTODETECT == eCharCode) {
		CCodeMediator cmediator( type->m_encoding );
		eCharCode = cmediator.CheckKanjiCodeOfFile( pszPath );
	}
	if (!IsValidCodeOrCPType( eCharCode )) {
		eCharCode = type->m_encoding.m_eDefaultCodetype;	// 2011.01.24 ryoji デフォルト文字コード
	}
	bool	bBom;
	if (eCharCode == type->m_encoding.m_eDefaultCodetype) {
		bBom = type->m_encoding.m_bDefaultBom;	// 2011.01.24 ryoji デフォルトBOM
	}
	else{
		bBom = CCodeTypeName( eCharCode ).IsBomDefOn();
	}
	pFileInfo->SetCodeSet( eCharCode, bBom );

	/* 既存データのクリア */
	pcDocLineMgr->DeleteAllLine();

	/* 処理中のユーザー操作を可能にする */
	if( !::BlockingHook( nullptr ) ){
		recordReadResult(RESULT_FAILURE);
		return RESULT_FAILURE; //######INTERRUPT
	}

	EConvertResult eRet = RESULT_COMPLETE;

	try{
		CFileLoad cfl( type->m_encoding );

		bool bBigFile;
#ifdef _WIN64
		bBigFile = true;
#else
		bBigFile = false;
#endif
		// ファイルを開く
		// ファイルを閉じるにはFileCloseメンバ又はデストラクタのどちらかで処理できます
		//	Jul. 28, 2003 ryoji BOMパラメータ追加
		cfl.FileOpen( pszPath, bBigFile, eCharCode, GetDllShareData().m_Common.m_sFile.GetAutoMIMEdecode(), &bBom );
		pFileInfo->SetBomExist( bBom );

		/* ファイル時刻の取得 */
		FILETIME	FileTime;
		if( cfl.GetFileTime( nullptr, nullptr, &FileTime ) ){
			pFileInfo->SetFileTime( FileTime );
		}

		// File size constrains parallelism before allocating per-partition state.
		// The actual line-boundary scan below can reduce this further.
		const int nThreadCount = SelectReadThreadCount(
			static_cast<std::size_t>(cfl.GetFileSize()), std::thread::hardware_concurrency());

		std::vector<CFileLoad> vecThreadFileLoads( nThreadCount );
		std::vector<CDocLineMgr> vecThreadDocLineMgrs( nThreadCount );
		std::vector<std::future<EConvertResult>> vecWorkerFutures;
		std::vector<int> activePartitions;
		std::atomic<bool> bCanceled = false;

		size_t nOffsetBegin = cfl.GetNextLineOffset( (size_t)cfl.GetFileSize() );
		for( int i = nThreadCount - 1; 0 <= i; i-- ){
			// 分担する範囲を決める
			const size_t nOffsetEnd = nOffsetBegin;
			nOffsetBegin = cfl.GetNextLineOffset( (size_t)((double)cfl.GetFileSize() / nThreadCount * i) );

			if( nOffsetBegin == nOffsetEnd ){
				continue;
			}

			vecThreadFileLoads[i].Prepare( cfl, nOffsetBegin, nOffsetEnd );
			vecThreadDocLineMgrs[i].SetMemoryResource(pcDocLineMgr->GetMemoryResource());
			activePartitions.push_back(i);
		}

		// The lowest non-empty range remains on the calling thread. Every other
		// partition is a real line-aligned range and may run concurrently.
		const int mainPartition = activePartitions.empty() ? -1 : activePartitions.back();
		const auto launchedWorkers = activePartitions.empty() ? 0 : activePartitions.size() - 1;
		CStartupTrace::SetStartupReadDecision(
			static_cast<std::int64_t>(cfl.GetFileSize()),
			static_cast<std::int64_t>(activePartitions.size()),
			static_cast<std::int64_t>(launchedWorkers));
		vecWorkerFutures.reserve(launchedWorkers);
		for (const int partition : activePartitions) {
			if (partition == mainPartition) continue;
			vecWorkerFutures.push_back(std::async(
				std::launch::async,
				&CReadManager::ReadLines,
				this,
				false,
				std::ref(vecThreadFileLoads[partition]),
				std::ref(vecThreadDocLineMgrs[partition]),
				std::ref(bCanceled)));
			++readWorkersStarted;
		}

		std::exception_ptr readException;
		if (mainPartition >= 0) {
			try {
				eRet = ReadLines(true, vecThreadFileLoads[mainPartition], vecThreadDocLineMgrs[mainPartition], bCanceled);
			}
			catch (...) {
				bCanceled.store(true);
				readException = std::current_exception();
			}
		}

		// Always observe every worker, even after a peer fails, before propagating
		// an exception or cancellation. Partial partition output is never appended.
		for( auto&& future : vecWorkerFutures ){
			try {
				EConvertResult eRetSub = future.get();
				if( eRetSub != RESULT_COMPLETE ){
					eRet = eRetSub;
					if (eRetSub == RESULT_FAILURE) bCanceled.store(true);
				}
			}
			catch (...) {
				bCanceled.store(true);
				if (!readException) readException = std::current_exception();
			}
			++readWorkersCollected;
		}
		CStartupTrace::SetStartupReadWorkerLifecycle(readWorkersStarted, readWorkersCollected);
		if (readException) std::rethrow_exception(readException);

		if( bCanceled.load() ){
			// 中断
			throw CAppExitException();
		}

		// 各スレッドの処理結果をpcDocLineMgrに集約
		for( int i = 0; i < nThreadCount; i++ ){
			pcDocLineMgr->AppendAsMove( vecThreadDocLineMgrs[i] );
		}

		cfl.FileClose();
	}
	catch(const CAppExitException&){
		//WM_QUITが発生した
		pcDocLineMgr->DeleteAllLine();
		recordReadResult(RESULT_FAILURE);
		return RESULT_FAILURE;
	}
	catch( const CError_FileOpen& ex ){
		eRet = RESULT_FAILURE;
		recordReadResult(eRet);
		if (ex.Reason() == CError_FileOpen::TOO_BIG) {
			// ファイルサイズが大きすぎる (32bit 版の場合は 2GB あたりが上限)
			ErrorMessage(
				CEditWnd::getInstance()->GetHwnd(),
				LS(STR_ERR_DLGDOCLM_TOOBIG),
				pszPath
			);
		}
		else if( !fexist( pszPath )){
			// ファイルがない
			ErrorMessage(
				CEditWnd::getInstance()->GetHwnd(),
				LS(STR_ERR_DLGDOCLM1),	//Mar. 24, 2001 jepro 若干修正
				pszPath
			);
		}
		else if( -1 == _waccess( pszPath, 4 )){
			// 読み込みアクセス権がない
			ErrorMessage(
				CEditWnd::getInstance()->GetHwnd(),
				LS(STR_ERR_DLGDOCLM2),
				pszPath
			 );
		}
		else{
			ErrorMessage(
				CEditWnd::getInstance()->GetHwnd(),
				LS(STR_ERR_DLGDOCLM3),
				pszPath
			 );
		}
	}
	catch( const CError_FileRead& ){
		eRet = RESULT_FAILURE;
		ErrorMessage(
			CEditWnd::getInstance()->GetHwnd(),
			LS(STR_ERR_DLGDOCLM4),
			pszPath
		 );
		/* 既存データのクリア */
		pcDocLineMgr->DeleteAllLine();
		recordReadResult(eRet);
	}
	catch (...) {
		// No worker-local line manager is ever published before every worker has
		// completed. Keep the externally visible document empty on any remaining
		// failure as well.
		pcDocLineMgr->DeleteAllLine();
		readWorkersCollected = readWorkersStarted;
		recordReadResult(RESULT_FAILURE);
		throw;
	} // 例外処理終わり

	NotifyProgress(0);
	/* 処理中のユーザー操作を可能にする */
	if( !::BlockingHook( nullptr ) ){
		pcDocLineMgr->DeleteAllLine();
		recordReadResult(RESULT_FAILURE);
		return RESULT_FAILURE; //####INTERRUPT
	}

	/* 行変更状態をすべてリセット */
//	CModifyVisitor().ResetAllModifyFlag(pcDocLineMgr, 0);
	recordReadResult(eRet);
	return eRet;
}

/*!
	ファイルから行データを読み込む
	@param[in]		bMainThread	メインスレッドで実行しているかどうか
	@param[in]		cFileLoad	ファイル読み込みクラス
	@param[out]		cDocLineMgr	読み込んだ行データを格納
	@param[in,out]	bCanceled	処理中断フラグ
	@returns	読み込み処理結果
*/
EConvertResult CReadManager::ReadLines(
	bool				bMainThread,
	CFileLoad&			cFileLoad,
	CDocLineMgr&		cDocLineMgr,
	std::atomic<bool>&	bCanceled
)
{
	CEol			cEol;
	CNativeW		cUnicodeBuffer;
	EConvertResult	eRead;
	constexpr DWORD timeInterval = 33;
	auto			nextTime = GetTickCount64() + timeInterval;
	EConvertResult	eRet = RESULT_COMPLETE;
	CStartupReadMetrics startupMetrics(!bMainThread);

	while( true ){
		LARGE_INTEGER decodeStart{};
		if (startupMetrics.IsEnabled()) {
			::QueryPerformanceCounter(&decodeStart);
		}
		eRead = cFileLoad.ReadLine(&cUnicodeBuffer, &cEol);
		if (startupMetrics.IsEnabled()) {
			LARGE_INTEGER decodeEnd{};
			::QueryPerformanceCounter(&decodeEnd);
			startupMetrics.AddDecode(
				decodeEnd.QuadPart - decodeStart.QuadPart, eRead != RESULT_FAILURE);
		}
		if (eRead == RESULT_FAILURE) {
			break;
		}
		if( eRead == RESULT_LOSESOME ){
			eRet = RESULT_LOSESOME;
		}

		if( bCanceled.load() ){
			break;
		}

		LARGE_INTEGER lineBuildStart{};
		if (startupMetrics.IsEnabled()) {
			::QueryPerformanceCounter(&lineBuildStart);
		}
		CDocEditAgent(&cDocLineMgr).AddLineStrXMove(&cUnicodeBuffer);
		if (startupMetrics.IsEnabled()) {
			LARGE_INTEGER lineBuildEnd{};
			::QueryPerformanceCounter(&lineBuildEnd);
			startupMetrics.AddLineBuild(lineBuildEnd.QuadPart - lineBuildStart.QuadPart);
		}

		if( bMainThread ){
			// 経過通知
			const auto currTime = GetTickCount64();
			if( currTime >= nextTime ){
				nextTime += timeInterval;
				NotifyProgress( cFileLoad.GetPercent() );
				// 処理中のユーザー操作を可能にする
				if( !::BlockingHook( nullptr ) ){
					// 中断検知
					bCanceled.store( true );
					eRet = RESULT_FAILURE;
					break;
				}
			}
		}
	}

	return eRet;
}
