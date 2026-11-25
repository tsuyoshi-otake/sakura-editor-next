/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include "charset/CUtf8.h"

// CUtf8 keeps its allocation-free whole-buffer loops protected. Tests that must run
// them without a CMemory or CNativeW per call reach them through this subclass,
// which adds no behavior of its own.
class CUtf8Access final : public CUtf8 {
public:
	static int DecodeForTest(const char* source, int sourceLength, wchar_t* destination, bool cesu8Mode)
	{
		return Utf8ToUni(source, sourceLength, destination, cesu8Mode);
	}

	static int DecodeCharacterForTest(const unsigned char* source, int sourceLength,
		unsigned short* destination, bool cesu8Mode)
	{
		return _Utf8ToUni_char(source, sourceLength, destination, cesu8Mode);
	}

	static int EncodeForTest(const wchar_t* source, int sourceLength, char* destination, bool* lossy,
		bool cesu8Mode)
	{
		return UniToUtf8(source, sourceLength, destination, lossy, cesu8Mode);
	}
};
