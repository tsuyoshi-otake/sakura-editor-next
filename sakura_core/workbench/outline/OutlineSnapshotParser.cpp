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
#include <map>
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

[[nodiscard]] std::vector<std::wstring> SplitQualifiedName( std::wstring_view name )
{
	std::vector<std::wstring> parts;
	std::size_t start = 0;
	int templateDepth = 0;
	for( std::size_t index = 0; index < name.size(); ++index ) {
		switch( name[index] ) {
		case L'<':
			++templateDepth;
			break;
		case L'>':
			if( templateDepth > 0 ) --templateDepth;
			break;
		case L':':
			if( templateDepth == 0 && index + 1 < name.size() && name[index + 1] == L':' ) {
				parts.emplace_back(name.substr(start, index - start));
				start = index + 2;
				++index;
			}
			break;
		default:
			break;
		}
	}
	parts.emplace_back(name.substr(start));
	return parts;
}

[[nodiscard]] std::wstring AppendOutlineLabel(
	const OutlineParseResult& result,
	std::wstring label,
	int info )
{
	if( info != FL_OBJ_DEFINITION && info != FL_OBJ_NAMESPACE && info != FL_OBJ_GLOBAL ) {
		if( const auto found = result.appendText.find(info); found != result.appendText.end() ) {
			label += found->second;
		}
	}
	return label;
}

void BuildTreePlan( OutlineParseResult& result, const OutlineParserWorker::CancelToken& cancelToken )
{
	result.TreeNodes().clear();
	result.TreeNodes().reserve(result.symbols.size() + result.symbols.size() / 2);
	std::map<std::wstring, int> classNodes;
	int globalNode = -1;

	for( std::size_t symbolIndex = 0; symbolIndex < result.symbols.size(); ++symbolIndex ) {
		if( IsCancelled(cancelToken) ) {
			result.TreeNodes().clear();
			return;
		}
		const auto& symbol = result.symbols[symbolIndex];
		const auto parts = SplitQualifiedName(symbol.name);
		int parentNode = -1;
		if( parts.size() > 1 ) {
			std::wstring path;
			for( std::size_t partIndex = 0; partIndex + 1 < parts.size(); ++partIndex ) {
				path.append(parts[partIndex]);
				path.push_back(L'\0');
				const auto found = classNodes.find(path);
				if( found != classNodes.end() ) {
					parentNode = found->second;
					continue;
				}
				const int classInfo = symbol.info == FL_OBJ_NAMESPACE ? FL_OBJ_NAMESPACE : FL_OBJ_CLASS;
				const std::wstring label = AppendOutlineLabel(
					result,
					parts[partIndex],
					classInfo);
				const int nodeIndex = static_cast<int>(result.TreeNodes().size());
				result.TreeNodes().emplace_back(
					-1,
					parentNode,
					classInfo,
					static_cast<int>(partIndex),
					true,
					label);
				classNodes.emplace(path, nodeIndex);
				parentNode = nodeIndex;
			}
		}else if( symbol.info < FL_OBJ_CLASS || symbol.info > FL_OBJ_ELEMENT_MAX ) {
			if( globalNode < 0 ) {
				globalNode = static_cast<int>(result.TreeNodes().size());
				std::wstring label;
				if( const auto found = result.appendText.find(FL_OBJ_GLOBAL); found != result.appendText.end() ) {
					label = found->second;
				}
				result.TreeNodes().emplace_back(-1, -1, FL_OBJ_GLOBAL, 0, true, std::move(label));
			}
			parentNode = globalNode;
		}

		const std::wstring label = AppendOutlineLabel(
			result,
			parts.empty() ? symbol.name : parts.back(),
			symbol.info);
		const int symbolNodeIndex = static_cast<int>(result.TreeNodes().size());
		result.TreeNodes().emplace_back(
			static_cast<int>(symbolIndex),
			parentNode,
			symbol.info,
			static_cast<int>(parts.size() > 0 ? parts.size() - 1 : 0),
			false,
			label);
		if( parts.size() == 1 && symbol.info >= FL_OBJ_CLASS && symbol.info <= FL_OBJ_ELEMENT_MAX ) {
			classNodes.emplace(parts[0] + std::wstring(1, L'\0'), symbolNodeIndex);
		}
	}
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
	BuildTreePlan(result, cancelToken);
	result.timings.dtoConstructionUs = NowUs() - dtoBegin;
	result.timings.totalUs = NowUs() - parserBegin;
	return result;
}

} // namespace workbench::outline
