/*!	@file
	@brief 行データの管理

	@author Norio Nakatani
	@date 1998/3/5  新規作成
	@date 2001/06/23 N.Nakatani WhereCurrentWord_2()追加 staticメンバ
	@date 2001/12/03 hor しおり(bookmark)機能追加に伴う関数追加
*/
/*
	Copyright (C) 1998-2001, Norio Nakatani
	Copyright (C) 2000, genta
	Copyright (C) 2001, hor, genta
	Copyright (C) 2002, aroka, MIK, hor
	Copyright (C) 2003, Moca, ryoji
	Copyright (C) 2018-2022, Sakura Editor Organization

	This source code is designed for sakura editor.
	Please contact the copyright holder to use this code for other purpose.
*/

#ifndef SAKURA_CDOCLINEMGR_2896A92E_87D2_4BA6_A745_95A5C157E59D_H_
#define SAKURA_CDOCLINEMGR_2896A92E_87D2_4BA6_A745_95A5C157E59D_H_
#pragma once

#include <Windows.h>
#include <cstdint>
#include <deque>
#include <memory_resource>
#include <memory>
#include <vector>
#include "_main/global.h" // 2002/2/10 aroka
#include "basis/SakuraBasis.h"
#include "util/design_template.h"
#include "cmd/COpe.h"
#include "CDocLine.h"

class CBregexp; // 2002/2/10 aroka

struct DocLineReplaceArg {
	CLogicRange		sDelRange;			//!< [in] 削除範囲。ロジック単位。
	COpeLineData*	pcmemDeleted;		//!< [out] 削除されたデータを保存
	COpeLineData*	pInsData;			//!< [in,out] 挿入するデータ(中身が移動する)
	CLogicInt		nDeletedLineNum;	//!< [out] 削除した行の総数
	CLogicInt		nInsLineNum;		//!< [out] 挿入によって増えた行の数
	CLogicPoint		ptNewPos;			//!< [out] 挿入された部分の次の位置
	int				nDelSeq;			//!< [in] 削除行のOpeシーケンス
	int				nInsSeq;			//!< [out] 挿入行の元のシーケンス
};

//! 論理行単位で記録される1回分のテキスト変更。`firstLine` は先頭の変更行で、
//! 旧 `[firstLine, firstLine+oldLineCount)` が `newLineCount` 行に置き換わった。
//! `full` は正確な範囲を記述できない変更 (一括ロード/リロード/復元経路) を示し、
//! 利用側は「全て変わった」として扱わなければならない。
struct DocTextEdit {
	const std::uint64_t	revision;
	const CLogicInt		firstLine;
	const CLogicInt		oldLineCount;
	const CLogicInt		newLineCount;
	const bool			full;
};

/*-----------------------------------------------------------------------
クラスの宣言
-----------------------------------------------------------------------*/
//2007.09.30 kobake WhereCurrentWord_2 を CWordParse に移動
class CDocLineMgr{
public:
	//コンストラクタ・デストラクタ
	CDocLineMgr();
	~CDocLineMgr();

	//状態
	CLogicInt GetLineCount() const{ return m_nLines; }	//!< 全行数を返す

	//行データへのアクセス
	const CDocLine* GetLine( CLogicInt nLine ) const;						//!< 指定行を取得
	CDocLine* GetLine( CLogicInt nLine ){
		return const_cast<CDocLine*>(const_cast<CDocLine*>(static_cast<const CDocLineMgr*>(this)->GetLine( nLine )));
	}
	const CDocLine* GetDocLineTop() const { return m_pDocLineTop; }		//!< 先頭行を取得
	CDocLine* GetDocLineTop() { return m_pDocLineTop; }		//!< 先頭行を取得
	const CDocLine* GetDocLineBottom() const { return m_pDocLineBot; }	//!< 最終行を取得
	CDocLine* GetDocLineBottom() { return m_pDocLineBot; }	//!< 最終行を取得

	//行データの管理
	CDocLine* InsertNewLine(CDocLine* pPos);	//!< pPosの直前に新しい行を挿入
	CDocLine* AddNewLine();						//!< 最下部に新しい行を挿入
	void DeleteAllLine();						//!< 全ての行を削除する
	void DeleteLine( CDocLine* );				//!< 行の削除
	void AppendAsMove( CDocLineMgr& other );	//!< 他のCDocLineMgrの内容を末尾に追加する

	//テキスト変更リビジョン/編集ログ（論理行単位）
	//! 単調増加リビジョン。記録された変更ごとに1ずつ進む。
	std::uint64_t GetTextRevision() const noexcept { return m_textRevision; }
	//! `sinceRevision` より新しい記録を全て `out` に追加する。ログの上限 (64件)
	//! を超えて古い記録が押し出されている場合は false (利用側は全無効化する)。
	bool CollectTextEdits( std::uint64_t sinceRevision, std::vector<DocTextEdit>& out ) const;
	//! ReplaceData 中の行リスト一括操作が個別に "full" を記録しないようにする
	//! RAII ガード。デストラクタで正確な変更範囲を1件記録する。
	//! 生成は CSearchAgent::ReplaceData の1箇所のみでネストはあり得ない
	//! (m_inDocMutation は検査用の bool)。
	class ScopedDocMutation final {
	public:
		ScopedDocMutation( CDocLineMgr& mgr, const DocLineReplaceArg& arg );
		~ScopedDocMutation();
		ScopedDocMutation( const ScopedDocMutation& ) = delete;
		ScopedDocMutation& operator=( const ScopedDocMutation& ) = delete;
	private:
		CDocLineMgr&	m_mgr;
		CLogicInt		m_linesBefore = CLogicInt(0);
		CLogicInt		m_fromY = CLogicInt(0);
		CLogicInt		m_toY = CLogicInt(0);
		CLogicInt		m_toX = CLogicInt(0);
		int				m_toLineLenWithoutEOL = 0;
		int				m_toLineLenWithEOL = 0;
		bool			m_hasInsData = false;
	};
	void RecordTextEdit( CLogicInt firstLine, CLogicInt oldLineCount, CLogicInt newLineCount );
	void RecordTextEditFull();

	// -- -- 編集ログの純粋継ぎ目（テストが直接駆動できる）-- -- //
	//! `record` を `log` の末尾へ追加し、`capacity` を超えた分を先頭から
	//! 落とす。途中で例外が出た場合はログを空にして「抜け」を作る。
	//! noexcept: PushTextEditRecord 経由の呼び出しは ScopedDocMutation の
	//! デストラクタからも行われるため、例外を透過できない。
	template<class Log>
	static void AppendBoundedEditRecord( Log& log, const typename Log::value_type& record,
		std::size_t capacity ) noexcept
	{
		try {
			log.push_back( record );
			while( log.size() > capacity ) log.pop_front();
		}
		catch( ... ) {
			log.clear();
		}
	}
	//! CollectTextEdits の抜け判定: 空、または先頭記録が `sinceRevision+1`
	//! より新しいログからは差分を再構成できない。
	template<class Log>
	static bool EditRecordLogHasGap( const Log& log, std::uint64_t sinceRevision ) noexcept
	{
		return log.empty() || log.front().revision > sinceRevision + 1;
	}

	// メモリリソースのアクセス
	std::shared_ptr<std::pmr::memory_resource> GetMemoryResource() const { return m_docLineMemRes; }
	void SetMemoryResource(std::shared_ptr<std::pmr::memory_resource> memoryResource) { m_docLineMemRes = memoryResource; }

	//デバッグ
	void DUMP();

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//                         実装補助                            //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
protected:
	void _Init();
	// -- -- チェーン関数 -- -- // 2007.10.11 kobake 作成
	void _PushBottom(CDocLine* pDocLineNew);             //!< 最下部に挿入
	void _InsertBeforePos(CDocLine* pDocLineNew, CDocLine* pPos); //!< pPosの直前に挿入
	void _InsertAfterPos(CDocLine* pDocLineNew, CDocLine* pPos); //!< pPosの直後に挿入

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//                        メンバ変数                           //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
private:
	void NoteStructuralMutation();	//!< 行リスト一括操作から呼ぶ。ガード非適用時は "full" を記録する
	void PushTextEditRecord( const DocTextEdit& record ) noexcept;	//!< 記録失敗時はログを空にして抜けを作る
	CDocLine*	m_pDocLineTop;		//!< 最初の行
	CDocLine*	m_pDocLineBot;		//!< 最後の行(※1行しかない場合はm_pDocLineTopと等しくなる)
	CLogicInt	m_nLines;			//!< 全行数
	std::shared_ptr<std::pmr::memory_resource> m_docLineMemRes;
	std::uint64_t			m_textRevision = 0;
	std::deque<DocTextEdit>	m_textEditLog;	//!< 上限64件
	bool					m_inDocMutation = false;	//!< ScopedDocMutation の生存中だけ true

public:
	//$$ kobake注: 以下、絶対に切り離したい（最低切り離せなくても、変数の意味をコメントで明確に記すべき）変数群
	mutable CDocLine*	m_pDocLineCurrent;	//!< 順アクセス時の現在位置
	mutable CLogicInt	m_nPrevReferLine;
	mutable CDocLine*	m_pCodePrevRefer;

	DISALLOW_COPY_AND_ASSIGN(CDocLineMgr);
};
#endif /* SAKURA_CDOCLINEMGR_2896A92E_87D2_4BA6_A745_95A5C157E59D_H_ */
