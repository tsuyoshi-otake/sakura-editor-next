/*! @file */
/*
	Copyright (C) 2008, kobake
	Copyright (C) 2018-2022, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_CDOCOUTLINE_BDF55702_D938_432D_99F2_BF0F98A7C5FE_H_
#define SAKURA_CDOCOUTLINE_BDF55702_D938_432D_99F2_BF0F98A7C5FE_H_
#pragma once

#include "basis/SakuraBasis.h"

#include <functional>
#include <memory>

class CEditDoc;
class CFuncInfoArr;
struct SOneRule;
enum EOutlineType;

namespace workbench::outline {
struct OutlineDocumentSnapshot;
}

//! Input boundary used by the legacy Outline parsers.
//!
//! The live implementation is an adapter around CEditDoc.  The workbench
//! implementation is a value-owned snapshot reader.  Keeping this as a named
//! interface prevents the parser implementation from depending on either
//! document lifetime and makes the logical-only snapshot contract explicit.
class CDocOutlineDocumentSource {
public:
	virtual ~CDocOutlineDocumentSource() = default;
	[[nodiscard]] virtual CLogicInt GetLineCount() const noexcept = 0;
	[[nodiscard]] virtual const wchar_t* GetLine( CLogicInt line, CLogicInt* length ) const noexcept = 0;
	[[nodiscard]] virtual bool TryLogicToLayout(
		const CLogicPoint& logic,
		CLayoutPoint* layout ) const noexcept = 0;
	[[nodiscard]] virtual const wchar_t* GetFilePath() const noexcept = 0;
	[[nodiscard]] virtual const wchar_t* GetOutlineRuleFileName() const noexcept = 0;
	[[nodiscard]] virtual int GetNewLineLength() const noexcept = 0;
	[[nodiscard]] virtual bool IsBookmarked( CLogicInt line ) const noexcept = 0;
	[[nodiscard]] virtual bool IsExtEolEnabled() const noexcept = 0;
	[[nodiscard]] virtual bool ShouldMarkBlankLines() const noexcept = 0;
	[[nodiscard]] virtual const wchar_t* GetCppAnonymousName() const noexcept = 0;
	[[nodiscard]] virtual const wchar_t* GetCppDefinitionPosition() const noexcept = 0;
	[[nodiscard]] virtual const wchar_t* GetJavaDefinitionPosition() const noexcept = 0;
};

class CDocOutline{
public:
	explicit CDocOutline(CEditDoc* pcDoc);
	explicit CDocOutline(
		const workbench::outline::OutlineDocumentSnapshot& snapshot,
		std::function<bool()> cancellationCheck = {});
	~CDocOutline();
	CDocOutline(CDocOutline&&) noexcept;
	CDocOutline& operator=(CDocOutline&&) noexcept;
	CDocOutline(const CDocOutline&) = delete;
	CDocOutline& operator=(const CDocOutline&) = delete;
	void	MakeFuncList_C( CFuncInfoArr* pcFuncInfoArr,
							EOutlineType& nOutlineType,
						    const WCHAR* pszFileName,
							bool bVisibleMemberFunc = true );					//!< C/C++関数リスト作成
	void	MakeFuncList_PLSQL( CFuncInfoArr* pcFuncInfoArr );					//!< PL/SQL関数リスト作成
	void	MakeTopicList_txt( CFuncInfoArr* pcFuncInfoArr );					//!< テキスト・トピックリスト作成
	void	MakeFuncList_Java( CFuncInfoArr* pcFuncInfoArr );					//!< Java関数リスト作成
	void	MakeTopicList_cobol( CFuncInfoArr* pcFuncInfoArr );					//!< COBOL アウトライン解析
	void	MakeTopicList_asm( CFuncInfoArr* pcFuncInfoArr );					//!< アセンブラ アウトライン解析
	void	MakeFuncList_Perl( CFuncInfoArr* pcFuncInfoArr );					//!< Perl関数リスト作成	//	Sep. 8, 2000 genta
	void	MakeFuncList_VisualBasic( CFuncInfoArr* pcFuncInfoArr );			//!< Visual Basic関数リスト作成 //June 23, 2001 N.Nakatani
	void	MakeFuncList_python( CFuncInfoArr* pcFuncInfoArr );					//!< Python アウトライン解析 // 2007.02.08 genta
	void	MakeFuncList_Erlang( CFuncInfoArr* pcFuncInfoArr );					//!< Erlang アウトライン解析 // 2009.08.10 genta
	void	MakeTopicList_wztxt( CFuncInfoArr* pcFuncInfoArr );					//!< 階層付きテキスト アウトライン解析 // 2003.05.20 zenryaku
	void	MakeTopicList_html(CFuncInfoArr* pcFuncInfoArr, bool bXml);			//!< HTML アウトライン解析 // 2003.05.20 zenryaku
	void	MakeTopicList_tex(CFuncInfoArr* pcFuncInfoArr);						//!< TeX アウトライン解析 // 2003.07.20 naoh
	void	MakeFuncList_RuleFile( CFuncInfoArr* pcFuncInfoArr,
								   std::wstring& sTitleOverride );				//!< ルールファイルを使ってリスト作成 2002.04.01 YAZAKI
	int		ReadRuleFile( const WCHAR* pszFilename, SOneRule* pcOneRule,
						  int nMaxCount, bool& bRegex, std::wstring& title );	//!< ルールファイル読込 2002.04.01 YAZAKI
	void	MakeFuncList_BookMark( CFuncInfoArr* );								//!< ブックマークリスト作成 //2001.12.03 hor

	// Parser-facing value accessors.  They are backed either by the live document
	// adapter or by an immutable workbench snapshot; parser implementations do not
	// need to know which one is active.
	[[nodiscard]] CLogicInt GetLineCount() const noexcept;
	[[nodiscard]] const wchar_t* GetLine( CLogicInt line, CLogicInt* length ) const noexcept;
	[[nodiscard]] bool TryLogicToLayout( const CLogicPoint& logic, CLayoutPoint* layout ) const noexcept;
	[[nodiscard]] const wchar_t* GetFilePath() const noexcept;
	[[nodiscard]] const wchar_t* GetOutlineRuleFileName() const noexcept;
	[[nodiscard]] int GetNewLineLength() const noexcept;
	[[nodiscard]] bool IsBookmarked( CLogicInt line ) const noexcept;
	[[nodiscard]] bool IsExtEolEnabled() const noexcept;
	[[nodiscard]] bool ShouldMarkBlankLines() const noexcept;
	[[nodiscard]] const wchar_t* GetCppAnonymousName() const noexcept;
	[[nodiscard]] const wchar_t* GetCppDefinitionPosition() const noexcept;
	[[nodiscard]] const wchar_t* GetJavaDefinitionPosition() const noexcept;
	[[nodiscard]] bool IsCancellationRequested() const noexcept;

private:
	// Legacy Outline implementations in sakura_core/types are member-function
	// definitions of CDocOutline and still require the live document.  Snapshot
	// parsing is deliberately limited to the C/C++ and Java implementations,
	// which use the source accessors above instead of this pointer.
	CEditDoc* m_pcDocRef = nullptr;
	std::unique_ptr<CDocOutlineDocumentSource> m_source;
	std::function<bool()> m_cancellationCheck;
};
#endif /* SAKURA_CDOCOUTLINE_BDF55702_D938_432D_99F2_BF0F98A7C5FE_H_ */
