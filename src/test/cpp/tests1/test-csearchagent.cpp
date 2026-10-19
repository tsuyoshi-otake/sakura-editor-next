/*
	Copyright (C) 2021-2022, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"
#include "agent/CSaveAgent.h"

#include "agent/CSearchAgent.h"
#include "cmd/COpeBlk.h"
#include "util/CpuDispatch.h"
#include "util/string_ex.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <initializer_list>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "doc/logic/CDocLineMgr.h"
#include "textmate/TextMateLineCache.h"

namespace {

template <typename T> void SetLines(CDocLineMgr& m, int seq, T begin, T end)
{
	for (auto it = begin; it != end; ++it) {
		CDocLine* line = m.AddNewLine();
		line->SetDocLineString(it->data(), it->length());
		line->m_sMark.m_cModified = seq;
	}
}

void SetLines(CDocLineMgr& m, int seq, std::initializer_list<std::wstring_view> args)
{
	SetLines(m, seq, args.begin(), args.end());
}

struct RawLineData {
	const wchar_t* line;
	const int seq;
};

COpeLineData MakeOpeLineData(std::initializer_list<RawLineData> lines)
{
	COpeLineData data;
	for (RawLineData rawLine : lines) {
		CLineData line;
		line.cmemLine = rawLine.line;
		line.nSeq = rawLine.seq;
		data.push_back(line);
	}
	return data;
}

}

TEST(CDeleteOpe, Dump001)
{
	CDeleteOpe ope;

	// 呼ぶだけ
	ope.DUMP();
}

TEST(CInsertOpe, Dump001)
{
	CInsertOpe ope;

	// 呼ぶだけ
	ope.DUMP();
}

TEST(COpeBlk, AppendOpe001)
{
	COpeBlk blk;

	auto pOpe = new CReplaceOpe();
	pOpe->m_ptCaretPos_PHY_Before = CLogicPoint(0, 1);
	pOpe->m_ptCaretPos_PHY_After = CLogicPoint(0, 1);
	blk.AppendOpe(pOpe);

	// 呼ぶだけ
	blk.DUMP();
}

TEST(COpeBlk, AppendOpe101)
{
	COpeBlk blk;
	auto pOpe = new CMoveCaretOpe();
	blk.AppendOpe(pOpe);
}

/*!
	CSearchAgent::ReplaceData のテスト

	行の一部を置き換える。
 */
TEST(CSearchAgent, ReplaceData1)
{
	CDocLineMgr m;
	SetLines(m, 1, {L"AAA\n", L"BBB\n", L"CCC\n"});

	COpeLineData insData = MakeOpeLineData({{L"DDD", 2}});
	COpeLineData delData;

	DocLineReplaceArg arg;
	arg.sDelRange = CLogicRange(CLogicPoint(0, 1), CLogicPoint(3, 1));
	arg.pcmemDeleted = &delData;
	arg.pInsData = &insData;
	arg.nDelSeq = 1;
	arg.nInsSeq = -1;
	CSearchAgent(&m).ReplaceData(&arg, false);

	EXPECT_EQ(m.GetLineCount(), 3);
	EXPECT_STREQ(m.GetLine(CLogicInt(0))->GetPtr(), L"AAA\n");
	EXPECT_EQ(m.GetLine(CLogicInt(0))->m_sMark.m_cModified.GetSeq(), 1);
	EXPECT_STREQ(m.GetLine(CLogicInt(1))->GetPtr(), L"DDD\n");
	EXPECT_EQ(m.GetLine(CLogicInt(1))->m_sMark.m_cModified.GetSeq(), 2);
	EXPECT_STREQ(m.GetLine(CLogicInt(2))->GetPtr(), L"CCC\n");
	EXPECT_EQ(m.GetLine(CLogicInt(2))->m_sMark.m_cModified.GetSeq(), 1);

	EXPECT_EQ(arg.nInsSeq, 1);
	EXPECT_EQ(arg.pcmemDeleted->size(), 1);
	EXPECT_STREQ(arg.pcmemDeleted->at(0).cmemLine.GetStringPtr(), L"BBB");
	EXPECT_EQ(arg.pcmemDeleted->at(0).nSeq, 1);
	EXPECT_EQ(arg.nDeletedLineNum, 0);
	EXPECT_EQ(arg.nInsLineNum, 0);
	EXPECT_EQ(arg.ptNewPos, CLogicPoint(3, 1));
}

/*!
	CSearchAgent::ReplaceData のテスト

	行全体を置き換える。
 */
TEST(CSearchAgent, ReplaceData2)
{
	CDocLineMgr m;
	SetLines(m, 1, {L"AAA\n", L"BBB\n", L"CCC\n"});

	COpeLineData insData = MakeOpeLineData({{L"DDD\n", 2}});
	COpeLineData delData;

	DocLineReplaceArg arg;
	arg.sDelRange = CLogicRange(CLogicPoint(0, 1), CLogicPoint(4, 1));
	arg.pcmemDeleted = &delData;
	arg.pInsData = &insData;
	arg.nDelSeq = 1;
	arg.nInsSeq = -1;
	CSearchAgent(&m).ReplaceData(&arg, false);

	EXPECT_EQ(m.GetLineCount(), 3);
	EXPECT_STREQ(m.GetLine(CLogicInt(0))->GetPtr(), L"AAA\n");
	EXPECT_EQ(m.GetLine(CLogicInt(0))->m_sMark.m_cModified.GetSeq(), 1);
	EXPECT_STREQ(m.GetLine(CLogicInt(1))->GetPtr(), L"DDD\n");
	EXPECT_EQ(m.GetLine(CLogicInt(1))->m_sMark.m_cModified.GetSeq(), 2);
	EXPECT_STREQ(m.GetLine(CLogicInt(2))->GetPtr(), L"CCC\n");
	EXPECT_EQ(m.GetLine(CLogicInt(2))->m_sMark.m_cModified.GetSeq(), 1);

	EXPECT_EQ(arg.nInsSeq, 0);
	EXPECT_EQ(arg.pcmemDeleted->size(), 1);
	EXPECT_STREQ(arg.pcmemDeleted->at(0).cmemLine.GetStringPtr(), L"BBB\n");
	EXPECT_EQ(arg.pcmemDeleted->at(0).nSeq, 1);
	EXPECT_EQ(arg.nDeletedLineNum, 1);
	EXPECT_EQ(arg.nInsLineNum, 1);
	EXPECT_EQ(arg.ptNewPos, CLogicPoint(0, 2));
}

/*!
	CSearchAgent::ReplaceData のテスト

	行末の改行を削除する。
 */
TEST(CSearchAgent, ReplaceData3)
{
	CDocLineMgr m;
	SetLines(m, 1, {L"AAA\n", L"BBB\n", L"CCC\n"});

	COpeLineData delData;

	DocLineReplaceArg arg;
	arg.sDelRange = CLogicRange(CLogicPoint(3, 1), CLogicPoint(4, 1));
	arg.pcmemDeleted = &delData;
	arg.pInsData = nullptr;
	arg.nDelSeq = 1;
	arg.nInsSeq = -1;
	CSearchAgent(&m).ReplaceData(&arg, false);

	EXPECT_EQ(m.GetLineCount(), 2);
	EXPECT_STREQ(m.GetLine(CLogicInt(0))->GetPtr(), L"AAA\n");
	EXPECT_EQ(m.GetLine(CLogicInt(0))->m_sMark.m_cModified.GetSeq(), 1);
	EXPECT_STREQ(m.GetLine(CLogicInt(1))->GetPtr(), L"BBBCCC\n");
	EXPECT_EQ(m.GetLine(CLogicInt(1))->m_sMark.m_cModified.GetSeq(), 1);

	EXPECT_EQ(arg.nInsSeq, 0);
	EXPECT_EQ(arg.pcmemDeleted->size(), 2);
	EXPECT_STREQ(arg.pcmemDeleted->at(0).cmemLine.GetStringPtr(), L"\n");
	EXPECT_EQ(arg.pcmemDeleted->at(0).nSeq, 1);
	EXPECT_STREQ(arg.pcmemDeleted->at(1).cmemLine.GetStringPtr(), L"");
	EXPECT_EQ(arg.pcmemDeleted->at(1).nSeq, 1);
	EXPECT_EQ(arg.nDeletedLineNum, 1);
	EXPECT_EQ(arg.nInsLineNum, 0);
	EXPECT_EQ(arg.ptNewPos, CLogicPoint(3, 1));
}

/*!
	CSearchAgent::ReplaceData のテスト

	行末の改行を削除する。削除するデータ長が長いケース。
 */
TEST(CSearchAgent, ReplaceData4)
{
	CDocLineMgr m;
	SetLines(m, 1, {L"AAA\n", L"BBB\n", L"CCC\n"});

	COpeLineData delData;

	DocLineReplaceArg arg;
	arg.sDelRange = CLogicRange(CLogicPoint(1, 1), CLogicPoint(4, 1));
	arg.pcmemDeleted = &delData;
	arg.pInsData = nullptr;
	arg.nDelSeq = 1;
	arg.nInsSeq = -1;
	CSearchAgent(&m).ReplaceData(&arg, false);

	EXPECT_EQ(m.GetLineCount(), 2);
	EXPECT_STREQ(m.GetLine(CLogicInt(0))->GetPtr(), L"AAA\n");
	EXPECT_EQ(m.GetLine(CLogicInt(0))->m_sMark.m_cModified.GetSeq(), 1);
	EXPECT_STREQ(m.GetLine(CLogicInt(1))->GetPtr(), L"BCCC\n");
	EXPECT_EQ(m.GetLine(CLogicInt(1))->m_sMark.m_cModified.GetSeq(), 1);

	EXPECT_EQ(arg.nInsSeq, 0);
	EXPECT_EQ(arg.pcmemDeleted->size(), 2);
	EXPECT_STREQ(arg.pcmemDeleted->at(0).cmemLine.GetStringPtr(), L"BB\n");
	EXPECT_EQ(arg.pcmemDeleted->at(0).nSeq, 1);
	EXPECT_STREQ(arg.pcmemDeleted->at(1).cmemLine.GetStringPtr(), L"");
	EXPECT_EQ(arg.pcmemDeleted->at(1).nSeq, 1);
	EXPECT_EQ(arg.nDeletedLineNum, 1);
	EXPECT_EQ(arg.nInsLineNum, 0);
	EXPECT_EQ(arg.ptNewPos, CLogicPoint(1, 1));
}

/*!
	CSearchAgent::ReplaceData のテスト

	行末の改行を削除する。対象行がデータ末尾であるケース。
 */
TEST(CSearchAgent, ReplaceData5)
{
	CDocLineMgr m;
	SetLines(m, 1, {L"AAA\n"});

	COpeLineData delData;

	DocLineReplaceArg arg;
	arg.sDelRange = CLogicRange(CLogicPoint(3, 0), CLogicPoint(4, 0));
	arg.pcmemDeleted = &delData;
	arg.pInsData = nullptr;
	arg.nDelSeq = 1;
	arg.nInsSeq = -1;
	CSearchAgent(&m).ReplaceData(&arg, false);

	EXPECT_EQ(m.GetLineCount(), 1);
	EXPECT_STREQ(m.GetLine(CLogicInt(0))->GetPtr(), L"AAA");
	EXPECT_EQ(m.GetLine(CLogicInt(0))->m_sMark.m_cModified.GetSeq(), 1);

	EXPECT_EQ(arg.nInsSeq, 0);
	EXPECT_EQ(arg.pcmemDeleted->size(), 1);
	EXPECT_STREQ(arg.pcmemDeleted->at(0).cmemLine.GetStringPtr(), L"\n");
	EXPECT_EQ(arg.pcmemDeleted->at(0).nSeq, 1);
	EXPECT_EQ(arg.nDeletedLineNum, 0);
	EXPECT_EQ(arg.nInsLineNum, 0);
	EXPECT_EQ(arg.ptNewPos, CLogicPoint(3, 0));
}

/*!
	CSearchAgent::ReplaceData のテスト

	文字を挿入して複数行を連結する。
 */
TEST(CSearchAgent, ReplaceData6)
{
	CDocLineMgr m;
	SetLines(m, 1, {L"AAA\n", L"BBB\n", L"CCC\n"});

	COpeLineData insData = MakeOpeLineData({{L" ", 2}});
	COpeLineData delData;

	DocLineReplaceArg arg;
	arg.sDelRange = CLogicRange(CLogicPoint(2, 1), CLogicPoint(1, 2));
	arg.pcmemDeleted = &delData;
	arg.pInsData = &insData;
	arg.nDelSeq = 1;
	arg.nInsSeq = -1;
	CSearchAgent(&m).ReplaceData(&arg, false);

	EXPECT_EQ(m.GetLineCount(), 2);
	EXPECT_STREQ(m.GetLine(CLogicInt(0))->GetPtr(), L"AAA\n");
	EXPECT_EQ(m.GetLine(CLogicInt(0))->m_sMark.m_cModified.GetSeq(), 1);
	EXPECT_STREQ(m.GetLine(CLogicInt(1))->GetPtr(), L"BB CC\n");
	EXPECT_EQ(m.GetLine(CLogicInt(1))->m_sMark.m_cModified.GetSeq(), 2);

	EXPECT_EQ(arg.nInsSeq, 1);
	EXPECT_EQ(arg.pcmemDeleted->size(), 2);
	EXPECT_STREQ(arg.pcmemDeleted->at(0).cmemLine.GetStringPtr(), L"B\n");
	EXPECT_EQ(arg.pcmemDeleted->at(0).nSeq, 1);
	EXPECT_STREQ(arg.pcmemDeleted->at(1).cmemLine.GetStringPtr(), L"C");
	EXPECT_EQ(arg.pcmemDeleted->at(1).nSeq, 1);
	EXPECT_EQ(arg.nDeletedLineNum, 1);
	EXPECT_EQ(arg.nInsLineNum, 0);
	EXPECT_EQ(arg.ptNewPos, CLogicPoint(3, 1));
}

/*!
	CSearchAgent::ReplaceData のテスト

	既存行の間に行を挿入し、次の行の先頭に文字を挿入する。
	先頭に文字を挿入した行がデータの末尾であるケース。
 */
TEST(CSearchAgent, ReplaceData7)
{
	CDocLineMgr m;
	SetLines(m, 1, {L"AAA\n", L"BBB\n"});

	COpeLineData insData = MakeOpeLineData({{L"CCC\n", 2}, {L"DDD", 3}});
	COpeLineData delData;

	DocLineReplaceArg arg;
	arg.sDelRange = CLogicRange(CLogicPoint(0, 1), CLogicPoint(0, 1));
	arg.pcmemDeleted = &delData;
	arg.pInsData = &insData;
	arg.nDelSeq = 0;
	arg.nInsSeq = -1;
	CSearchAgent(&m).ReplaceData(&arg, false);

	EXPECT_EQ(m.GetLineCount(), 3);
	EXPECT_STREQ(m.GetLine(CLogicInt(0))->GetPtr(), L"AAA\n");
	EXPECT_EQ(m.GetLine(CLogicInt(0))->m_sMark.m_cModified.GetSeq(), 1);
	EXPECT_STREQ(m.GetLine(CLogicInt(1))->GetPtr(), L"CCC\n");
	EXPECT_EQ(m.GetLine(CLogicInt(1))->m_sMark.m_cModified.GetSeq(), 2);
	EXPECT_STREQ(m.GetLine(CLogicInt(2))->GetPtr(), L"DDDBBB\n");
	EXPECT_EQ(m.GetLine(CLogicInt(2))->m_sMark.m_cModified.GetSeq(), 3);

	EXPECT_EQ(arg.nInsSeq, 1);
	EXPECT_TRUE(arg.pcmemDeleted->empty());
	EXPECT_EQ(arg.nDeletedLineNum, 0);
	EXPECT_EQ(arg.nInsLineNum, 1);
	EXPECT_EQ(arg.ptNewPos, CLogicPoint(3, 2));
}

/*!
	CSearchAgent::ReplaceData のテスト

	既存行の間に行を挿入し、次の行の先頭に文字を挿入する。
	先頭に文字を挿入した行がデータの末尾ではないケース。
 */
TEST(CSearchAgent, ReplaceData8)
{
	CDocLineMgr m;
	SetLines(m, 1, {L"AAA\n", L"BBB\n", L"CCC\n"});

	COpeLineData insData = MakeOpeLineData({{L"DDD\n", 2}, {L"EEE", 3}});
	COpeLineData delData;

	DocLineReplaceArg arg;
	arg.sDelRange = CLogicRange(CLogicPoint(0, 1), CLogicPoint(0, 1));
	arg.pcmemDeleted = &delData;
	arg.pInsData = &insData;
	arg.nDelSeq = 0;
	arg.nInsSeq = -1;
	CSearchAgent(&m).ReplaceData(&arg, false);

	EXPECT_EQ(m.GetLineCount(), 4);
	EXPECT_STREQ(m.GetLine(CLogicInt(0))->GetPtr(), L"AAA\n");
	EXPECT_EQ(m.GetLine(CLogicInt(0))->m_sMark.m_cModified.GetSeq(), 1);
	EXPECT_STREQ(m.GetLine(CLogicInt(1))->GetPtr(), L"DDD\n");
	EXPECT_EQ(m.GetLine(CLogicInt(1))->m_sMark.m_cModified.GetSeq(), 2);
	EXPECT_STREQ(m.GetLine(CLogicInt(2))->GetPtr(), L"EEEBBB\n");
	EXPECT_EQ(m.GetLine(CLogicInt(2))->m_sMark.m_cModified.GetSeq(), 3);
	EXPECT_STREQ(m.GetLine(CLogicInt(3))->GetPtr(), L"CCC\n");
	EXPECT_EQ(m.GetLine(CLogicInt(3))->m_sMark.m_cModified.GetSeq(), 1);

	EXPECT_EQ(arg.nInsSeq, 1);
	EXPECT_TRUE(arg.pcmemDeleted->empty());
	EXPECT_EQ(arg.nDeletedLineNum, 0);
	EXPECT_EQ(arg.nInsLineNum, 1);
	EXPECT_EQ(arg.ptNewPos, CLogicPoint(3, 2));
}

/*!
	CSearchAgent::ReplaceData のテスト

	既存行の末尾に新しい行を追加する。
 */
TEST(CSearchAgent, ReplaceData9)
{
	CDocLineMgr m;
	SetLines(m, 1, {L"AAA\n", L"BBB\n"});

	COpeLineData insData = MakeOpeLineData({{L"CCC\n", 2}});
	COpeLineData delData;

	DocLineReplaceArg arg;
	arg.sDelRange = CLogicRange(CLogicPoint(0, 2), CLogicPoint(0, 2));
	arg.pcmemDeleted = &delData;
	arg.pInsData = &insData;
	arg.nDelSeq = 0;
	arg.nInsSeq = -1;
	CSearchAgent(&m).ReplaceData(&arg, false);

	EXPECT_EQ(m.GetLineCount(), 3);
	EXPECT_STREQ(m.GetLine(CLogicInt(0))->GetPtr(), L"AAA\n");
	EXPECT_EQ(m.GetLine(CLogicInt(0))->m_sMark.m_cModified.GetSeq(), 1);
	EXPECT_STREQ(m.GetLine(CLogicInt(1))->GetPtr(), L"BBB\n");
	EXPECT_EQ(m.GetLine(CLogicInt(1))->m_sMark.m_cModified.GetSeq(), 1);
	EXPECT_STREQ(m.GetLine(CLogicInt(2))->GetPtr(), L"CCC\n");
	EXPECT_EQ(m.GetLine(CLogicInt(2))->m_sMark.m_cModified.GetSeq(), 2);

	EXPECT_EQ(arg.nInsSeq, 0);
	EXPECT_TRUE(arg.pcmemDeleted->empty());
	EXPECT_EQ(arg.nDeletedLineNum, 0);
	EXPECT_EQ(arg.nInsLineNum, 1);
	EXPECT_EQ(arg.ptNewPos, CLogicPoint(0, 3));
}

/*!
	CSearchAgent::ReplaceData のテスト

	置換後の文字列が既存の行バッファの有効長に収まる場合の最適化済みコードパスの検査。
 */
TEST(CSearchAgent, ReplaceData10)
{
	CDocLineMgr m;
	SetLines(m, 1, {L"0123456789\n"});
	m.GetLine(CLogicInt(0))->_GetDocLineData().AllocStringBuffer(15);

	COpeLineData insData = MakeOpeLineData({{L"0123", 2}});
	COpeLineData delData;

	DocLineReplaceArg arg;
	arg.sDelRange = CLogicRange(CLogicPoint(9, 0), CLogicPoint(10, 0));
	arg.pcmemDeleted = &delData;
	arg.pInsData = &insData;
	arg.nDelSeq = 0;
	arg.nInsSeq = -1;
	CSearchAgent(&m).ReplaceData(&arg, false);

	EXPECT_EQ(m.GetLineCount(), 1);
	EXPECT_STREQ(m.GetLine(CLogicInt(0))->GetPtr(), L"0123456780123\n");
	EXPECT_EQ(m.GetLine(CLogicInt(0))->m_sMark.m_cModified.GetSeq(), 2);

	EXPECT_EQ(arg.nInsSeq, 1);
	EXPECT_EQ(arg.pcmemDeleted->size(), 1);
	EXPECT_STREQ(arg.pcmemDeleted->at(0).cmemLine.GetStringPtr(), L"9");
	EXPECT_EQ(arg.pcmemDeleted->at(0).nSeq, 1);
	EXPECT_EQ(arg.nDeletedLineNum, 0);
	EXPECT_EQ(arg.nInsLineNum, 0);
	EXPECT_EQ(arg.ptNewPos, CLogicPoint(13, 0));
}

/* =========================================================================
	CSearchAgent::SearchString (#52: SIMD 先頭文字フィルタ)
========================================================================= */

namespace {

//! 全単位位置を順に試す素朴な参照実装。SearchString と同じ最左一致を返す。
const wchar_t* SearchStringNaive(
	const wchar_t* pLine, int nLineLen, int nIdxPos,
	const wchar_t* pszPattern, int nPatternLen, bool bLoHiCase )
{
	if( nLineLen < nPatternLen || nPatternLen <= 0 || nLineLen <= 0 ){
		return nullptr;
	}
	for( int nPos = nIdxPos; nPos <= nLineLen - nPatternLen; ++nPos ){
		int i = 0;
		for( ; i < nPatternLen; ++i ){
			wchar_t c1 = pLine[nPos + i];
			wchar_t c2 = pszPattern[i];
			if( !bLoHiCase ){
				c1 = (wchar_t)skr_towlower( c1 );
				c2 = (wchar_t)skr_towlower( c2 );
			}
			if( c1 != c2 ){
				break;
			}
		}
		if( i == nPatternLen ){
			return &pLine[nPos];
		}
	}
	return nullptr;
}

} // namespace

/*!
	CSearchAgent::SearchString のテスト

	SIMD 先頭文字フィルタ経路 (大小区別・パターン長 128 以下)、BMH 経路
	(129 以上の長パターン)、大小同一視経路のすべてが素朴な参照実装と同じ
	最左一致ポインタを返すことを検査する (#52)。行はサロゲートペアと日本語を含み、
	パターンは行内の任意位置から切り出すため、途中一致・部分一致・候補多数の
	いずれも通る。
*/
TEST(CSearchAgent, SearchStringMatchesNaiveReference)
{
	std::wstring line;
	const std::wstring alphabet = L"aAbBqz 検索対象日本語";
	for( int i = 0; i < 400; ++i ){
		line += alphabet[(i * 13 + i / 7) % alphabet.size()];
	}
	line += L"\xD83D\xDE80";	// サロゲートペアも UTF-16 単位で照合される
	line += L"終端";
	const int nLineLen = (int)line.size();

	const int patternLengths[] = { 1, 2, 3, 4, 5, 6, 7, 8, 15, 16, 17, 32, 63, 64, 65, 80, 127, 128, 129, 160 };
	const int patternOrigins[] = { 0, 1, 37, 123, 250, nLineLen - 90 };
	const int idxPositions[] = { 0, 1, 61 };

	for( bool bLoHiCase : { true, false } ){
		SSearchOption option( false, bLoHiCase, false );
		for( int nPatternLen : patternLengths ){
			for( int nOrigin : patternOrigins ){
				const std::wstring pat = line.substr( nOrigin, nPatternLen );
				CSearchStringPattern pattern;
				ASSERT_TRUE( pattern.SetPattern( nullptr, pat.c_str(), pat.size(), option, nullptr ) );
				for( int nIdxPos : idxPositions ){
					const wchar_t* expected = SearchStringNaive( line.c_str(), nLineLen, nIdxPos, pat.c_str(), (int)pat.size(), bLoHiCase );
					const wchar_t* actual = CSearchAgent::SearchString( line.c_str(), nLineLen, nIdxPos, pattern );
					EXPECT_EQ( expected, actual )
						<< "bLoHiCase=" << bLoHiCase << " len=" << nPatternLen
						<< " origin=" << nOrigin << " idx=" << nIdxPos;
				}
			}
		}
	}
}

/*!
	CSearchAgent::SearchString のテスト

	不一致パターンで nullptr を返すことの検査 (#52)。先頭文字が行の全域に
	出現して最後の 1 文字だけ異なるパターンは、SIMD フィルタに候補検証を
	最大回数行わせる敵対的入力になる。パターンが行より長い場合も検査する。
*/
TEST(CSearchAgent, SearchStringReturnsNullWhenAbsent)
{
	std::wstring line( 300, L'a' );
	line += L"検索";
	const int nLineLen = (int)line.size();

	for( bool bLoHiCase : { true, false } ){
		SSearchOption option( false, bLoHiCase, false );
		for( int nPatternLen : { 1, 2, 5, 6, 63, 64, 65, 128, 129 } ){
			std::wstring pat( nPatternLen, L'a' );
			pat.back() = L'￮';	// 行に存在しない文字
			CSearchStringPattern pattern;
			ASSERT_TRUE( pattern.SetPattern( nullptr, pat.c_str(), pat.size(), option, nullptr ) );
			EXPECT_EQ( nullptr, CSearchAgent::SearchString( line.c_str(), nLineLen, 0, pattern ) )
				<< "bLoHiCase=" << bLoHiCase << " len=" << nPatternLen;
		}

		std::wstring longPat( line.size() + 1, L'a' );
		CSearchStringPattern pattern;
		ASSERT_TRUE( pattern.SetPattern( nullptr, longPat.c_str(), longPat.size(), option, nullptr ) );
		EXPECT_EQ( nullptr, CSearchAgent::SearchString( line.c_str(), nLineLen, 0, pattern ) );
	}
}

TEST(CSearchAgent, Utf16DispatchCallerFindsFirstLastNulSurrogateAndMultipleMatches)
{
	constexpr std::size_t lineLength = 3U * 1024U * 1024U;
	std::wstring line(lineLength, L'x');
	line.front() = L'^';
	line[31] = L'\0';
	line[64] = static_cast<wchar_t>(0xd800);
	line[100] = L'm';
	line[lineLength / 2] = L'm';
	line[lineLength - 2] = L'm';
	line.back() = L'$';
	const int nLineLen = static_cast<int>(line.size());
	SSearchOption option(false, true, false);

	const auto findOffset = [&](std::wstring_view value, int start) {
		CSearchStringPattern pattern;
		EXPECT_TRUE(pattern.SetPattern(nullptr, value.data(), value.size(), option, nullptr));
		const wchar_t* const found = CSearchAgent::SearchString(
			line.data(), nLineLen, start, pattern);
		return found == nullptr ? -1 : static_cast<int>(found - line.data());
	};

	EXPECT_EQ(0, findOffset(std::wstring_view(L"^", 1), 0));
	EXPECT_EQ(nLineLen - 1, findOffset(std::wstring_view(L"$", 1), 0));
	const wchar_t nul = L'\0';
	EXPECT_EQ(31, findOffset(std::wstring_view(&nul, 1), 0));
	const wchar_t loneSurrogate = static_cast<wchar_t>(0xd800);
	EXPECT_EQ(64, findOffset(std::wstring_view(&loneSurrogate, 1), 0));
	EXPECT_EQ(100, findOffset(std::wstring_view(L"m", 1), 0));
	EXPECT_EQ(static_cast<int>(lineLength / 2),
		findOffset(std::wstring_view(L"m", 1), 101));
	EXPECT_EQ(nLineLen - 2,
		findOffset(std::wstring_view(L"m", 1), static_cast<int>(lineLength / 2 + 1)));
	EXPECT_EQ(-1, findOffset(std::wstring_view(L"z", 1), 0));
}

namespace {

//! CSearchAgent.cpp 内で inline 定義されている GetMapIndex はこの TU から
//! リンクできないため、同じ写像を複製する (ベンチマーク専用)。
int BenchMapIndex( wchar_t c )
{
	return ((c & 0xff00) ? 0x100 : 0) | (c & 0xff);
}

//! 変更前 SearchString の大小区別 (bLoHiCase==true) BMH 経路の複製。
//! パターン長 5 以下は wmemcmp 版、6 以上は文字比較ループ版という分岐も
//! 変更前と同一にする。
const wchar_t* SearchStringBmhCaseSensitive(
	const wchar_t* pLine, int nLineLen, int nIdxPos, const CSearchStringPattern& pattern )
{
	const int nPatternLen = pattern.GetLen();
	const wchar_t* pszPattern = pattern.GetCaseKey();
	const int* const useSkipMap = pattern.GetUseCharSkipMap();
	if( nLineLen < nPatternLen || nPatternLen <= 0 || nLineLen <= 0 ){
		return nullptr;
	}
	const int nCompareTo = nLineLen - nPatternLen;
	if( nPatternLen > 5 ){
		for( int nPos = nIdxPos; nPos <= nCompareTo; ){
			int i;
			for( i = 0; i < nPatternLen && pLine[nPos + i] == pszPattern[i]; i++ ){
			}
			if( i >= nPatternLen ){
				return &pLine[nPos];
			}
			nPos += useSkipMap[BenchMapIndex( pLine[nPos + nPatternLen] )];
		}
	}else{
		for( int nPos = nIdxPos; nPos <= nCompareTo; ){
			if( 0 == wmemcmp( &pLine[nPos], pszPattern, nPatternLen ) ){
				return &pLine[nPos];
			}
			nPos += useSkipMap[BenchMapIndex( pLine[nPos + nPatternLen] )];
		}
	}
	return nullptr;
}

//! パターン長上限を外した SIMD 先頭文字フィルタの複製。
//! kFirstCharFilterMaxPatternLength を実測で決めるために、本体の上限判定より
//! 長いパターンでも SIMD 経路を計測できるようにする。
const wchar_t* SearchStringSimdFilterUncapped(
	const wchar_t* pLine, int nLineLen, int nIdxPos, const CSearchStringPattern& pattern )
{
	const int nPatternLen = pattern.GetLen();
	const wchar_t* pszPattern = pattern.GetCaseKey();
	if( nLineLen < nPatternLen || nPatternLen <= 0 || nLineLen <= 0 ){
		return nullptr;
	}
	const int nCompareTo = nLineLen - nPatternLen;
	const auto& cpuDispatch = CpuDispatch::Get();
	const std::size_t nMinimumScan = cpuDispatch.utf16ScanPolicy.findCharMinimumLength;
	const wchar_t wcFirst = pszPattern[0];
	int nPos = nIdxPos;
	while( nPos <= nCompareTo ){
		const std::size_t nSpan = static_cast<std::size_t>(nCompareTo - nPos) + 1;
		std::size_t nFound;
		if( nSpan >= nMinimumScan ){
			nFound = cpuDispatch.findUtf16Char( &pLine[nPos], nSpan, wcFirst );
		}else{
			nFound = 0;
			while( nFound < nSpan && pLine[nPos + nFound] != wcFirst ){
				++nFound;
			}
		}
		if( nFound >= nSpan ){
			return nullptr;
		}
		nPos += static_cast<int>(nFound);
		if( 0 == wmemcmp( &pLine[nPos], pszPattern, nPatternLen ) ){
			return &pLine[nPos];
		}
		++nPos;
	}
	return nullptr;
}

template <typename TBody>
double MeasureSearchMilliseconds( int iterations, TBody&& body )
{
	const auto start = std::chrono::steady_clock::now();
	for( int i = 0; i < iterations; ++i ){
		body();
	}
	const auto stop = std::chrono::steady_clock::now();
	return std::chrono::duration<double, std::milli>( stop - start ).count();
}

} // namespace

/*!
	SearchString の BMH 経路と SIMD 先頭文字フィルタの比較マイクロベンチマーク。

	kFirstCharFilterMaxPatternLength (CSearchAgent.cpp) を実測で決めるための
	計測専用テスト。通常実行では無効。実行方法:
	  tests1.exe --gtest_also_run_disabled_tests
	    --gtest_filter=CSearchAgent.DISABLED_SearchStringMicrobenchmark

	各コーパスは末尾だけに擬似乱数の一意領域を持ち、パターンはそこから切り
	出すため、全アルゴリズムがほぼ全長を走査してから末尾で一致する。
*/
TEST(CSearchAgent, DISABLED_SearchStringMicrobenchmark)
{
	struct Corpus {
		const char* name;
		const std::wstring text;
	};
	std::vector<Corpus> corpora;
	{
		std::mt19937 engine( 20260808u );

		// 本文とパターン (末尾 160 文字) の文字集合を分離する。大小区別検索
		// なので、小文字だけのパターンは大文字だけの本文と決して一致せず、
		// 長さ 1 のパターンでも全長走査後の末尾一致だけが起きる。
		std::wstring ascii;
		while( ascii.size() < 8192 - 160 ){
			ascii += L"THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG. ";
		}
		ascii.resize( 8192 - 160 );
		std::uniform_int_distribution<int> asciiTail( L'a', L'z' );
		for( int i = 0; i < 160; ++i ){
			ascii += (wchar_t)asciiTail( engine );
		}
		corpora.push_back( { "ascii", std::move( ascii ) } );

		// 本文は漢字のみ、末尾はひらがな・カタカナ (U+3041..U+30FE) のみ
		std::wstring japanese;
		while( japanese.size() < 8192 - 160 ){
			japanese += L"検索対象文章走査高速化実装評価計測基準文字列比較処理性能改善";
		}
		japanese.resize( 8192 - 160 );
		std::uniform_int_distribution<int> japaneseTail( 0x3041, 0x30FE );
		for( int i = 0; i < 160; ++i ){
			japanese += (wchar_t)japaneseTail( engine );
		}
		corpora.push_back( { "japanese", std::move( japanese ) } );
	}

	const int patternLengths[] = { 1, 2, 3, 4, 5, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128 };
	const int iterations = 2000;

	for( const auto& corpus : corpora ){
		const wchar_t* pLine = corpus.text.c_str();
		const int nLineLen = (int)corpus.text.size();
		for( int nPatternLen : patternLengths ){
			const std::wstring pat = corpus.text.substr( corpus.text.size() - nPatternLen );
			SSearchOption option( false, true, false );
			CSearchStringPattern pattern;
			ASSERT_TRUE( pattern.SetPattern( nullptr, pat.c_str(), pat.size(), option, nullptr ) );

			// 三実装の一致と、一致が末尾領域 (擬似乱数 160 文字) 内で起きる
			// こと (= 本文全長を走査してから一致すること) を確認してから計測
			// する。短いパターンは末尾領域内で複数回一致し得るため、位置の
			// 完全一致ではなく領域判定にする。
			const wchar_t* rBmh = SearchStringBmhCaseSensitive( pLine, nLineLen, 0, pattern );
			const wchar_t* rSimd = SearchStringSimdFilterUncapped( pLine, nLineLen, 0, pattern );
			const wchar_t* rProd = CSearchAgent::SearchString( pLine, nLineLen, 0, pattern );
			ASSERT_NE( nullptr, rBmh ) << corpus.name << " len=" << nPatternLen;
			ASSERT_GE( rBmh, pLine + nLineLen - 160 ) << corpus.name << " len=" << nPatternLen;
			ASSERT_EQ( rBmh, rSimd );
			ASSERT_EQ( rBmh, rProd );

			size_t sink = 0;
			const double msBmh = MeasureSearchMilliseconds( iterations, [&]{
				sink += SearchStringBmhCaseSensitive( pLine, nLineLen, 0, pattern ) != nullptr;
			} );
			const double msSimd = MeasureSearchMilliseconds( iterations, [&]{
				sink += SearchStringSimdFilterUncapped( pLine, nLineLen, 0, pattern ) != nullptr;
			} );
			const double msProd = MeasureSearchMilliseconds( iterations, [&]{
				sink += CSearchAgent::SearchString( pLine, nLineLen, 0, pattern ) != nullptr;
			} );
			ASSERT_EQ( (size_t)iterations * 3, sink );

			std::printf( "[%-8s] len=%3d  bmh=%9.3fms  simd=%9.3fms  prod=%9.3fms  simd/bmh=%.3f\n",
				corpus.name, nPatternLen, msBmh, msSimd, msProd, msSimd / msBmh );
		}
	}
}

/* =========================================================================
	CDocLineMgr テキスト変更ログ (#358)
	実際の ReplaceData が記録した編集範囲を検査する。
========================================================================= */

namespace {

//! sinceRevision 以降の記録を集める。取りこぼし時は ASSERT で失敗させる。
std::vector<DocTextEdit> CollectTextEditsOrFail(CDocLineMgr& m, std::uint64_t sinceRevision)
{
	std::vector<DocTextEdit> edits;
	EXPECT_TRUE(m.CollectTextEdits(sinceRevision, edits));
	return edits;
}

void DoReplaceData(CDocLineMgr& m, CLogicPoint from, CLogicPoint to, COpeLineData* pInsData)
{
	DocLineReplaceArg arg{};
	arg.sDelRange = CLogicRange(from, to);
	arg.pcmemDeleted = nullptr;
	arg.pInsData = pInsData;
	arg.nDelSeq = 1;
	arg.nInsSeq = -1;
	CSearchAgent(&m).ReplaceData(&arg, false);
}

} // namespace

// 行をまたぐ削除と挿入を1回の ReplaceData で記録する。
// BBB\n を丸ごと削除しつつ DDD\n + EEE を挿入するので、削除行に加えて
// 先頭に文字列が連結される CCC 行も変更行になる (old=2, new=2)。
TEST(CDocLineMgrTextEditLog, ReplaceDataRecordsPreciseRange)
{
	CDocLineMgr m;
	SetLines(m, 1, {L"AAA\n", L"BBB\n", L"CCC\n"});
	const auto seen = m.GetTextRevision();

	COpeLineData insData = MakeOpeLineData({{L"DDD\n", 2}, {L"EEE", 3}});
	DoReplaceData(m, CLogicPoint(0, 1), CLogicPoint(0, 2), &insData);
	EXPECT_EQ(int(m.GetLineCount()), 3);
	EXPECT_STREQ(m.GetLine(CLogicInt(0))->GetPtr(), L"AAA\n");
	EXPECT_STREQ(m.GetLine(CLogicInt(1))->GetPtr(), L"DDD\n");
	EXPECT_STREQ(m.GetLine(CLogicInt(2))->GetPtr(), L"EEECCC\n");

	const auto edits = CollectTextEditsOrFail(m, seen);
	ASSERT_EQ(edits.size(), 1u);
	EXPECT_FALSE(edits[0].full);
	EXPECT_EQ(int(edits[0].firstLine), 1);
	EXPECT_EQ(int(edits[0].oldLineCount), 2);
	EXPECT_EQ(int(edits[0].newLineCount), 2);
	EXPECT_EQ(m.GetTextRevision(), seen + 1);

	textmate::TextMateLineCache cache;
	cache.Reset(3);
	cache.ApplyEdits(edits, static_cast<size_t>(int(m.GetLineCount())));
	EXPECT_EQ(cache.LineCount(), static_cast<size_t>(int(m.GetLineCount())));
}

// 最終行だけを書き換える編集。
TEST(CDocLineMgrTextEditLog, EditOnLastLine)
{
	CDocLineMgr m;
	SetLines(m, 1, {L"AAA\n", L"BBB\n", L"CCC\n"});
	const auto seen = m.GetTextRevision();

	COpeLineData insData = MakeOpeLineData({{L"XYZ", 2}});
	DoReplaceData(m, CLogicPoint(0, 2), CLogicPoint(3, 2), &insData);
	EXPECT_STREQ(m.GetLine(CLogicInt(2))->GetPtr(), L"XYZ\n");

	const auto edits = CollectTextEditsOrFail(m, seen);
	ASSERT_EQ(edits.size(), 1u);
	EXPECT_EQ(int(edits[0].firstLine), 2);
	EXPECT_EQ(int(edits[0].oldLineCount), 1);
	EXPECT_EQ(int(edits[0].newLineCount), 1);

	textmate::TextMateLineCache cache;
	cache.Reset(3);
	cache.ApplyEdits(edits, static_cast<size_t>(int(m.GetLineCount())));
	EXPECT_EQ(cache.LineCount(), static_cast<size_t>(int(m.GetLineCount())));
}

// 最終改行の後ろ (ファントム行位置) への行挿入。
TEST(CDocLineMgrTextEditLog, InsertAfterFinalEol)
{
	CDocLineMgr m;
	SetLines(m, 1, {L"AAA\n", L"BBB\n"});
	const auto seen = m.GetTextRevision();

	COpeLineData insData = MakeOpeLineData({{L"CCC\n", 2}});
	DoReplaceData(m, CLogicPoint(0, 2), CLogicPoint(0, 2), &insData);
	EXPECT_EQ(int(m.GetLineCount()), 3);

	const auto edits = CollectTextEditsOrFail(m, seen);
	ASSERT_EQ(edits.size(), 1u);
	EXPECT_EQ(int(edits[0].firstLine), 2);
	EXPECT_EQ(int(edits[0].oldLineCount), 0);
	EXPECT_EQ(int(edits[0].newLineCount), 1);

	textmate::TextMateLineCache cache;
	cache.Reset(2);
	cache.ApplyEdits(edits, static_cast<size_t>(int(m.GetLineCount())));
	EXPECT_EQ(cache.LineCount(), static_cast<size_t>(int(m.GetLineCount())));
}

// 行末の改行だけを削除して次行と連結する編集は、2行を1行に置き換える記録になる。
TEST(CDocLineMgrTextEditLog, DeleteFinalEolRecordsTwoLines)
{
	CDocLineMgr m;
	SetLines(m, 1, {L"AAA\n", L"BBB\n", L"CCC\n"});
	const auto seen = m.GetTextRevision();

	DoReplaceData(m, CLogicPoint(3, 1), CLogicPoint(4, 1), nullptr);
	EXPECT_EQ(int(m.GetLineCount()), 2);
	EXPECT_STREQ(m.GetLine(CLogicInt(1))->GetPtr(), L"BBBCCC\n");

	const auto edits = CollectTextEditsOrFail(m, seen);
	ASSERT_EQ(edits.size(), 1u);
	EXPECT_EQ(int(edits[0].firstLine), 1);
	EXPECT_EQ(int(edits[0].oldLineCount), 2);
	EXPECT_EQ(int(edits[0].newLineCount), 1);

	textmate::TextMateLineCache cache;
	cache.Reset(3);
	cache.ApplyEdits(edits, static_cast<size_t>(int(m.GetLineCount())));
	EXPECT_EQ(cache.LineCount(), static_cast<size_t>(int(m.GetLineCount())));
}

// 末尾3行の削除。行数は減るが old は編集前の行数で切り詰められる。
TEST(CDocLineMgrTextEditLog, DeleteLastThreeLines)
{
	CDocLineMgr m;
	SetLines(m, 1, {L"0\n", L"1\n", L"2\n", L"3\n", L"4\n"});
	const auto seen = m.GetTextRevision();

	DoReplaceData(m, CLogicPoint(0, 2), CLogicPoint(0, 5), nullptr);
	EXPECT_EQ(int(m.GetLineCount()), 2);

	const auto edits = CollectTextEditsOrFail(m, seen);
	ASSERT_EQ(edits.size(), 1u);
	EXPECT_EQ(int(edits[0].firstLine), 2);
	EXPECT_EQ(int(edits[0].oldLineCount), 3);
	EXPECT_EQ(int(edits[0].newLineCount), 0);

	textmate::TextMateLineCache cache;
	cache.Reset(5);
	cache.ApplyEdits(edits, static_cast<size_t>(int(m.GetLineCount())));
	EXPECT_EQ(cache.LineCount(), static_cast<size_t>(int(m.GetLineCount())));
}

// 空のドキュメントへの挿入。
TEST(CDocLineMgrTextEditLog, InsertIntoEmptyDocument)
{
	CDocLineMgr m;
	const auto seen = m.GetTextRevision();

	COpeLineData insData = MakeOpeLineData({{L"AAA\n", 1}, {L"BBB", 1}});
	DoReplaceData(m, CLogicPoint(0, 0), CLogicPoint(0, 0), &insData);
	EXPECT_EQ(int(m.GetLineCount()), 2);

	const auto edits = CollectTextEditsOrFail(m, seen);
	ASSERT_EQ(edits.size(), 1u);
	EXPECT_EQ(int(edits[0].firstLine), 0);
	EXPECT_EQ(int(edits[0].oldLineCount), 0);
	EXPECT_EQ(int(edits[0].newLineCount), 2);

	textmate::TextMateLineCache cache;
	cache.Reset(0);
	cache.ApplyEdits(edits, static_cast<size_t>(int(m.GetLineCount())));
	EXPECT_EQ(cache.LineCount(), static_cast<size_t>(int(m.GetLineCount())));
}

// 全文書の置き換え。
TEST(CDocLineMgrTextEditLog, ReplaceWholeDocument)
{
	CDocLineMgr m;
	SetLines(m, 1, {L"AAA\n", L"BBB\n", L"CCC\n"});
	const auto seen = m.GetTextRevision();

	COpeLineData insData = MakeOpeLineData({{L"XYZ\n", 2}});
	DoReplaceData(m, CLogicPoint(0, 0), CLogicPoint(0, 3), &insData);

	const auto edits = CollectTextEditsOrFail(m, seen);
	ASSERT_EQ(edits.size(), 1u);
	EXPECT_EQ(int(edits[0].firstLine), 0);
	EXPECT_EQ(int(edits[0].oldLineCount), 3);
	EXPECT_EQ(int(edits[0].newLineCount), int(m.GetLineCount()));

	textmate::TextMateLineCache cache;
	cache.Reset(3);
	cache.ApplyEdits(edits, static_cast<size_t>(int(m.GetLineCount())));
	EXPECT_EQ(cache.LineCount(), static_cast<size_t>(int(m.GetLineCount())));
}

// 挿入データのない削除のみの編集 (ReplaceData の早期 return 経路)。
TEST(CDocLineMgrTextEditLog, DeleteOnlyEditIsRecorded)
{
	CDocLineMgr m;
	SetLines(m, 1, {L"AAA\n", L"BBB\n", L"CCC\n"});
	const auto seen = m.GetTextRevision();

	DoReplaceData(m, CLogicPoint(0, 1), CLogicPoint(3, 1), nullptr);
	EXPECT_STREQ(m.GetLine(CLogicInt(1))->GetPtr(), L"\n");

	const auto edits = CollectTextEditsOrFail(m, seen);
	ASSERT_EQ(edits.size(), 1u);
	EXPECT_EQ(int(edits[0].firstLine), 1);
	EXPECT_EQ(int(edits[0].oldLineCount), 1);
	EXPECT_EQ(int(edits[0].newLineCount), 1);
	EXPECT_EQ(m.GetTextRevision(), seen + 1);
}

// 行内への1行挿入 (bInsOneLine の早期 return 経路)。
TEST(CDocLineMgrTextEditLog, OneLineInsertInsideLineIsRecorded)
{
	CDocLineMgr m;
	SetLines(m, 1, {L"AAA\n", L"BBB\n", L"CCC\n"});
	const auto seen = m.GetTextRevision();

	COpeLineData insData = MakeOpeLineData({{L"DDD", 2}});
	DoReplaceData(m, CLogicPoint(0, 1), CLogicPoint(3, 1), &insData);
	EXPECT_STREQ(m.GetLine(CLogicInt(1))->GetPtr(), L"DDD\n");

	const auto edits = CollectTextEditsOrFail(m, seen);
	ASSERT_EQ(edits.size(), 1u);
	EXPECT_EQ(int(edits[0].firstLine), 1);
	EXPECT_EQ(int(edits[0].oldLineCount), 1);
	EXPECT_EQ(int(edits[0].newLineCount), 1);
	EXPECT_EQ(m.GetTextRevision(), seen + 1);
}

// ログの上限を超える編集を行うと、取りこぼし側は全無効化を余儀なくされる。
TEST(CDocLineMgrTextEditLog, OverflowYieldsFullInvalidation)
{
	CDocLineMgr m;
	SetLines(m, 1, {L"AAA\n", L"BBB\n", L"CCC\n"});
	const auto seen = m.GetTextRevision();

	COpeLineData insData = MakeOpeLineData({{L"x", 2}});
	for (int i = 0; i < 70; ++i) {
		DoReplaceData(m, CLogicPoint(0, 0), CLogicPoint(0, 0), &insData);
	}

	std::vector<DocTextEdit> edits;
	EXPECT_FALSE(m.CollectTextEdits(seen, edits));
}

// 行データ一括操作は範囲を特定できないため full 記録になる。
TEST(CDocLineMgrTextEditLog, DeleteAllLineRecordsFull)
{
	CDocLineMgr m;
	SetLines(m, 1, {L"AAA\n", L"BBB\n", L"CCC\n"});
	const auto seen = m.GetTextRevision();
	m.DeleteAllLine();

	const auto edits = CollectTextEditsOrFail(m, seen);
	ASSERT_GE(edits.size(), 1u);
	EXPECT_TRUE(edits.back().full);
}

namespace {

// push_back が std::bad_alloc を投げるコンテナで、上限付き追記の失敗経路を
// 直接駆動するためのアロケータ。`*m_failures > 0` の間、確保ごとに残り回数を
// 1減らして投げる。
template<typename T>
class ThrowingAllocator final {
public:
	using value_type = T;
	ThrowingAllocator() noexcept = default;
	explicit ThrowingAllocator( const std::shared_ptr<int>& failures ) noexcept
		: m_failures( failures ) {}
	template<typename U>
	ThrowingAllocator( const ThrowingAllocator<U>& other ) noexcept
		: m_failures( other.m_failures ) {}
	T* allocate( std::size_t n ) {
		if( m_failures && *m_failures > 0 ) {
			--*m_failures;
			throw std::bad_alloc();
		}
		return std::allocator<T>{}.allocate( n );
	}
	void deallocate( T* p, std::size_t n ) noexcept { std::allocator<T>{}.deallocate( p, n ); }
	template<typename U> bool operator==( const ThrowingAllocator<U>& other ) const noexcept {
		return m_failures == other.m_failures;
	}
	template<typename U> bool operator!=( const ThrowingAllocator<U>& other ) const noexcept {
		return !(*this == other);
	}
private:
	template<typename> friend class ThrowingAllocator;
	std::shared_ptr<int> m_failures;
};

} // namespace

// #364: PushTextEditRecord の記録失敗経路。デストラクタ経由では例外を
// 透過できないため、失敗時はログを空にして CollectTextEdits が「抜け」を
// 検出するのが契約。継ぎ目 (CDocLineMgr::AppendBoundedEditRecord /
// EditRecordLogHasGap) をアロケータが投げる deque で直接駆動する。
TEST(CDocLineMgrTextEditLog, PushFailureClearsTheLogSoConsumersSeeAGap)
{
	using Log = std::deque<DocTextEdit, ThrowingAllocator<DocTextEdit>>;
	const auto failures = std::make_shared<int>( 0 );
	Log log{ ThrowingAllocator<DocTextEdit>( failures ) };

	// 通常経路: 追記と 64 件上限の切り詰めは失敗しない。
	for (std::uint64_t rev = 1; rev <= 65; ++rev) {
		CDocLineMgr::AppendBoundedEditRecord( log,
			DocTextEdit{ rev, CLogicInt(0), CLogicInt(1), CLogicInt(1), false }, 64 );
	}
	ASSERT_EQ( 64u, log.size() );
	EXPECT_EQ( 2u, log.front().revision );

	// 追記中の確保失敗: 部分記録を残さずログ全体を空にする。noexcept も契約。
	// 実装によっては push_back が新ブロックを即座に確保しないことがあるため、
	// 空になるまで試行する (std::deque は一ブロック分の空きを使い切れば必ず
	// 確保するので、有限回で必ず失敗に到達する)。
	*failures = 128;
	for (std::uint64_t rev = 66; rev < 66 + 128 && !log.empty(); ++rev) {
		static_assert( noexcept(
			CDocLineMgr::AppendBoundedEditRecord( log, DocTextEdit{}, 64 ) ) );
		CDocLineMgr::AppendBoundedEditRecord( log,
			DocTextEdit{ rev, CLogicInt(0), CLogicInt(1), CLogicInt(1), false }, 64 );
	}
	EXPECT_TRUE( log.empty() )
		<< "a bounded append whose allocation fails must leave no partial record";

	// CollectTextEdits と同じ抜け判定: 空ログは差分を再構成できない。
	EXPECT_TRUE( CDocLineMgr::EditRecordLogHasGap( log, 65 ) );

	// 対照: 追記が普通に成功したログは抜けにならない。MSVC の deque は
	// コンストラクタで先頭ブロック群を確保するため、カウンタのリセットは
	// 構築より先に行う。
	*failures = 0;
	Log intact{ ThrowingAllocator<DocTextEdit>( failures ) };
	CDocLineMgr::AppendBoundedEditRecord( intact,
		DocTextEdit{ 1, CLogicInt(0), CLogicInt(0), CLogicInt(1), false }, 64 );
	EXPECT_FALSE( CDocLineMgr::EditRecordLogHasGap( intact, 0 ) );
}
