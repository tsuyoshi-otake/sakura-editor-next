#pragma once

#include "io/CFileLoad.h"

// Exposes the protected production scanner (GetNextLineCharCode) to tests.
// startOffset is the first byte scanned and length is the exclusive region end.
// The scan returns the first delimiter at or after startOffset.
// Attach prepares the production partition planner.
// The destructor does not free the attached memory; FileClose only resets the pointer.
class CFileLoadScannerForTest final : public CFileLoad {
public:
	struct Result {
		const size_t lineLength;
		const size_t nextOffset;
		const size_t eolLength;
		const EEolType eolType;
	};

	Result Scan(EEncodingTrait trait, const char* data, size_t length, bool extendedEol = false, size_t startOffset = 0)
	{
		m_encodingTrait = trait;
		m_bEolEx = extendedEol;
		size_t lineLength = 0;
		size_t nextOffset = startOffset;
		size_t eolLength = 0;
		CEol eol;
		GetNextLineCharCode(data, length, &lineLength, &nextOffset, &eol, &eolLength);
		return {lineLength, nextOffset, eolLength, eol.GetType()};
	}

	// Attaches an in-memory file image so the public production planner (GetNextLineOffset) can run on it.
	void Attach(EEncodingTrait trait, const char* data, size_t size, bool extendedEol)
	{
		m_encodingTrait = trait;
		m_bEolEx = extendedEol;
		m_pReadBufTop = data;
		m_nFileSize = static_cast<LONGLONG>(size);
	}
};
