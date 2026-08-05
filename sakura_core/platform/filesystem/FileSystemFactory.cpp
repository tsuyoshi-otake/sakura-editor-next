/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "CFileService.h"
#include "CWin32FileSystemProvider.h"

#include <new>
#include <stdexcept>

namespace platform::filesystem {
namespace {

template <class TFactory>
[[nodiscard]] FileResult<std::unique_ptr<IFileService>> MakeService(TFactory&& factory)
{
	try {
		return FileResult<std::unique_ptr<IFileService>>::Success(factory());
	}
	catch (const std::bad_alloc&) {
		return FileResult<std::unique_ptr<IFileService>>::Failure(
			EFileResultStatus::Failed, L"file service allocation failed");
	}
	catch (...) {
		return FileResult<std::unique_ptr<IFileService>>::Failure(
			EFileResultStatus::Failed, L"file service construction failed");
	}
}

} // namespace

FileResult<std::unique_ptr<IFileService>> CreateFileService()
{
	return MakeService([] {
		return std::unique_ptr<IFileService>(std::make_unique<CFileService>());
	});
}

FileResult<std::unique_ptr<IFileService>> CreateWin32FileService()
{
	return MakeService([] {
		auto service = std::make_unique<CFileService>();
		auto registration = service->RegisterProvider(
			L"file", std::make_shared<CWin32FileSystemProvider>());
		if (!registration.Succeeded()) {
			throw std::runtime_error("local filesystem provider registration failed");
		}
		return std::unique_ptr<IFileService>(std::move(service));
	});
}

} // namespace platform::filesystem
