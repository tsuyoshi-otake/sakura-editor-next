/*! @file
\t@brief Composition factory for the default Control-owned storage authority.
*/
/*
\tCopyright (C) 2026, Sakura Editor Organization

\tSPDX-License-Identifier: Zlib
*/
#pragma once

#include <sakura/storage/IStorageAuthority.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace platform::storage {

//! Creates the default durable authority used by the Control composition root.
//!
//! The returned object is closed until the composition root calls Open().  The
//! factory exposes only the lifecycle port; file format, locking, and Win32 I/O
//! seams remain private to the storage implementation.
[[nodiscard]] std::shared_ptr<IStorageAuthority> CreateAtomicFileStorageAuthority(
	const std::filesystem::path& directory,
	std::uint64_t generation,
	std::size_t maxCompletedOperations);

} // namespace platform::storage
