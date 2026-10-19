/*!	@file
	@brief 行データの管理

	@author Norio Nakatani
	@date 1998/03/05  新規作成
	@date 2001/06/23 N.Nakatani 単語単位で検索する機能を実装
	@date 2001/06/23 N.Nakatani WhereCurrentWord()変更 WhereCurrentWord_2をコールするようにした
	@date 2005/09/25 D.S.Koba GetSizeOfCharで書き換え
*/
/*
	Copyright (C) 1998-2001, Norio Nakatani
	Copyright (C) 2000, genta, ao
	Copyright (C) 2001, genta, jepro, hor
	Copyright (C) 2002, hor, aroka, MIK, Moca, genta, frozen, Azumaiya, YAZAKI
	Copyright (C) 2003, Moca, ryoji, genta, かろと
	Copyright (C) 2004, genta, Moca
	Copyright (C) 2005, D.S.Koba, ryoji, かろと
	Copyright (C) 2018-2022, Sakura Editor Organization

	This source code is designed for sakura editor.
	Please contact the copyright holder to use this code for other purpose.
*/

#include "StdAfx.h"
// Oct 6, 2000 ao
#include <stdio.h>
#include <list>
#include "CDocLineMgr.h"
#include "CDocLine.h"// 2002/2/10 aroka ヘッダー整理
#include "charset/charcode.h"
#include "charset/CCodeFactory.h"
#include "charset/CCodeBase.h"
#include "charset/CCodeMediator.h"
//	Jun. 26, 2001 genta	正規表現ライブラリの差し替え
#include "extmodule/CBregexp.h"
#include "_main/global.h"

//	May 15, 2000 genta
#include "basis/CEol.h"
#include "mem/CMemory.h"// 2002/2/10 aroka
#include "mem/CPoolResource.h"

#include "io/CFileLoad.h" // 2002/08/30 Moca
#include "io/CIoBridge.h"
#include "basis/SakuraBasis.h"
#include "parse/CWordParse.h"
#include "util/window.h"
#include "util/file.h"
#include "debug/CRunningTimer.h"

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
//               コンストラクタ・デストラクタ                  //
// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //

CDocLineMgr::CDocLineMgr()
	: m_docLineMemRes(new CPoolResource<CDocLine>())
	//: m_docLineMemRes(new std::pmr::unsynchronized_pool_resource()) // メモリ使用量が大きい為に使用しない
{
	_Init();
}

CDocLineMgr::~CDocLineMgr()
{
	DeleteAllLine();
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
//                      行データの管理                         //
// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //

//! pPosの直前に新しい行を挿入
CDocLine* CDocLineMgr::InsertNewLine(CDocLine* pPos)
{
	NoteStructuralMutation();
	CDocLine* pcDocLineNew = new (m_docLineMemRes->allocate(sizeof(CDocLine))) CDocLine();
	_InsertBeforePos(pcDocLineNew,pPos);
	return pcDocLineNew;
}

//! 最下部に新しい行を挿入
CDocLine* CDocLineMgr::AddNewLine()
{
	NoteStructuralMutation();
	CDocLine* pcDocLineNew = new (m_docLineMemRes->allocate(sizeof(CDocLine))) CDocLine();
	_PushBottom(pcDocLineNew);
	return pcDocLineNew;
}

//! 全ての行を削除する
void CDocLineMgr::DeleteAllLine()
{
	NoteStructuralMutation();
	CDocLine* pDocLine = m_pDocLineTop;
	while( pDocLine ){
		CDocLine* pDocLineNext = pDocLine->GetNextLine();
		pDocLine->~CDocLine();
		m_docLineMemRes->deallocate(pDocLine, sizeof(CDocLine));
		pDocLine = pDocLineNext;
	}
	_Init();
}

//! 行の削除
void CDocLineMgr::DeleteLine( CDocLine* pcDocLineDel )
{
	NoteStructuralMutation();
	//Prev切り離し
	if( pcDocLineDel->GetPrevLine() ){
		pcDocLineDel->GetPrevLine()->m_pNext = pcDocLineDel->GetNextLine();
	}
	else{
		m_pDocLineTop = pcDocLineDel->GetNextLine();
	}

	//Next切り離し
	if( pcDocLineDel->GetNextLine() ){
		pcDocLineDel->m_pNext->m_pPrev = pcDocLineDel->GetPrevLine();
	}
	else{
		m_pDocLineBot = pcDocLineDel->GetPrevLine();
	}
	
	//参照切り離し
	if( m_pCodePrevRefer == pcDocLineDel ){
		m_pCodePrevRefer = pcDocLineDel->GetNextLine();
	}

	//データ削除
	pcDocLineDel->~CDocLine();
	m_docLineMemRes->deallocate(pcDocLineDel, sizeof(CDocLine));

	//行数減算
	m_nLines--;
	if( CLogicInt(0) == m_nLines ){
		// データがなくなった
		_Init();
	}
}

//! 他のCDocLineMgrの内容を末尾に追加する
//! 元のCDocLineMgrの内容は空になる
void CDocLineMgr::AppendAsMove( CDocLineMgr& other )
{
	NoteStructuralMutation();
	if( other.GetDocLineTop() == nullptr ){ return; }

	// 双方向リンクをつなぐ
	other.GetDocLineTop()->m_pPrev = m_pDocLineBot;
	if( m_pDocLineBot != nullptr ){
		m_pDocLineBot->m_pNext = other.GetDocLineTop();
	}

	// 先頭/末尾を更新
	if( m_pDocLineTop == nullptr ){
		m_pDocLineTop = other.GetDocLineTop();
	}
	m_pDocLineBot = other.GetDocLineBottom();

	m_nLines += other.GetLineCount();

	// 所有権が移ったので空っぽにしておく
	other._Init();
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
//              テキスト変更リビジョン/編集ログ                 //
// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //

void CDocLineMgr::PushTextEditRecord( const DocTextEdit& record ) noexcept
{
	// 追記/上限切り詰め/失敗時の全消去は AppendBoundedEditRecord の継ぎ目が
	// 担う。デストラクタ(ScopedDocMutation)経由では例外を投げられない。
	// 記録を失ったまま進むと差分が壊れるので、失敗時はログを空にして
	// CollectTextEdits 側が「抜け」を検出して全無効化するようにする。
	AppendBoundedEditRecord( m_textEditLog, record, 64 );
}

void CDocLineMgr::RecordTextEdit( CLogicInt firstLine, CLogicInt oldLineCount, CLogicInt newLineCount )
{
	++m_textRevision;
	PushTextEditRecord(DocTextEdit{ m_textRevision, firstLine, oldLineCount, newLineCount, false });
}

void CDocLineMgr::RecordTextEditFull()
{
	++m_textRevision;
	PushTextEditRecord(DocTextEdit{ m_textRevision, CLogicInt(0), CLogicInt(0), CLogicInt(0), true });
}

void CDocLineMgr::NoteStructuralMutation()
{
	// ScopedDocMutation の中では ReplaceData が正確な範囲を記録するので何もしない
	if( !m_inDocMutation ) RecordTextEditFull();
}

bool CDocLineMgr::CollectTextEdits( std::uint64_t sinceRevision, std::vector<DocTextEdit>& out ) const
{
	if( sinceRevision >= m_textRevision ) return true;
	if( EditRecordLogHasGap( m_textEditLog, sinceRevision ) ) {
		return false; // 記録が押し出されて差分を再構成できない
	}
	for( const auto& record : m_textEditLog ) {
		if( record.revision > sinceRevision ) out.push_back( record );
	}
	return true;
}

CDocLineMgr::ScopedDocMutation::ScopedDocMutation(
	CDocLineMgr& mgr, const DocLineReplaceArg& arg )
	: m_mgr( mgr )
{
	// ReplaceData は自分自身を呼ばないのでネストはあり得ない (不変条件)。
	assert( !m_mgr.m_inDocMutation );
	m_mgr.m_inDocMutation = true;
	m_linesBefore = m_mgr.GetLineCount();
	m_fromY = arg.sDelRange.GetFrom().y;
	m_toY = arg.sDelRange.GetTo().y;
	m_toX = arg.sDelRange.GetTo().x;
	const CDocLine* pcToLine = m_mgr.GetLine( m_toY );
	m_toLineLenWithoutEOL = pcToLine == nullptr ? 0 : static_cast<int>( pcToLine->GetLengthWithoutEOL() );
	m_toLineLenWithEOL = pcToLine == nullptr ? 0 : static_cast<int>( pcToLine->GetLengthWithEOL() );
	m_hasInsData = arg.pInsData != nullptr && arg.pInsData->size() > 0;
}

CDocLineMgr::ScopedDocMutation::~ScopedDocMutation()
{
	assert( m_mgr.m_inDocMutation );

	// 変更された旧行の数。範囲の終端行 to.y は常に影響を受けるものとして
	// 数える (VS Code の inclusive endLineNumber と同じく、行頭終端で
	// 実際には変わらない行を1行余分に無効化するだけで済む安全側の近似)。
	// サクラの座標系では「行末以降」の位置が (行の文字数+EOL長, 同一y) で
	// 表されるので、そこまで達する削除は EOL を消して次行を連結する
	// → 次行も変更行になる。部分行の削除が連結になった場合、その連結が
	// 消去済み領域をまたいで最初に残った行 (= to.y) を取り込むので、
	// to.y を含めて数えれば必ずカバーできる。
	// from.y が論理行数を超える入力は ReplaceData の呼び出し規約に存在しない
	// (先頭位置は常に実在する行または直後の phantom 行)。assert で契約を示す。
	const CLogicInt linesAfter = m_mgr.GetLineCount();
	assert( m_fromY <= m_linesBefore );
	const CLogicInt from = m_fromY;
	CLogicInt lastAffected = m_toY;
	if( m_toY < m_linesBefore && static_cast<int>( m_toX ) > m_toLineLenWithoutEOL ) {
		++lastAffected;
	}
	// 挿入データがある場合、挿入位置にある最初の生き残り行も変更される
	// (行全体が削除されたときは、その次の行が書き換え・連結対象になる)。
	if( m_hasInsData ) {
		CLogicInt insFirst = m_toY;
		if( m_toY < m_linesBefore && static_cast<int>( m_toX ) >= m_toLineLenWithEOL ) {
			++insFirst;
		}
		if( insFirst > lastAffected ) lastAffected = insFirst;
	}
	CLogicInt oldCount = lastAffected - m_fromY + CLogicInt(1);
	// sDelRange は常に正規化済み (from <= to) なので lastAffected >= m_fromY。
	assert( oldCount >= CLogicInt(1) );
	const CLogicInt maxOld = m_linesBefore - from;
	if( oldCount > maxOld ) oldCount = maxOld;
	const CLogicInt newCount = oldCount + ( linesAfter - m_linesBefore );
	assert( newCount >= CLogicInt(0) );
	m_mgr.RecordTextEdit( from, oldCount, newCount );
	m_mgr.m_inDocMutation = false;
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
//                   行データへのアクセス                      //
// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //

/*!
	指定された番号の行へのポインタを返す

	@param nLine [in] 行番号
	@return 行オブジェクトへのポインタ。該当行がない場合はNULL。
*/
const CDocLine* CDocLineMgr::GetLine( CLogicInt nLine ) const
{
	CLogicInt nCounter;
	CDocLine* pDocLine;
	if( CLogicInt(0) == m_nLines ){
		return nullptr;
	}
	// 2004.03.28 Moca nLineが負の場合のチェックを追加
	if( CLogicInt(0) > nLine || nLine >= m_nLines ){
		return nullptr;
	}
	// 2004.03.28 Moca m_pCodePrevReferより、Top,Botのほうが近い場合は、そちらを利用する
	CLogicInt nPrevToLineNumDiff = t_abs( m_nPrevReferLine - nLine );
	if( m_pCodePrevRefer == nullptr
	  || nLine < nPrevToLineNumDiff
	  || m_nLines - nLine < nPrevToLineNumDiff
	){
		if( m_pCodePrevRefer == nullptr ){
			MY_RUNNINGTIMER( cRunningTimer, L"CDocLineMgr::GetLine() 	m_pCodePrevRefer == NULL" );
		}

		if( nLine < (m_nLines / 2) ){
			nCounter = CLogicInt(0);
			pDocLine = m_pDocLineTop;
			while( pDocLine ){
				if( nLine == nCounter ){
					m_nPrevReferLine = nLine;
					m_pCodePrevRefer = pDocLine;
					m_pDocLineCurrent = pDocLine->GetNextLine();
					return pDocLine;
				}
				pDocLine = pDocLine->GetNextLine();
				nCounter++;
			}
		}
		else{
			nCounter = m_nLines - CLogicInt(1);
			pDocLine = m_pDocLineBot;
			while( nullptr != pDocLine ){
				if( nLine == nCounter ){
					m_nPrevReferLine = nLine;
					m_pCodePrevRefer = pDocLine;
					m_pDocLineCurrent = pDocLine->GetNextLine();
					return pDocLine;
				}
				pDocLine = pDocLine->GetPrevLine();
				nCounter--;
			}
		}
	}
	else{
		if( nLine == m_nPrevReferLine ){
			m_nPrevReferLine = nLine;
			m_pDocLineCurrent = m_pCodePrevRefer->GetNextLine();
			return m_pCodePrevRefer;
		}
		else if( nLine > m_nPrevReferLine ){
			nCounter = m_nPrevReferLine + CLogicInt(1);
			pDocLine = m_pCodePrevRefer->GetNextLine();
			while( nullptr != pDocLine ){
				if( nLine == nCounter ){
					m_nPrevReferLine = nLine;
					m_pCodePrevRefer = pDocLine;
					m_pDocLineCurrent = pDocLine->GetNextLine();
					return pDocLine;
				}
				pDocLine = pDocLine->GetNextLine();
				++nCounter;
			}
		}
		else{
			nCounter = m_nPrevReferLine - CLogicInt(1);
			pDocLine = m_pCodePrevRefer->GetPrevLine();
			while( nullptr != pDocLine ){
				if( nLine == nCounter ){
					m_nPrevReferLine = nLine;
					m_pCodePrevRefer = pDocLine;
					m_pDocLineCurrent = pDocLine->GetNextLine();
					return pDocLine;
				}
				pDocLine = pDocLine->GetPrevLine();
				nCounter--;
			}
		}
	}
	return nullptr;
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
//                         実装補助                            //
// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //

void CDocLineMgr::_Init()
{
	m_pDocLineTop = nullptr;
	m_pDocLineBot = nullptr;
	m_nLines = CLogicInt(0);
	m_nPrevReferLine = CLogicInt(0);
	m_pCodePrevRefer = nullptr;
	m_pDocLineCurrent = nullptr;
	CDiffManager::getInstance()->SetDiffUse(false);	/* DIFF使用中 */	//@@@ 2002.05.25 MIK     //##後でCDocListener::OnClear (OnAfterClose) を作成し、そこに移動
}

// -- -- チェーン関数 -- -- // 2007.10.11 kobake 作成
//!最下部に挿入
void CDocLineMgr::_PushBottom(CDocLine* pDocLineNew)
{
	if( !m_pDocLineTop ){
		m_pDocLineTop = pDocLineNew;
	}
	pDocLineNew->m_pPrev = m_pDocLineBot;

	if( m_pDocLineBot ){
		m_pDocLineBot->m_pNext = pDocLineNew;
	}
	m_pDocLineBot = pDocLineNew;
	pDocLineNew->m_pNext = nullptr;

	++m_nLines;
}

//!pPosの直前に挿入。pPosにNULLを指定した場合は、最下部に追加。
void CDocLineMgr::_InsertBeforePos(CDocLine* pDocLineNew, CDocLine* pPos)
{
	//New.Nextを設定
	pDocLineNew->m_pNext = pPos;

	//New.Prev, Other.Prevを設定
	if(pPos){
		pDocLineNew->m_pPrev = pPos->GetPrevLine();
		pPos->m_pPrev = pDocLineNew;
	}
	else{
		pDocLineNew->m_pPrev = m_pDocLineBot;
		m_pDocLineBot = pDocLineNew;
	}

	//Other.Nextを設定
	if( pDocLineNew->GetPrevLine() ){
		pDocLineNew->GetPrevLine()->m_pNext = pDocLineNew;
	}
	else{
		m_pDocLineTop = pDocLineNew;
	}

	//行数を加算
	++m_nLines;
}

//! pPosの直後に挿入。pPosにNULLを指定した場合は、先頭に追加。
void CDocLineMgr::_InsertAfterPos(CDocLine* pDocLineNew, CDocLine* pPos)
{
	//New.Prevを設定
	pDocLineNew->m_pPrev = pPos;

	//New.Next, Other.Nextを設定
	if( pPos ){
		pDocLineNew->m_pNext = pPos->GetNextLine();
		pPos->m_pNext = pDocLineNew;
	}
	else{
		pDocLineNew->m_pNext = m_pDocLineTop;
		m_pDocLineTop = pDocLineNew;
	}

	//Other.Prevを設定
	if( pDocLineNew->GetNextLine() ){
		pDocLineNew->m_pNext->m_pPrev = pDocLineNew;
	}
	else{
		m_pDocLineBot = pDocLineNew;
	}

	//行数を加算
	m_nLines++;
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
//                         デバッグ                            //
// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //

/*!	@brief CDocLineMgrDEBUG用

	@date 2004.03.18 Moca
		m_pDocLineCurrentとm_pCodePrevReferがデータチェーンの
		要素を指しているかの検証機能を追加．

*/
void CDocLineMgr::DUMP()
{
#ifdef _DEBUG
	MYTRACE( L"------------------------\n" );

	CDocLine* pDocLine;
	CDocLine* pDocLineNext;
	CDocLine* pDocLineEnd = nullptr;
	pDocLine = m_pDocLineTop;

	// 正当性を調べる
	bool bIncludeCurrent = false;
	bool bIncludePrevRefer = false;
	CLogicInt nNum = CLogicInt(0);
	if( m_pDocLineTop->m_pPrev != nullptr ){
		MYTRACE( L"error: m_pDocLineTop->m_pPrev != NULL\n");
	}
	if( m_pDocLineBot->m_pNext != nullptr ){
		MYTRACE( L"error: m_pDocLineBot->m_pNext != NULL\n" );
	}
	while( nullptr != pDocLine ){
		if( m_pDocLineCurrent == pDocLine ){
			bIncludeCurrent = true;
		}
		if( m_pCodePrevRefer == pDocLine ){
			bIncludePrevRefer = true;
		}
		if( nullptr != pDocLine->GetNextLine() ){
			if( pDocLine->m_pNext == pDocLine ){
				MYTRACE( L"error: pDocLine->m_pPrev Invalid value.\n" );
				break;
			}
			if( pDocLine->m_pNext->m_pPrev != pDocLine ){
				MYTRACE( L"error: pDocLine->m_pNext->m_pPrev != pDocLine.\n" );
				break;
			}
		}else{
			pDocLineEnd = pDocLine;
		}
		pDocLine = pDocLine->GetNextLine();
		nNum++;
	}
	
	if( pDocLineEnd != m_pDocLineBot ){
		MYTRACE( L"error: pDocLineEnd != m_pDocLineBot" );
	}
	
	if( nNum != m_nLines ){
		MYTRACE( L"error: nNum(%d) != m_nLines(%d)\n", nNum, m_nLines );
	}
	if( false == bIncludeCurrent && m_pDocLineCurrent != nullptr ){
		MYTRACE( L"error: m_pDocLineCurrent=%08lxh Invalid value.\n", m_pDocLineCurrent );
	}
	if( false == bIncludePrevRefer && m_pCodePrevRefer != nullptr ){
		MYTRACE( L"error: m_pCodePrevRefer =%08lxh Invalid value.\n", m_pCodePrevRefer );
	}

	// DUMP
	MYTRACE( L"m_nLines=%d\n", m_nLines );
	MYTRACE( L"m_pDocLineTop=%08lxh\n", m_pDocLineTop );
	MYTRACE( L"m_pDocLineBot=%08lxh\n", m_pDocLineBot );
	pDocLine = m_pDocLineTop;
	while( nullptr != pDocLine ){
		pDocLineNext = pDocLine->GetNextLine();
		MYTRACE( L"\t-------\n" );
		MYTRACE( L"\tthis=%08lxh\n", pDocLine );
		MYTRACE( L"\tpPrev; =%08lxh\n", pDocLine->GetPrevLine() );
		MYTRACE( L"\tpNext; =%08lxh\n", pDocLine->GetNextLine() );

		MYTRACE( L"\tm_enumEOLType =%ls\n", pDocLine->GetEol().GetName() );
		MYTRACE( L"\tm_nEOLLen =%d\n", pDocLine->GetEol().GetLen() );

//		MYTRACE( L"\t[%ls]\n", *(pDocLine->m_pLine) );
		MYTRACE( L"\tpDocLine->m_cLine.GetLength()=[%d]\n", pDocLine->GetLengthWithEOL() );
		MYTRACE( L"\t[%ls]\n", pDocLine->GetPtr() );

		pDocLine = pDocLineNext;
	}
	MYTRACE( L"------------------------\n" );
#endif // _DEBUG
	return;
}
