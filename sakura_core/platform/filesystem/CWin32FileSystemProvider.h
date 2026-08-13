/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <sakura/filesystem/IFileSystemProvider.h>

namespace platform::filesystem {

//! Win32 のローカル file: URI を扱う最初の filesystem provider。
//!
//! URI から native path への変換はこの adapter の内部だけで行う。
class CWin32FileSystemProvider final : public IFileSystemProvider {
public:
	[[nodiscard]] FileSystemCapabilities Capabilities() const noexcept override;
	[[nodiscard]] FileResult<FileStat> Stat(const platform::uri::Uri& resource) override;
	[[nodiscard]] FileResult<std::vector<DirectoryEntry>> Enumerate(const platform::uri::Uri& directory) override;
	[[nodiscard]] FileResult<FileBytes> Read(
		const platform::uri::Uri& resource,
		const FileReadOptions& options) override;
	[[nodiscard]] FileResult<FileContentSnapshot> ReadVersioned(
		const platform::uri::Uri& resource,
		const FileReadOptions& options) override;
	[[nodiscard]] FileConditionalReplaceResult ConditionalAtomicReplace(
		const platform::uri::Uri& resource,
		const FileBytes& bytes,
		const FileConditionalReplaceOptions& options) override;
	[[nodiscard]] FileResult<std::unique_ptr<IFileWatch>> Watch(
		const platform::uri::Uri& resource,
		const FileWatchOptions& options) override;
	[[nodiscard]] FileResult<void> MakeDirectory(const platform::uri::Uri& directory) override;
	[[nodiscard]] FileResult<void> Rename(
		const platform::uri::Uri& source,
		const platform::uri::Uri& target,
		const FileRenameOptions& options) override;
	[[nodiscard]] FileResult<void> Delete(
		const platform::uri::Uri& resource,
		const FileDeleteOptions& options) override;
};

} // namespace platform::filesystem
