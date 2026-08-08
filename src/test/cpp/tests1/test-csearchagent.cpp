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
	int seq;
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
		std::wstring text;
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
