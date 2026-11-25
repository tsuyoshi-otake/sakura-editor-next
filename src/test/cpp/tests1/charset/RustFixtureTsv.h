/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <charconv>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// Reading a TSV corpus under rust/native/fixtures: where the file is, how a line is split
// and numbered, and what a malformed line reports. What a row means belongs to the corpus
// that owns the file.
namespace rust_fixture_tsv {

// Walks up from the current directory (the repository root, or a build directory below it)
// until rust/native/fixtures/<fileName> exists.
inline std::filesystem::path Locate(std::string_view fileName)
{
	const auto relative = std::filesystem::path("rust/native/fixtures") / std::filesystem::path(fileName);
	const auto start = std::filesystem::current_path();
	auto directory = start;
	for (size_t level = 0; level < 8; ++level) {
		const auto candidate = directory / relative;
		std::error_code error;
		if (std::filesystem::is_regular_file(candidate, error)) {
			return candidate;
		}
		const auto parent = directory.parent_path();
		if (parent == directory) {
			break;
		}
		directory = parent;
	}
	throw std::runtime_error("Cannot find " + relative.generic_string() + " from " + start.string());
}

inline std::vector<std::string_view> Split(std::string_view text, char delimiter)
{
	std::vector<std::string_view> parts;
	for (;;) {
		const auto pos = text.find(delimiter);
		parts.push_back(text.substr(0, pos));
		if (pos == std::string_view::npos) {
			return parts;
		}
		text.remove_prefix(pos + 1);
	}
}

template <class UInt>
UInt ParseNumber(std::string_view text, int base, const char* reason)
{
	if (text.empty()) {
		throw std::runtime_error(reason);
	}
	UInt value{};
	const auto end = text.data() + text.size();
	const auto parsed = std::from_chars(text.data(), end, value, base);
	if (parsed.ec != std::errc{} || parsed.ptr != end) {
		throw std::runtime_error(reason);
	}
	return value;
}

inline std::runtime_error LineError(
	const std::filesystem::path& path, size_t line, const char* reason)
{
	return std::runtime_error(path.string() + ':' + std::to_string(line) + ": " + reason);
}

// Visits every data row after checking the header line. Blank lines, lines that start with
// '#' and a trailing CR are ignored. A row is passed as its tab-separated fields; a
// std::runtime_error it throws is rethrown with the path and the line number.
template <class OnRow>
void ForEachRow(const std::filesystem::path& path, std::string_view header, size_t fieldCount, OnRow&& onRow)
{
	std::ifstream input(path, std::ios::binary);
	if (!input) {
		throw LineError(path, 1, "cannot open corpus");
	}

	size_t lineNumber = 0;
	bool sawHeader = false;
	for (std::string line; std::getline(input, line);) {
		++lineNumber;
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line.empty() || line.front() == '#') {
			continue;
		}
		try {
			if (!sawHeader) {
				if (line != header) {
					throw std::runtime_error("invalid header");
				}
				sawHeader = true;
				continue;
			}

			const auto fields = Split(line, '\t');
			if (fields.size() != fieldCount) {
				throw std::runtime_error("expected " + std::to_string(fieldCount) + " tab-separated fields");
			}
			onRow(fields);
		} catch (const std::runtime_error& error) {
			throw LineError(path, lineNumber, error.what());
		}
	}
	if (input.bad() || !input.eof()) {
		throw LineError(path, lineNumber + 1, "cannot read corpus");
	}
	if (!sawHeader) {
		throw LineError(path, lineNumber + 1, "missing header");
	}
}

} // namespace rust_fixture_tsv
