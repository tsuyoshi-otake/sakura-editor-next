/*! @file */
/*
	Copyright (C) 2018-2022, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
// 2008.11.10 変換ロジックを書き直す

#include "StdAfx.h"
#include "CUtf8.h"
#include "charset/codechecker.h"
#include "basis/CEol.h"
#include "env/CommonSetting.h"
#include "util/CpuDispatch.h"

/*!
	UTF-8 → Unicode 実装

	@param[in] bCESU8Mode CESU-8 を処理する場合 true
*/
int CUtf8::Utf8ToUni( const char* pSrc, const int nSrcLen, wchar_t* pDst, bool bCESU8Mode )
{
	const unsigned char *pr, *pr_end;
	unsigned short *pw;
	int nclen;
	ECharSet echarset;

	if( nSrcLen < 1 ){
		return 0;
	}

	pr = reinterpret_cast<const unsigned char*>(pSrc);
	pr_end = reinterpret_cast<const unsigned char*>(pSrc+nSrcLen);
	pw = reinterpret_cast<unsigned short*>(pDst);

	// ASCII バイト (0x80 未満) は UTF-8/CESU-8 いずれでも 1 バイト = 1 UTF-16
	// 単位へそのまま拡幅されるため、連続区間は文字単位チェックを介さず一括変換
	// できる。短い区間の間接呼び出しを避けるため、ポリシー最小長までは
	// 呼び出し側スカラーで消化し、まだ続く場合のみ SIMD カーネルへ委譲する。
	const auto& cpuDispatch = CpuDispatch::Get();
	const std::size_t nWidenMinimum = cpuDispatch.utf8ConversionPolicy.widenAsciiMinimumLength;

	for( ; ; ){

		if( pr < pr_end && *pr < 0x80 ){
			const std::size_t nRemaining = pr_end - pr;
			const std::size_t nScalarSpan = nRemaining < nWidenMinimum ? nRemaining : nWidenMinimum;
			std::size_t nAscii = 0;
			while( nAscii < nScalarSpan && pr[nAscii] < 0x80 ){
				pw[nAscii] = pr[nAscii];
				++nAscii;
			}
			if( nAscii == nScalarSpan && nRemaining > nScalarSpan ){
				nAscii += cpuDispatch.widenAsciiToUtf16(
					reinterpret_cast<const char*>(pr + nAscii),
					nRemaining - nAscii,
					reinterpret_cast<wchar_t*>(pw + nAscii));
			}
			pr += nAscii;
			pw += nAscii;
			if( pr >= pr_end ){
				break;
			}
		}

		// 文字をチェック
		if( bCESU8Mode != true ){
			nclen = CheckUtf8Char( reinterpret_cast<const char*>(pr), pr_end-pr, &echarset, true, 0 );
		}else{
			nclen = CheckCesu8Char( reinterpret_cast<const char*>(pr), pr_end-pr, &echarset, 0 );
		}
		if( nclen < 1 ){
			break;
		}

		// 変換
		if( echarset != CHARSET_BINARY ){
			pw += _Utf8ToUni_char( pr, nclen, pw, bCESU8Mode );
			pr += nclen;
		}else{
			if( nclen != 1 ){	// 保護コード
				nclen = 1;
			}
			pw += BinToText( pr, 1, pw );
			++pr;
		}
	}

	return int(pw - reinterpret_cast<unsigned short*>(pDst));
}

//! UTF-8→Unicodeコード変換
// 2007.08.13 kobake 作成
EConvertResult CUtf8::_UTF8ToUnicode( const CMemory& cSrc, CNativeW* pDstMem, bool bCESU8Mode/*, bool decodeMime*/ )
{
	// エラー状態
	bool bError = false;

	// データ取得
	int nSrcLen = cSrc.GetRawLength();
	const char* pSrc = reinterpret_cast<const char*>( cSrc.GetRawPtr() );

	if( &cSrc == pDstMem->_GetMemory() )
	{
		// 必要なバッファサイズを調べて確保する
		wchar_t* pDst = new (std::nothrow) wchar_t[nSrcLen];
		if( pDst == nullptr ){
			return RESULT_FAILURE;
		}

		// 変換
		int nDstLen = Utf8ToUni( pSrc, nSrcLen, pDst, bCESU8Mode );

		// pDstMem を更新
		pDstMem->_GetMemory()->SetRawDataHoldBuffer( pDst, nDstLen*sizeof(wchar_t) );

		// 後始末
		delete [] pDst;
	}
	else
	{
		// 変換先バッファサイズを設定してメモリ領域確保
		pDstMem->AllocStringBuffer( nSrcLen + 1 );
		wchar_t* pDst = pDstMem->GetStringPtr();

		// 変換
		size_t nDstLen = Utf8ToUni(pSrc, nSrcLen, pDst, bCESU8Mode);

		// pDstMem を更新
		pDstMem->_SetStringLength( nDstLen );

		// 変換元バイト数を文字数とみなして確保しているため、マルチバイト主体の行では余剰が大きい
		pDstMem->_GetMemory()->ShrinkBuffer();
	}

	if( bError == false ){
		return RESULT_COMPLETE;
	}else{
		return RESULT_LOSESOME;
	}
}

/*!
	Unicode -> UTF-8 実装

	@param[in] bCESU8Mode CESU-8 を処理する場合 true
*/
int CUtf8::UniToUtf8( const wchar_t* pSrc, const int nSrcLen, char* pDst, bool* pbError, bool bCESU8Mode )
{
	const unsigned short* pr = reinterpret_cast<const unsigned short*>(pSrc);
	const unsigned short* pr_end = reinterpret_cast<const unsigned short*>(pSrc+nSrcLen);
	unsigned char* pw = reinterpret_cast<unsigned char*>(pDst);
	int nclen;
	bool berror=false;
	ECharSet echarset;

	while( (nclen = CheckUtf16leChar(reinterpret_cast<const wchar_t*>(pr), pr_end-pr, &echarset, 0)) > 0 ){
		// 保護コード
		switch( echarset ){
		case CHARSET_UNI_NORMAL:
			nclen = 1;
			break;
		case CHARSET_UNI_SURROG:
			nclen = 2;
			break;
		default:
			echarset = CHARSET_BINARY;
			nclen = 1;
		}
		if( echarset != CHARSET_BINARY ){
			pw += _UniToUtf8_char( pr, nclen, pw, bCESU8Mode );
			pr += nclen;
		}else{
			if(  nclen == 1 && IsBinaryOnSurrogate(static_cast<wchar_t>(*pr)) ){
				*pw = static_cast<unsigned char>( TextToBin(*pr) & 0x000000ff );
				++pw;
			}else{
				berror = true;
				*pw = '?';
				++pw;
			}
			++pr;
		}
	}

	if( pbError ){
		*pbError = berror;
	}

	return int(pw - reinterpret_cast<unsigned char*>(pDst));
}

//! コード変換 Unicode→UTF-8
EConvertResult CUtf8::_UnicodeToUTF8( const CNativeW& cSrc, CMemory* pDstMem, bool bCesu8Mode )
{
	// エラー状態
	bool bError = false;

	// ソースを取得
	const wchar_t* pSrc = cSrc.GetStringPtr();
	int nSrcLen = cSrc.GetStringLength();

	// 必要なバッファサイズを調べてメモリを確保
	char* pDst = new (std::nothrow) char[nSrcLen * 3];
	if( pDst == nullptr ){
		return RESULT_FAILURE;
	}

	// 変換
	int nDstLen = UniToUtf8( pSrc, nSrcLen, pDst, &bError, bCesu8Mode );

	// pDstMem を更新
	pDstMem->SetRawDataHoldBuffer( pDst, nDstLen );

	// 後始末
	delete [] pDst;

	if( bError == false ){
		return RESULT_COMPLETE;
	}else{
		return RESULT_LOSESOME;
	}
}

// 文字コード表示用	UNICODE → Hex 変換	2008/6/21 Uchi
EConvertResult CUtf8::_UnicodeToHex(const wchar_t* cSrc, const int iSLen, WCHAR* pDst, const CommonSetting_Statusbar* psStatusbar, const bool bCESUMode)
{
	CNativeW		cBuff;
	EConvertResult	res;
	int				i;
	WCHAR*			pd;
	unsigned char*	ps;
	bool			bbinary=false;

	if (psStatusbar->m_bDispUtf8Codepoint) {
		// Unicodeで表示
		return CCodeBase::UnicodeToHex(cSrc, iSLen, pDst, psStatusbar);
	}
	cBuff.AllocStringBuffer(4);
	// 1文字データバッファ
	if (IsUTF16High(cSrc[0]) && iSLen >= 2 && IsUTF16Low(cSrc[1])) {
		cBuff._GetMemory()->SetRawDataHoldBuffer(cSrc, 4);
	}
	else if (iSLen >= 3 && IsVariationSelector(cSrc + 1)) {
		cBuff._GetMemory()->SetRawDataHoldBuffer(cSrc, sizeof(wchar_t) * 3);
	}
	else {
		cBuff._GetMemory()->SetRawDataHoldBuffer(cSrc, 2);
		if( IsBinaryOnSurrogate(cSrc[0]) ){
			bbinary = true;
		}
	}

	// UTF-8/CESU-8 変換
	if (bCESUMode != true) {
		res = UnicodeToUTF8(cBuff, cBuff._GetMemory());
	}
	else {
		res = UnicodeToCESU8(cBuff, cBuff._GetMemory());
	}
	if (res != RESULT_COMPLETE) {
		return res;
	}

	// Hex変換
	ps = reinterpret_cast<unsigned char*>( cBuff._GetMemory()->GetRawPtr() );
	pd = pDst;
	if( bbinary == false ){
		for (i = cBuff._GetMemory()->GetRawLength(); i >0; i--, ps ++, pd += 2) {
			auto_sprintf( pd, L"%02X", *ps);
		}
	}else{
		auto_sprintf( pd, L"?%02X", *ps );
	}

	return RESULT_COMPLETE;
}
