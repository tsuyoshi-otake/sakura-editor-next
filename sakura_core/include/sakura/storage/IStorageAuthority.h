/*! @file
	@brief Control-owned lifecycle port for an authoritative storage service.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <sakura/storage/IStorageService.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace platform::storage {

//! The bounded replay window is part of the storage protocol contract.
inline constexpr std::size_t kMaximumStorageCompletedOperations = 4096;

enum class EStorageAuthorityOpenStatus : std::uint8_t {
	Opened,
	AlreadyOpen,
	InvalidArgument,
	WriterBusy,
	IoError,
	CorruptData,
	UnsupportedFormat,
};

struct StorageAuthorityOpenResult {
	EStorageAuthorityOpenStatus status = EStorageAuthorityOpenStatus::IoError;
	std::string diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EStorageAuthorityOpenStatus::Opened
			|| status == EStorageAuthorityOpenStatus::AlreadyOpen;
	}
};

/*! @brief Control-owned lifecycle port for a stateful authoritative store.

	The authority owns its worker/queue/OS handles and must reach a terminal
	closed state before the composition root releases its last reference. The
	port deliberately exposes no file format, writer lock, or platform I/O seam;
	those belong to the selected implementation at the Control composition root.
*/
class IStorageAuthority : public IStorageService {
public:
	~IStorageAuthority() override = default;

	[[nodiscard]] virtual StorageAuthorityOpenResult Open() = 0;
	virtual void Close() noexcept = 0;
	[[nodiscard]] virtual bool IsOpen() const noexcept = 0;
};

} // namespace platform::storage
