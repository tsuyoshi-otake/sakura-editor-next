/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
 */
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace tests1 {

struct ZipArchiveEntry {
	std::string name;
	std::string content;
};

std::vector<ZipArchiveEntry> ReadZipArchive(const std::filesystem::path& zipPath);

} // namespace tests1
