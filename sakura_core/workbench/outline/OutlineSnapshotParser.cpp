/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"

#include "workbench/outline/OutlineParserWorker.h"

#include "doc/CDocOutline.h"
#include "outline/CFuncInfo.h"
#include "outline/CFuncInfoArr.h"
#include "types/CType.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace workbench::outline {

namespace {

using SteadyClock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t NowUs() noexcept
{
	return static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now().time_since_epoch()).count());
}

[[nodiscard]] bool IsCancelled( const OutlineParserWorker::CancelToken& token ) noexcept
{
	return token != nullptr && token->load(std::memory_order_acquire);
}

[[nodiscard]] std::wstring CopyNativeString( const CNativeW& value )
{
	const wchar_t* const text = value.GetStringPtr();
	return text != nullptr ? std::wstring(text) : std::wstring();
}

} // namespace

OutlineParserWorker::OutlineParserWorker()
	: OutlineParserWorker(ParseSnapshot)
{
}

OutlineParseResult OutlineParserWorker::ParseSnapshot(
	const OutlineDocumentSnapshot& snapshot,
	int outlineType,
	int listType,
	const CancelToken& cancelToken )
{
	OutlineParseResult result;
	result.documentVersion = snapshot.documentVersion;
	result.outlineType = outlineType;
	result.listType = listType;
	result.filePath = snapshot.filePath;
	result.appendText = snapshot.appendText;

	if( !snapshot.IsValid() ) {
		throw std::invalid_argument("Malformed Outline document snapshot");
	}
	if( IsCancelled(cancelToken) ) return result;

	CFuncInfoArr parsed;
	CDocOutline outline(
		snapshot,
		[cancelToken] { return IsCancelled(cancelToken); });
	const auto parserBegin = NowUs();

	switch( static_cast<EOutlineType>(outlineType) ) {
	case OUTLINE_C:
	case OUTLINE_C_CPP:
	case OUTLINE_CPP: {
		EOutlineType effectiveType = static_cast<EOutlineType>(outlineType);
		outline.MakeFuncList_C(&parsed, effectiveType, snapshot.filePath.c_str());
		result.outlineType = static_cast<int>(effectiveType);
		break;
	}
	case OUTLINE_JAVA:
		outline.MakeFuncList_Java(&parsed);
		break;
	default:
		// Every other legacy parser still reads live document/global state and is
		// intentionally kept on the synchronous UI boundary.
		throw std::invalid_argument("Outline snapshot parser is not available for this outline type");
	}
	result.timings.parserUs = NowUs() - parserBegin;

	const auto dtoBegin = NowUs();
	result.symbols.reserve(static_cast<std::size_t>(std::max(0, parsed.GetNum())));
	for( int index = 0; index < parsed.GetNum(); ++index ) {
		if( IsCancelled(cancelToken) ) break;
		const CFuncInfo* const info = parsed.GetAt(static_cast<std::size_t>(index));
		if( info == nullptr ) continue;
		OutlineSymbolDto dto;
		dto.logicalLine = static_cast<int>(info->m_nFuncLineCRLF);
		dto.logicalColumn = static_cast<int>(info->m_nFuncColCRLF);
		dto.name = CopyNativeString(info->m_cmemFuncName);
		dto.fileName = CopyNativeString(info->m_cmemFileName);
		dto.info = info->m_nInfo;
		dto.depth = info->m_nDepth;
		result.symbols.emplace_back(std::move(dto));
	}
	for( int info = 0; info < FL_OBJ_ELEMENT_MAX; ++info ) {
		std::wstring append = parsed.GetAppendText(info);
		if( !append.empty() ) result.appendText[info] = std::move(append);
	}
	result.timings.dtoConstructionUs = NowUs() - dtoBegin;
	result.timings.totalUs = NowUs() - parserBegin;
	return result;
}

} // namespace workbench::outline
