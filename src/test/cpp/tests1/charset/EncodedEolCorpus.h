#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "basis/CEol.h"
#include "charset/CCodePage.h"

#include "RustFixtureTsv.h"

namespace encoded_eol_corpus {

enum class Format { Utf16Le, Utf16Be, Utf32Le, Utf32Be };

namespace detail {

struct FormatInfo {
	const char* const name;
	const EEncodingTrait trait;
	const size_t width;
	const bool bigEndian;
};

inline constexpr std::array kFormats{
	FormatInfo{ "utf16le", ENCODING_TRAIT_UTF16LE, 2, false },
	FormatInfo{ "utf16be", ENCODING_TRAIT_UTF16BE, 2, true },
	FormatInfo{ "utf32le", ENCODING_TRAIT_UTF32LE, 4, false },
	FormatInfo{ "utf32be", ENCODING_TRAIT_UTF32BE, 4, true },
};

constexpr const FormatInfo& Info(Format format)
{
	return kFormats[static_cast<size_t>(format)];
}

struct KindInfo {
	const std::string_view name;
	const EEolType kind;
};

inline constexpr std::array kKinds{
	KindInfo{ "CRLF", EEolType::cr_and_lf },
	KindInfo{ "LF", EEolType::line_feed },
	KindInfo{ "CR", EEolType::carriage_return },
	KindInfo{ "NEL", EEolType::next_line },
	KindInfo{ "LS", EEolType::line_separator },
	KindInfo{ "PS", EEolType::paragraph_separator },
};

} // namespace detail

constexpr size_t UnitWidth(Format format)
{
	return detail::Info(format).width;
}

inline EEncodingTrait TraitOf(Format format)
{
	return detail::Info(format).trait;
}

inline const char* Name(Format format)
{
	return detail::Info(format).name;
}

// Appends the first byteCount bytes (in memory order) of one unit value encoded for the format.
inline void AppendUnit(std::string& bytes, Format format, std::uint32_t value, size_t byteCount)
{
	const auto& info = detail::Info(format);
	for (size_t byte = 0; byte < byteCount; ++byte) {
		const size_t shift = 8 * (info.bigEndian ? info.width - 1 - byte : byte);
		bytes.push_back(static_cast<char>((value >> shift) & 0xff));
	}
}

// Immutable: a record is built once, so the containers of records below are appended to one element at a time.
struct Record {
	const size_t offset;
	const size_t width;
	const EEolType kind;
};

inline std::string ToString(const std::vector<Record>& records)
{
	if (records.empty()) {
		return "-";
	}
	std::string result;
	for (const auto& record : records) {
		if (!result.empty()) {
			result += ';';
		}
		result += std::to_string(record.offset) + ':' + std::to_string(record.width) + ':';
		const auto kind = std::ranges::find(detail::kKinds, record.kind, &detail::KindInfo::kind);
		result += kind == detail::kKinds.end() ? std::string_view("UNKNOWN") : kind->name;
	}
	return result;
}

// Immutable: LoadCases builds every case in one expression and the tests only read it.
struct Case {
	const std::string id;
	const std::string cls;
	const Format format;
	const bool extendedEol;
	const size_t start;
	const std::string bytes;
	const std::vector<Record> records;
	const bool hasTail;

	std::string Describe() const
	{
		return id + '/' + Name(format) + "/extended=" + (extendedEol ? "1" : "0");
	}
};

namespace detail {

inline std::array<bool, kFormats.size()> ParseFormats(std::string_view text)
{
	if (text == "all") {
		return {true, true, true, true};
	}
	if (text == "utf16") {
		return {true, true, false, false};
	}
	if (text == "utf32") {
		return {false, false, true, true};
	}

	std::array<bool, kFormats.size()> selected{};
	for (const auto name : rust_fixture_tsv::Split(text, ',')) {
		const auto format = std::ranges::find_if(kFormats, [name](const auto& info) {
			return name == info.name;
		});
		if (format == kFormats.end()) {
			throw std::runtime_error("unknown format: " + std::string(name));
		}
		selected[static_cast<size_t>(format - kFormats.begin())] = true;
	}
	return selected;
}

inline std::uint32_t ParseUnit(std::string_view token, size_t unitWidth)
{
	if (token == "CR") return 0x0d;
	if (token == "LF") return 0x0a;
	if (token == "NEL") return 0x85;
	if (token == "LS") return 0x2028;
	if (token == "PS") return 0x2029;
	if (token == "NUL") return 0;
	if (token.size() == 1 && token.front() >= 'A' && token.front() <= 'D') {
		return static_cast<std::uint32_t>(token.front());
	}
	if (token.size() == 5 && token.front() == 'u') {
		return rust_fixture_tsv::ParseNumber<std::uint32_t>(token.substr(1), 16, "invalid 16-bit hex unit");
	}
	if (token.size() == 9 && token.front() == 'U') {
		if (unitWidth == 2) {
			throw std::runtime_error("32-bit unit in a UTF-16 format");
		}
		return rust_fixture_tsv::ParseNumber<std::uint32_t>(token.substr(1), 16, "invalid 32-bit hex unit");
	}
	throw std::runtime_error("invalid unit token: " + std::string(token));
}

inline std::string EncodeUnits(std::string_view text, Format format)
{
	if (text == "-") {
		return {};
	}

	const auto& info = Info(format);
	const auto tokens = rust_fixture_tsv::Split(text, ' ');
	std::string bytes;
	for (size_t i = 0; i < tokens.size(); ++i) {
		auto token = tokens[i];
		size_t byteCount = info.width;
		const auto tail = token.find('~');
		if (tail != std::string_view::npos) {
			const auto suffix = token.substr(tail + 1);
			if (i + 1 != tokens.size() || suffix.size() != 1 || suffix[0] < '1' || suffix[0] > '3') {
				throw std::runtime_error("partial unit must be the last token and specify 1 to 3 bytes");
			}
			byteCount = static_cast<size_t>(suffix[0] - '0');
			if (byteCount >= info.width) {
				throw std::runtime_error("partial unit is too wide for the format");
			}
			token = token.substr(0, tail);
		}
		const auto value = ParseUnit(token, info.width);
		AppendUnit(bytes, format, value, byteCount);
	}
	return bytes;
}

inline std::vector<Record> ParseRecords(
	std::string_view text, size_t start, size_t completeUnits, size_t unitWidth)
{
	if (text == "-") {
		return {};
	}

	std::vector<Record> records;
	size_t nextAllowed = start;
	for (const auto record : rust_fixture_tsv::Split(text, ';')) {
		const auto parts = rust_fixture_tsv::Split(record, ':');
		if (parts.size() != 3) {
			throw std::runtime_error("expected offset:width:KIND");
		}
		const size_t offset = rust_fixture_tsv::ParseNumber<size_t>(parts[0], 10, "invalid record offset");
		const size_t width = rust_fixture_tsv::ParseNumber<size_t>(parts[1], 10, "invalid record width");
		const auto kind = std::ranges::find(kKinds, parts[2], &KindInfo::name);
		if (kind == kKinds.end()) {
			throw std::runtime_error("unknown record kind: " + std::string(parts[2]));
		}
		const size_t expectedWidth = kind->kind == EEolType::cr_and_lf ? 2 : 1;
		if (width != expectedWidth) {
			throw std::runtime_error("record width does not match its kind");
		}
		if (offset < nextAllowed || offset > completeUnits || width > completeUnits - offset) {
			throw std::runtime_error("record is out of order, overlapping or outside the scan range");
		}
		records.push_back({offset * unitWidth, width * unitWidth, kind->kind});
		nextAllowed = offset + width;
	}
	return records;
}

} // namespace detail

inline std::vector<Case> LoadCases()
{
	constexpr std::string_view header = "id\tclass\tformats\tstart\tunits\trecords_off\trecords_on";
	std::vector<Case> cases;
	std::set<std::string> ids;
	rust_fixture_tsv::ForEachRow(rust_fixture_tsv::Locate("encoded-eol-corpus.tsv"), header, 7,
		[&](const auto& fields) {
			const auto id = fields[0];
			if (id.empty() || id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string_view::npos) {
				throw std::runtime_error("invalid id");
			}
			if (!ids.emplace(id).second) {
				throw std::runtime_error("duplicate id: " + std::string(id));
			}
			if (fields[1].empty()) {
				throw std::runtime_error("empty class");
			}
			const auto formats = detail::ParseFormats(fields[2]);
			const size_t start = rust_fixture_tsv::ParseNumber<size_t>(fields[3], 10, "invalid start");
			for (size_t i = 0; i < formats.size(); ++i) {
				if (!formats[i]) {
					continue;
				}
				const auto format = static_cast<Format>(i);
				const size_t width = UnitWidth(format);
				const auto bytes = detail::EncodeUnits(fields[4], format);
				const size_t completeUnits = bytes.size() / width;
				if (start > completeUnits) {
					throw std::runtime_error("start is past the last complete unit");
				}
				for (bool extended : {false, true}) {
					const auto expected = extended && fields[6] != "=" ? fields[6] : fields[5];
					cases.push_back({
						std::string(id), std::string(fields[1]), format, extended, start * width, bytes,
						detail::ParseRecords(expected, start, completeUnits, width), bytes.size() % width != 0,
					});
				}
			}
		});
	return cases;
}

} // namespace encoded_eol_corpus
