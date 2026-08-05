/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <sakura/filesystem/IFileSystemProvider.h>

namespace platform::filesystem {

//! URI を scheme provider に dispatch するアプリケーション向けファイルサービス。
class IFileService {
public:
	virtual ~IFileService() = default;

	//! Construction-time provider seam. Runtime consumers should not use this
	//! after composition; test fakes may use it without reaching a concrete
	//! registry implementation.
	[[nodiscard]] virtual FileResult<void> RegisterProvider(
		std::wstring_view,
		std::shared_ptr<IFileSystemProvider>)
	{
		return FileResult<void>::Failure(
			EFileResultStatus::Unsupported, L"file service provider registration is unavailable");
	}

	//! Parse-result overloads keep URI parsing and filesystem dispatch in one
	//! typed boundary; malformed input never reaches a provider.
	[[nodiscard]] FileResult<FileStat> Stat(const platform::uri::UriParseResult& resource)
	{
		return resource
			? Stat(*resource.value)
			: FileResult<FileStat>::Failure(EFileResultStatus::InvalidUri, L"filesystem URI is invalid");
	}

	[[nodiscard]] FileResult<std::vector<DirectoryEntry>> Enumerate(
		const platform::uri::UriParseResult& directory)
	{
		return directory
			? Enumerate(*directory.value)
			: FileResult<std::vector<DirectoryEntry>>::Failure(
				EFileResultStatus::InvalidUri, L"filesystem URI is invalid");
	}

	[[nodiscard]] FileResult<FileBytes> Read(
		const platform::uri::UriParseResult& resource,
		const FileReadOptions& options)
	{
		return resource
			? Read(*resource.value, options)
			: FileResult<FileBytes>::Failure(EFileResultStatus::InvalidUri, L"filesystem URI is invalid");
	}

	[[nodiscard]] FileResult<FileContentSnapshot> ReadVersioned(
		const platform::uri::UriParseResult& resource,
		const FileReadOptions& options)
	{
		return resource
			? ReadVersioned(*resource.value, options)
			: FileResult<FileContentSnapshot>::Failure(
				EFileResultStatus::InvalidUri, L"filesystem URI is invalid");
	}

	[[nodiscard]] FileConditionalReplaceResult ConditionalAtomicReplace(
		const platform::uri::UriParseResult& resource,
		const FileBytes& bytes,
		const FileConditionalReplaceOptions& options)
	{
		return resource
			? ConditionalAtomicReplace(*resource.value, bytes, options)
			: FileConditionalReplaceResult::Failure(L"filesystem URI is invalid");
	}

	[[nodiscard]] FileResult<std::unique_ptr<IFileWatch>> Watch(
		const platform::uri::UriParseResult& resource,
		const FileWatchOptions& options = {})
	{
		return resource
			? Watch(*resource.value, options)
			: FileResult<std::unique_ptr<IFileWatch>>::Failure(
				EFileResultStatus::InvalidUri, L"filesystem URI is invalid");
	}

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
