/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
 */
#include "pch.h"

#include "ZipArchiveFixture.h"

#include <miniz-cpp/zip_file.hpp>

namespace tests1 {

std::vector<ZipArchiveEntry> ReadZipArchive(const std::filesystem::path& zipPath)
{
	miniz_cpp::zip_file archive(zipPath.string());
	std::vector<ZipArchiveEntry> entries;
	for (const auto& name : archive.namelist()) {
		ZipArchiveEntry entry{ .name = name };
		if (!name.empty() && name.back() != '/') entry.content = archive.read(name);
		entries.push_back(std::move(entry));
	}
	return entries;
}

} // namespace tests1
