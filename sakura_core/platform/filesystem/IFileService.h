/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include "platform/filesystem/IFileSystemProvider.h"

namespace platform::filesystem {

//! URI を scheme provider に dispatch するアプリケーション向けファイルサービス。
class IFileService {
public:
	virtual ~IFileService() = default;

	[[nodiscard]] virtual FileResult<FileStat> Stat(const platform::uri::Uri& resource) = 0;
	[[nodiscard]] virtual FileResult<std::vector<DirectoryEntry>> Enumerate(const platform::uri::Uri& directory) = 0;
	[[nodiscard]] virtual FileResult<FileBytes> Read(
		const platform::uri::Uri& resource,
		const FileReadOptions& options) = 0;
	//! 既存の service fake を壊さず段階導入するため、既定は Unsupported。
	[[nodiscard]] virtual FileResult<FileContentSnapshot> ReadVersioned(
		const platform::uri::Uri&,
		const FileReadOptions&)
	{
		return FileResult<FileContentSnapshot>::Failure(
			EFileResultStatus::Unsupported, L"file service does not support versioned reads");
	}
	[[nodiscard]] virtual FileConditionalReplaceResult ConditionalAtomicReplace(
		const platform::uri::Uri&,
		const FileBytes&,
		const FileConditionalReplaceOptions&)
	{
		return FileConditionalReplaceResult::Unsupported(
			L"file service does not support conditional atomic replace");
	}
	[[nodiscard]] virtual FileResult<std::unique_ptr<IFileWatch>> Watch(
		const platform::uri::Uri& resource,
		const FileWatchOptions& options = {}) = 0;
};

} // namespace platform::filesystem
