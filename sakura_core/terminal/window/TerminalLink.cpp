/*! @file */
#include "StdAfx.h"
#include "terminal/window/TerminalLink.h"

#include <algorithm>
#include <string_view>
#include <vector>

namespace terminal {
namespace {

bool IsDelimiter( wchar_t ch ) noexcept
{
	return ch <= L' ' || (ch >= 0x7f && ch <= 0x9f) ||
		std::wstring_view(L"\"'`<>\\|\u00a0\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000\u3001\u3002\u300c\u300d\u2018\u2019\u201c\u201d").find(ch) != std::wstring_view::npos;
}

bool StartsWith( std::wstring_view text, std::wstring_view prefix ) noexcept
{
	if( text.size() < prefix.size() ) return false;
	for( std::size_t i = 0; i < prefix.size(); ++i ) {
		const auto ch = text[i] >= L'A' && text[i] <= L'Z' ? text[i] + (L'a' - L'A') : text[i];
		if( ch != prefix[i] ) return false;
	}
	return true;
}

bool IsWordCharacter( wchar_t ch ) noexcept
{
	return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
		(ch >= L'0' && ch <= L'9') || ch == L'_';
}

} // namespace

std::optional<TerminalWebLink> DetectTerminalWebLink( const TerminalModel& model, TerminalSelectionPoint point )
{
	const auto* clickedRow = GetTerminalRow(model, point.row);
	if( !clickedRow || point.column >= clickedRow->cells.size() ) return std::nullopt;
	while( point.column > 0 && clickedRow->cells[point.column].continuation ) --point.column;
	auto firstRow = point.row;
	std::size_t inspected = 0;
	while( firstRow > 0 ) {
		const auto* previous = GetTerminalRow(model, firstRow - 1);
		if( !previous || !previous->wrapped ) break;
		if( ++inspected > kTerminalLinkScanLimit ) return std::nullopt;
		--firstRow;
	}

	std::wstring text;
	std::vector<TerminalSelectionPoint> positions;
	std::optional<std::size_t> clickedOffset;
	inspected = 0;
	for( auto rowIndex = firstRow; ; ++rowIndex ) {
		const auto* row = GetTerminalRow(model, rowIndex);
		if( !row || row->cells.size() > kTerminalLinkScanLimit - inspected ) return std::nullopt;
		inspected += row->cells.size();
		for( std::size_t column = 0; column < row->cells.size(); ++column ) {
			const auto& cell = row->cells[column];
			if( cell.continuation ) continue;
			const auto cellText = cell.Text().empty() ? std::wstring_view(L" ") : cell.Text();
			if( cellText.size() > kTerminalLinkScanLimit - text.size() ) return std::nullopt;
			if( point == TerminalSelectionPoint{ rowIndex, column } ) clickedOffset = text.size();
			text.append(cellText);
			positions.insert(positions.end(), cellText.size(), { rowIndex, column });
		}
		if( !row->wrapped ) break;
		// Even a malformed chain of empty rows has a fixed work bound.
		if( rowIndex - firstRow >= kTerminalLinkScanLimit ) return std::nullopt;
	}
	if( !clickedOffset ) return std::nullopt;

	for( std::size_t begin = 0; begin < text.size(); ++begin ) {
		const auto remaining = std::wstring_view(text).substr(begin);
		const auto prefixLength = StartsWith(remaining, L"https://") ? 8U : StartsWith(remaining, L"http://") ? 7U : 0U;
		if( prefixLength == 0 || (begin > 0 && IsWordCharacter(text[begin - 1])) ) continue;
		auto end = begin + prefixLength;
		int parentheses = 0, brackets = 0, braces = 0;
		for( ; end < text.size() && !IsDelimiter(text[end]); ++end ) {
			const auto ch = text[end];
			if( ch == L'(' ) ++parentheses;
			else if( ch == L')' && --parentheses < 0 ) break;
			else if( ch == L'[' ) ++brackets;
			else if( ch == L']' && --brackets < 0 ) break;
			else if( ch == L'{' ) ++braces;
			else if( ch == L'}' && --braces < 0 ) break;
		}
		while( end > begin + prefixLength && std::wstring_view(L".,;:!?").find(text[end - 1]) != std::wstring_view::npos ) --end;
		const auto uri = std::wstring_view(text).substr(begin, end - begin);
		const auto authority = uri.substr(prefixLength, uri.find_first_of(L"/?#", prefixLength) - prefixLength);
		const bool supported = !authority.empty() && authority.front() != L':' && authority != L".";
		if( supported && *clickedOffset >= begin && *clickedOffset < end ) {
			auto last = positions[end - 1];
			const auto* lastRow = GetTerminalRow(model, last.row);
			last.column += std::max<std::size_t>(1, lastRow->cells[last.column].width);
			return TerminalWebLink{ std::wstring(uri), positions[begin], last };
		}
		begin = std::max(begin, end - 1);
	}
	return std::nullopt;
}

} // namespace terminal
