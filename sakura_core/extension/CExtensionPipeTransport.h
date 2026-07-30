/*! @file
	@brief Node.js 拡張ホストへ直結する安全な Named Pipe transport
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_CEXTENSIONPIPETRANSPORT_0BD3EFB4_20D6_4F80_8D02_C016CB647A86_H_
#define SAKURA_CEXTENSIONPIPETRANSPORT_0BD3EFB4_20D6_4F80_8D02_C016CB647A86_H_
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

struct SExtensionPipeConnectResult {
	bool success = false;
	std::uint32_t errorCode = 0;
	std::uint32_t serverProcessId = 0;
	std::wstring diagnostic;
};

struct SExtensionPipeWriteResult {
	bool success = false;
	std::uint32_t errorCode = 0;
	std::wstring diagnostic;
};

class IExtensionPipeTransportSink {
public:
	virtual ~IExtensionPipeTransportSink() = default;
	// 専用 read thread から呼ばれる。実処理は UI queue 等へ移送し、この callback 内で transport を破棄しないこと。
	virtual void OnExtensionPipeBytes(std::vector<std::uint8_t> bytes) noexcept = 0;
	virtual void OnExtensionPipeClosed(std::uint32_t errorCode, std::wstring diagnostic) noexcept = 0;
};

class CExtensionPipeTransport final {
public:
	explicit CExtensionPipeTransport(IExtensionPipeTransportSink& sink);
	~CExtensionPipeTransport();
	CExtensionPipeTransport(const CExtensionPipeTransport&) = delete;
	CExtensionPipeTransport& operator=(const CExtensionPipeTransport&) = delete;

	SExtensionPipeConnectResult Connect(
		std::wstring pipeName,
		std::uint32_t expectedServerProcessId,
		std::chrono::milliseconds timeout);
	SExtensionPipeWriteResult Send(
		std::span<const std::uint8_t> bytes,
		std::chrono::milliseconds timeout = std::chrono::seconds(5));
	void Close() noexcept;
	bool IsConnected() const noexcept;

private:
	class Impl;
	std::unique_ptr<Impl> m_impl;
};

#endif /* SAKURA_CEXTENSIONPIPETRANSPORT_0BD3EFB4_20D6_4F80_8D02_C016CB647A86_H_ */
