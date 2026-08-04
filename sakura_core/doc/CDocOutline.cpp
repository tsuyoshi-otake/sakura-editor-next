/*!	@file
	@brief アウトライン解析

	@author genta
	@date	2004.08.08 作成
*/
/*
	Copyright (C) 1998-2001, Norio Nakatani
	Copyright (C) 2000, genta
	Copyright (C) 2001, genta
	Copyright (C) 2002, frozen
	Copyright (C) 2003, zenryaku
	Copyright (C) 2005, genta, D.S.Koba, じゅうじ
	Copyright (C) 2018-2022, Sakura Editor Organization

	This source code is designed for sakura editor.
	Please contact the copyright holders to use this code for other purpose.
*/

#include "StdAfx.h"
#include <string.h>
#include <limits>
#include <memory>
#include <utility>
#include "doc/CDocOutline.h"
#include "doc/CEditDoc.h"
#include "doc/logic/CDocLine.h"
#include "workbench/outline/OutlineDocumentSnapshot.h"
#include "_main/global.h"
#include "outline/CFuncInfoArr.h"
#include "outline/CFuncInfo.h"
#include "charset/charcode.h"
#include "io/CTextStream.h"
#include "extmodule/CBregexp.h"
#include "CSelectLang.h"
#include "config/system_constants.h"

/*! ルールファイルの1行を管理する構造体

	@date 2002.04.01 YAZAKI
	@date 2007.11.29 kobake 名前変更: oneRule→SOneRule
*/
struct SOneRule {
	wchar_t szMatch[256];
	int		nLength;
	wchar_t szText[256]; // RegexReplace時の置換後文字列
	wchar_t szGroupName[256];
	int		nLv;
	int		nRegexOption;
	int		nRegexMode; // 0 ==「Mode=Regex」, 1 == 「Mode=RegexReplace」
};

namespace {

class LiveDocumentSource final : public CDocOutlineDocumentSource {
public:
	explicit LiveDocumentSource( CEditDoc* document ) noexcept
		: m_document(document)
	{
	}

	[[nodiscard]] CLogicInt GetLineCount() const noexcept override
	{
		return m_document != nullptr ? m_document->m_cDocLineMgr.GetLineCount() : CLogicInt(0);
	}

	[[nodiscard]] const wchar_t* GetLine( CLogicInt line, CLogicInt* length ) const noexcept override
	{
		if( length != nullptr ) *length = CLogicInt(0);
		if( m_document == nullptr ) return nullptr;
		const auto* docLine = m_document->m_cDocLineMgr.GetLine(line);
		return docLine != nullptr ? docLine->GetDocLineStrWithEOL(length) : nullptr;
	}

	[[nodiscard]] bool TryLogicToLayout( const CLogicPoint& logic, CLayoutPoint* layout ) const noexcept override
	{
		if( layout == nullptr ) return false;
		*layout = CLayoutPoint(0, 0);
		if( m_document == nullptr ) return false;
		m_document->m_cLayoutMgr.LogicToLayout(logic, layout);
		return true;
	}

	[[nodiscard]] const wchar_t* GetFilePath() const noexcept override
	{
		return m_document != nullptr ? m_document->m_cDocFile.GetFilePath() : L"";
	}

	[[nodiscard]] const wchar_t* GetOutlineRuleFileName() const noexcept override
	{
		return m_document != nullptr
			? m_document->m_cDocType.GetDocumentAttribute().m_szOutlineRuleFilename
			: L"";
	}

	[[nodiscard]] int GetNewLineLength() const noexcept override
	{
		return m_document != nullptr ? m_document->m_cDocEditor.m_cNewLineCode.GetLen() : 0;
	}

	[[nodiscard]] bool IsBookmarked( CLogicInt line ) const noexcept override
	{
		return m_document != nullptr
			&& CBookmarkGetter(m_document->m_cDocLineMgr.GetLine(line)).IsBookmarked();
	}

	[[nodiscard]] bool IsExtEolEnabled() const noexcept override
	{
		return GetDllShareData().m_Common.m_sEdit.m_bEnableExtEol;
	}

	[[nodiscard]] bool ShouldMarkBlankLines() const noexcept override
	{
		return GetDllShareData().m_Common.m_sOutline.m_bMarkUpBlankLineEnable != FALSE;
	}

	[[nodiscard]] const wchar_t* GetCppAnonymousName() const noexcept override
	{
		return LS(STR_OUTLINE_CPP_NONAME);
	}

	[[nodiscard]] const wchar_t* GetCppDefinitionPosition() const noexcept override
	{
		return LS(STR_OUTLINE_CPP_DEFPOS);
	}

	[[nodiscard]] const wchar_t* GetJavaDefinitionPosition() const noexcept override
	{
		return LS(STR_OUTLINE_JAVA_DEFPOS);
	}

private:
	CEditDoc* m_document = nullptr;
};

class SnapshotDocumentSource final : public CDocOutlineDocumentSource {
public:
	explicit SnapshotDocumentSource( const workbench::outline::OutlineDocumentSnapshot& snapshot ) noexcept
		: m_snapshot(snapshot)
	{
	}

	[[nodiscard]] CLogicInt GetLineCount() const noexcept override
	{
		return CLogicInt(m_snapshot.LineCount());
	}

	[[nodiscard]] const wchar_t* GetLine( CLogicInt line, CLogicInt* length ) const noexcept override
	{
		if( length != nullptr ) *length = CLogicInt(0);
		if( line < 0 || static_cast<std::size_t>(line) >= m_snapshot.lineSpans.size() ) return nullptr;
		const auto span = m_snapshot.lineSpans[static_cast<std::size_t>(line)];
		if( span.offset > m_snapshot.textWithEol.size()
			|| span.length > m_snapshot.textWithEol.size() - span.offset
			|| span.length > static_cast<std::size_t>((std::numeric_limits<int>::max)())
			|| span.offset + span.length >= m_snapshot.textWithEol.size()
			|| m_snapshot.textWithEol[span.offset + span.length] != L'\0' ) return nullptr;
		if( length != nullptr ) *length = CLogicInt(span.length);
		return m_snapshot.textWithEol.data() + span.offset;
	}

	[[nodiscard]] bool TryLogicToLayout(
		[[maybe_unused]] const CLogicPoint& logic,
		[[maybe_unused]] CLayoutPoint* layout ) const noexcept override
	{
		// Snapshot parsing is explicitly logical-only.  The worker must not ask
		// for layout coordinates; the UI commit maps only returned symbol rows.
		return false;
	}

	[[nodiscard]] const wchar_t* GetFilePath() const noexcept override
	{
		return m_snapshot.filePath.c_str();
	}

	[[nodiscard]] const wchar_t* GetOutlineRuleFileName() const noexcept override
	{
		return L"";
	}

	[[nodiscard]] int GetNewLineLength() const noexcept override
	{
		return 0;
	}

	[[nodiscard]] bool IsBookmarked( CLogicInt line ) const noexcept override
	{
		(void)line;
		return false;
	}

	[[nodiscard]] bool IsExtEolEnabled() const noexcept override
	{
		return m_snapshot.extendedLineDelimiters;
	}

	[[nodiscard]] bool ShouldMarkBlankLines() const noexcept override
	{
		return false;
	}

	[[nodiscard]] const wchar_t* GetCppAnonymousName() const noexcept override
	{
		return m_snapshot.cppAnonymousName.c_str();
	}

	[[nodiscard]] const wchar_t* GetCppDefinitionPosition() const noexcept override
	{
		return m_snapshot.cppDefinitionPosition.c_str();
	}

	[[nodiscard]] const wchar_t* GetJavaDefinitionPosition() const noexcept override
	{
		return m_snapshot.javaDefinitionPosition.c_str();
	}

private:
	const workbench::outline::OutlineDocumentSnapshot& m_snapshot;
};

} // namespace

CDocOutline::CDocOutline( CEditDoc* pcDoc )
	: m_pcDocRef(pcDoc)
	, m_source(std::make_unique<LiveDocumentSource>(pcDoc))
{
}

CDocOutline::CDocOutline(
	const workbench::outline::OutlineDocumentSnapshot& snapshot,
	std::function<bool()> cancellationCheck )
	: m_source(std::make_unique<SnapshotDocumentSource>(snapshot))
	, m_cancellationCheck(std::move(cancellationCheck))
{
}

CDocOutline::~CDocOutline() = default;
CDocOutline::CDocOutline(CDocOutline&&) noexcept = default;
CDocOutline& CDocOutline::operator=(CDocOutline&&) noexcept = default;

CLogicInt CDocOutline::GetLineCount() const noexcept
{
	return m_source != nullptr ? m_source->GetLineCount() : CLogicInt(0);
}

const wchar_t* CDocOutline::GetLine( CLogicInt line, CLogicInt* length ) const noexcept
{
	return m_source != nullptr ? m_source->GetLine(line, length) : nullptr;
}

bool CDocOutline::TryLogicToLayout( const CLogicPoint& logic, CLayoutPoint* layout ) const noexcept
{
	return m_source != nullptr && m_source->TryLogicToLayout(logic, layout);
}

const wchar_t* CDocOutline::GetFilePath() const noexcept
{
	return m_source != nullptr ? m_source->GetFilePath() : L"";
}

const wchar_t* CDocOutline::GetOutlineRuleFileName() const noexcept
{
	return m_source != nullptr ? m_source->GetOutlineRuleFileName() : L"";
}

int CDocOutline::GetNewLineLength() const noexcept
{
	return m_source != nullptr ? m_source->GetNewLineLength() : 0;
}

bool CDocOutline::IsBookmarked( CLogicInt line ) const noexcept
{
	return m_source != nullptr && m_source->IsBookmarked(line);
}

bool CDocOutline::IsExtEolEnabled() const noexcept
{
	return m_source != nullptr && m_source->IsExtEolEnabled();
}

bool CDocOutline::ShouldMarkBlankLines() const noexcept
{
	return m_source != nullptr && m_source->ShouldMarkBlankLines();
}

const wchar_t* CDocOutline::GetCppAnonymousName() const noexcept
{
	return m_source != nullptr ? m_source->GetCppAnonymousName() : L"";
}

const wchar_t* CDocOutline::GetCppDefinitionPosition() const noexcept
{
	return m_source != nullptr ? m_source->GetCppDefinitionPosition() : L"";
}

const wchar_t* CDocOutline::GetJavaDefinitionPosition() const noexcept
{
	return m_source != nullptr ? m_source->GetJavaDefinitionPosition() : L"";
}

bool CDocOutline::IsCancellationRequested() const noexcept
{
	return m_cancellationCheck && m_cancellationCheck();
}

/*! ルールファイルを読み込み、ルール構造体の配列を作成する

	@date 2002.04.01 YAZAKI
	@date 2002.11.03 Moca 引数nMaxCountを追加。バッファ長チェックをするように変更
	@date 2013.06.02 _wfopen_absini,fgetwsをCTextInputStream_AbsIniに変更。UTF-8対応。Regex対応
	@date 2014.06.20 RegexReplace 正規表現置換モード追加
*/
int CDocOutline::ReadRuleFile( const WCHAR* pszFilename, SOneRule* pcOneRule, int nMaxCount, bool& bRegex, std::wstring& title )
{
	// 2003.06.23 Moca 相対パスは実行ファイルからのパスとして開く
	// 2007.05.19 ryoji 相対パスは設定ファイルからのパスを優先
	CTextInputStream_AbsIni	file = CTextInputStream_AbsIni( pszFilename );
	if( !file.Good() ){
		return 0;
	}
	std::wstring	strLine;
	wchar_t			szLine[LINEREADBUFSIZE];
	wchar_t			szText[256];
	const wchar_t*	pszDelimit = L" /// ";
	const wchar_t*	pszKeySeps = L",\0";
	const wchar_t*	pszWork;
	wchar_t	cComment = L';';
	auto nDelimitLen = int(wcslen(pszDelimit));
	int nCount = 0;
	bRegex = false;
	bool bRegexReplace = false;
	title.clear();
	int regexOption = CBregexp::optCaseSensitive;

	// 通常モード
	// key1,key2 /// GroupName,Lv=1
	// 正規表現モード
	// RegexMode /// GroupName,Lv=1
	// 正規表現置換モード
	// RegexReplace /// TitleReplace /// GroupName
	while( file.Good() && nCount < nMaxCount ){
		strLine = file.ReadLineW();
		pszWork = wcsstr( strLine.c_str(), pszDelimit );
		if( nullptr != pszWork && 0 < strLine.length() && strLine[0] != cComment ){
			int nLen = int(pszWork - strLine.c_str());
			if( nLen < LINEREADBUFSIZE ){
				// szLine == 「key1,key2」
				wmemcpy(szLine, strLine.c_str(), nLen);
				szLine[nLen] = L'\0';
			}else{
				// この行は長すぎる
				continue;
			}
			pszWork += nDelimitLen;

			/* 最初のトークンを取得します。 */
			const wchar_t* pszTextReplace = L"";
			wchar_t* pszToken;
			wchar_t* context{ nullptr };
			bool bTopDummy = false;
			bool bRegexRep2 = false;
			if( bRegex ){
				// regexのときは,区切りにしない
				pszToken = szLine;
				if( szLine[0] == L'\0' ){
					if( 0 < nCount ){
						// 空のKey は無視
						pszToken = nullptr;
					}else{
						// 最初の要素が空のKeyだったらダミー要素
						bTopDummy = true;
					}
				}
				if( bRegexReplace && pszToken ){
					const wchar_t* pszGroupDel = wcsstr( pszWork, pszDelimit );
					if( nullptr != pszGroupDel && 0 < pszWork[0] != L'\0' ){
						// pszWork = 「titleRep /// group」
						// pszGroupDel = 「 /// group」
						int nTitleLen = int(pszGroupDel - pszWork); // Len == 0 OK
						if( nTitleLen < int(std::size(szText)) ){
							wcsncpy_s(szText, std::size(szText), pszWork, nTitleLen);
						}else{
							wcsncpy_s(szText, std::size(szText), pszWork, _TRUNCATE);
						}
						pszTextReplace = szText;
						bRegexRep2 = true;
						pszWork = pszGroupDel + nDelimitLen; // group
					}
				}
			}else{
				pszToken = wcstok_s( szLine, pszKeySeps, &context );
				if( nCount == 0 && pszToken == nullptr ){
					pszToken = szLine;
					bTopDummy = true;
				}
			}
			const WCHAR* p = wcsstr( pszWork, L",Lv=" );
			int nLv = 0;
			if( p ){
				nLv = _wtoi( p + 4 );
			}
			while( nullptr != pszToken ){
				wcsncpy( pcOneRule[nCount].szMatch, pszToken, 255 );
				wcsncpy_s( pcOneRule[nCount].szText, _countof(pcOneRule[0].szText), pszTextReplace, _TRUNCATE );
				wcsncpy( pcOneRule[nCount].szGroupName, pszWork, 255 );
				pcOneRule[nCount].szMatch[255] = L'\0';
				pcOneRule[nCount].szGroupName[255] = L'\0';
				pcOneRule[nCount].nLv = nLv;
				pcOneRule[nCount].nLength = (int)wcslen(pcOneRule[nCount].szMatch);
				pcOneRule[nCount].nRegexOption = regexOption;
				pcOneRule[nCount].nRegexMode = bRegexRep2 ? 1 : 0; // 文字列が正しい時だけReplaceMode
				nCount++;
				if( bTopDummy || bRegex ){
					pszToken = nullptr;
				}else{
					pszToken = wcstok_s( nullptr, pszKeySeps, &context );
				}
			}
		}else{
			if( 0 < strLine.length() && strLine[0] == cComment ){
				if( 13 <= strLine.length() && strLine.length() <= 14 && 0 == wcsnicmp_literal( strLine.c_str() + 1, L"CommentChar=" ) ){
					if( 13 == strLine.length() ){
						cComment = L'\0';
					}else{
						cComment = strLine[13];
					}
				}else if( 11 == strLine.length() && 0 == _wcsicmp( strLine.c_str() + 1, L"Mode=Regex" ) ){
					bRegex = true;
					bRegexReplace = false;
				}else if( 18 == strLine.length() && 0 == _wcsicmp( strLine.c_str() + 1, L"Mode=RegexReplace" ) ){
					bRegex = true;
					bRegexReplace = true;
				}else if( 7 <= strLine.length() && 0 == wcsnicmp_literal( strLine.c_str() + 1, L"Title=" ) ){
					title = strLine.c_str() + 7;
				}else if( 13 < strLine.length() && 0 == wcsnicmp_literal( strLine.c_str() + 1, L"RegexOption=" ) ){
					int nCaseFlag = CBregexp::optCaseSensitive;
					regexOption = 0;
					for( int i = 13; i < (int)strLine.length(); i++ ){
						if( strLine[i] == L'i' ){
							nCaseFlag = 0;
						}else if( strLine[i] == L'g' ){
							regexOption |= CBregexp::optGlobal;
						}else if( strLine[i] == L'x' ){
							regexOption |= CBregexp::optExtend;
						}else if( strLine[i] == L'a' ){
							regexOption |= CBregexp::optASCII;
						}else if( strLine[i] == L'u' ){
							regexOption |= CBregexp::optUnicode;
						}else if( strLine[i] == L'd' ){
							regexOption |= CBregexp::optDefault;
						}else if( strLine[i] == L'l' ){
							regexOption |= CBregexp::optLocale;
						}else if( strLine[i] == L'R' ){
							regexOption |= CBregexp::optR;
						}
					}
					regexOption |= nCaseFlag;
				}
			}
		}
	}
	file.Close();
	return nCount;
}

/*! ルールファイルを元に、トピックリストを作成

	@date 2002.04.01 YAZAKI
	@date 2002.11.03 Moca ネストの深さが最大値を超えるとバッファオーバーランするのを修正
		最大値以上は追加せずに無視する
	@date 2007.11.29 kobake SOneRule test[1024] でスタックが溢れていたのを修正
*/
void CDocOutline::MakeFuncList_RuleFile( CFuncInfoArr* pcFuncInfoArr, std::wstring& sTitleOverride )
{
	/* ルールファイルの内容をバッファに読み込む */
	auto test = std::make_unique<SOneRule[]>(1024);	// 1024個許可。 2007.11.29 kobake スタック使いすぎなので、ヒープに確保するように修正。
	bool bRegex;
	std::wstring title;
	int nCount = ReadRuleFile(GetOutlineRuleFileName(), test.get(), 1024, bRegex, title );
	if ( nCount < 1 ){
		return;
	}
	if( 0 < title.size() ){
		sTitleOverride = title;
	}

	/*	ネストの深さは、32レベルまで、ひとつのヘッダーは、最長256文字まで区別
		（256文字まで同じだったら同じものとして扱います）
	*/
	const int	nMaxStack = 32;	//	ネストの最深
	int			nDepth = 0;				//	いまのアイテムの深さを表す数値。
	wchar_t		pszStack[nMaxStack][256];
	wchar_t		nLvStack[nMaxStack];
	wchar_t		szTitle[256];			//	一時領域
	CBregexp*	pRegex = nullptr;
	if( bRegex ){
		pRegex = new CBregexp[nCount];
		for( int i = 0; i < nCount; i++ ){
			if( 0 == test[i].nLength ){
				continue;
			}
			if( !InitRegexp( nullptr, pRegex[i], true ) ){
				delete [] pRegex;
				return;
			}
			if( test[i].nRegexMode == 1 ){
				if( !pRegex[i].Compile(test[i].szMatch, test[i].szText, test[i].nRegexOption) ){
					std::wstring str = test[i].szMatch;
					str += L"\n";
					str += test[i].szText;
					ErrorMessage( nullptr, LS(STR_DOCOUTLINE_REGEX),
						str.c_str(),
						pRegex[i].GetLastMessage()
					);
					delete [] pRegex;
					return;
				}
			}else if( !pRegex[i].Compile(test[i].szMatch, test[i].nRegexOption) ){
				ErrorMessage( nullptr, LS(STR_DOCOUTLINE_REGEX),
					test[i].szMatch,
					pRegex[i].GetLastMessage()
				);
				delete [] pRegex;
				return;
			}
		}
	}
	// 1つめが空行だった場合は、ルート要素とする
	// 項目名はグループ名
	if( test[0].nLength == 0 ){
		const wchar_t* g = test[0].szGroupName;
		wcscpy(pszStack[0], g);
		nLvStack[0] = wchar_t(test[0].nLv);
		const wchar_t *p = wcschr(g, L',');
		int len;
		if( p != nullptr ){
			len = int(p - g);
		}else{
			len = (int)wcslen(g);
		}
		CNativeW mem;
		mem.SetString(g, len);
		pcFuncInfoArr->AppendData( CLogicInt(1), CLayoutInt(1), mem.GetStringPtr(), FUNCINFO_NOCLIPTEXT, nDepth );
		nDepth = 1;
	}
	for( CLogicInt nLineCount = CLogicInt(0); nLineCount < GetLineCount(); ++nLineCount )
	{
		//行取得
		CLogicInt		nLineLen;
		const wchar_t*	pLine = GetLine(nLineCount, &nLineLen);
		if( nullptr == pLine ){
			break;
		}

		//行頭の空白飛ばし
		int		i = 0;
		if( !bRegex ){
			for( i = 0; i < nLineLen; ++i ){
				if( pLine[i] == L' ' || pLine[i] == L'\t' || pLine[i] == L'　'){
					continue;
				}
				break;
			}
			if( i >= nLineLen ){
				continue;
			}
		}

		//先頭文字が見出し記号のいずれかであれば、次へ進む
		const wchar_t*		pszText = nullptr;
		std::wstring strText;
		int		j;
		for( j = 0; j < nCount; j++ ){
			if( bRegex ){
				if( test[j].nRegexMode == 0 ){
					if( 0 < test[j].nLength && pRegex[j].Match( pLine, nLineLen, 0 ) ){
						wcscpy( szTitle, test[j].szGroupName );
						break;
					}
				}else{
					if( 0 < test[j].nLength && 0 < pRegex[j].Replace( pLine, nLineLen, 0 ) ){
						// pLine = "ABC123DEF"
						// testのszMatch = "\d+"
						// testのszText = "$&456"
						// GetString() = "ABC123456DEF"
						// pszText = "123456"
						int nIndex = pRegex[j].GetIndex();
						int nMatchLen = pRegex[j].GetMatchLen();
						int nTextLen = pRegex[j].GetStringLen() - nLineLen + nMatchLen;
						strText.assign( pRegex[j].GetString() + nIndex, nTextLen );
						pszText = strText.c_str();
						wcscpy( szTitle, test[j].szGroupName );
						break;
					}
				}
			}else{
				if ( 0 < test[j].nLength && 0 == wcsncmp( &pLine[i], test[j].szMatch, test[j].nLength ) ){
					wcscpy( szTitle, test[j].szGroupName );
					break;
				}
			}
		}
		if( j >= nCount ){
			continue;
		}
		if( 0 == wcscmp( szTitle, L"Except" ) ){
			continue;
		}

		/*	ルールにマッチした行は、アウトライン結果に表示する。
		*/

		//行文字列から改行を取り除く pLine -> pszText
		// 正規表現置換のときは設定済み
		if( nullptr == pszText ){
			pszText = &pLine[i];
			nLineLen -= i;
			const bool bExtEol = IsExtEolEnabled();
			for( i = 0; i < nLineLen; ++i ){
				if( WCODE::IsLineDelimiter(pszText[i], bExtEol) ){
					break;
				}
			}
			strText.assign( pszText, i );
			pszText = strText.c_str();
		}

		/*
		  カーソル位置変換
		  物理位置(行頭からのバイト数、折り返し無し行位置)
		  →
		  レイアウト位置(行頭からの表示桁位置、折り返しあり行位置)
		*/
		CLayoutPoint ptPos;
		TryLogicToLayout(
			CLogicPoint(0, nLineCount),
			&ptPos
		);

		/* nDepthを計算 */
		int k;
		bool bAppend = true;
		for ( k = 0; k < nDepth; k++ ){
			int nResult = wcscmp( pszStack[k], szTitle );
			if ( nResult == 0 ){
				break;
			}
		}
		if ( k < nDepth ){
			//	ループ途中でbreak;してきた。＝今までに同じ見出しが存在していた。
			//	ので、同じレベルに合わせてAppendData.
			nDepth = k;
		}
		else if( nMaxStack > k ){
			//	いままでに同じ見出しが存在しなかった。
			//	Lvが高い場合は、一致するまでさかのぼる
			for ( k = nDepth - 1; 0 <= k ; k-- ){
				if ( nLvStack[k] <= test[j].nLv ){
					k++;
					break;
				}
			}
			if( k < 0 ){
				k = 0;
			}
			wcscpy(pszStack[k], szTitle);
			nLvStack[k] = wchar_t(test[j].nLv);
			nDepth = k;
		}else{
			// 2002.11.03 Moca 最大値を超えるとバッファオーバーランするから規制する
			// nDepth = nMaxStack;
			bAppend = false;
		}

		if( bAppend ){
			pcFuncInfoArr->AppendData( nLineCount + CLogicInt(1), ptPos.GetY2() + CLayoutInt(1) , pszText, 0, nDepth );
			nDepth++;
		}
	}
	delete [] pRegex;
	return;
}

/*! ブックマークリスト作成（無理矢理！）

	@date 2001.12.03 hor   新規作成
	@date 2002.01.19 aroka 空行をマーク対象にするフラグ bMarkUpBlankLineEnable を導入しました。
	@date 2005.10.11 ryoji "ａ@" の右２バイトが全角空白と判定される問題の対処
	@date 2005.11.03 genta 文字列長修正．右端のゴミを除去
*/
void CDocOutline::MakeFuncList_BookMark( CFuncInfoArr* pcFuncInfoArr )
{
	const wchar_t*	pLine;
	CLogicInt		nLineLen;
	CLogicInt		nLineCount;
	int		leftspace, pos_wo_space, k;
	BOOL	bMarkUpBlankLineEnable = ShouldMarkBlankLines();	//! 空行をマーク対象にするフラグ 20020119 aroka
	int		nNewLineLen	= GetNewLineLength();
	CLogicInt	nLineLast	= GetLineCount();
	int		nCharChars;

	for( nLineCount = CLogicInt(0); nLineCount <  nLineLast; ++nLineCount ){
		if(!IsBookmarked(nLineCount))continue;
		pLine = GetLine(nLineCount, &nLineLen);
		if( nullptr == pLine ){
			break;
		}
		// Jan, 16, 2002 hor
		if( bMarkUpBlankLineEnable ){// 20020119 aroka
			if( nLineLen<=nNewLineLen && nLineCount< nLineLast ){
			  continue;
			}
		}// LTrim
		for( leftspace = 0; leftspace < nLineLen; ++leftspace ){
			if( WCODE::IsBlank(pLine[leftspace]) ){
				continue;
			}
			break;
		}
		
		if( bMarkUpBlankLineEnable ){// 20020119 aroka
			if(( leftspace >= nLineLen-nNewLineLen && nLineCount< nLineLast )||
				( leftspace >= nLineLen )) {
				continue;
			}
		}// RTrim
		// 2005.10.11 ryoji 右から遡るのではなく左から探すように修正（"ａ@" の右２バイトが全角空白と判定される問題の対処）
		k = pos_wo_space = leftspace;
		bool bExtEol = IsExtEolEnabled();
		while( k < nLineLen ){
			nCharChars = CNativeW::GetSizeOfChar( pLine, nLineLen, k );
			if( 1 == nCharChars ){
				if( !(WCODE::IsLineDelimiter(pLine[k], bExtEol) ||
						pLine[k] == WCODE::SPACE ||
						pLine[k] == WCODE::TAB ||
						WCODE::IsZenkakuSpace(pLine[k]) ||
						pLine[k] == L'\0') )
					pos_wo_space = k + nCharChars;
			}
			k += nCharChars;
		}
		//	Nov. 3, 2005 genta 文字列長計算式の修正
		std::wstring strText( &pLine[leftspace], pos_wo_space - leftspace );

		CLayoutPoint ptXY;
		TryLogicToLayout( CLogicPoint(CLogicInt(0), nLineCount), &ptXY );
		pcFuncInfoArr->AppendData( nLineCount+CLogicInt(1), ptXY.GetY2()+CLayoutInt(1), strText.c_str(), 0 );
	}
	return;
}
