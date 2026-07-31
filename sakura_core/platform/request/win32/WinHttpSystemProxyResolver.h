/*! @file
 * @brief Current-user IE/PAC/WPAD proxy resolver backed by WinHTTP.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "config/CConfigurationProxyService.h"

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace platform::request::win32 {

//! WinHTTP values deliberately represented without exposing a broad Win32 API
//! surface to deterministic unit tests.
using WinHttpSystemProxyHandle = void*;

enum class EWinHttpSystemProxyCallbackStatus : std::uint8_t {
	GetProxyForUrlComplete,
	RequestError,
	HandleClosing,
};

class IWinHttpSystemProxyCallbackSink {
public:
	virtual ~IWinHttpSystemProxyCallbackSink() = default;
	virtual void OnWinHttpSystemProxyCallback(
		EWinHttpSystemProxyCallbackStatus status,
		unsigned long error
	) noexcept = 0;
};

struct WinHttpCurrentUserProxyConfig final {
	bool autoDetect = false;
	std::wstring autoConfigUrl;
	std::wstring proxy;
	std::wstring proxyBypass;
};

struct WinHttpAutoProxyOptions final {
	bool autoDetect = false;
	std::wstring autoConfigUrl;
	bool autoLogonIfChallenged = false;
};

struct WinHttpSystemProxyEntry final {
	bool isProxy = false;
	bool bypassed = false;
	//! INTERNET_SCHEME value.  The adapter accepts only INTERNET_SCHEME_HTTP.
	unsigned long scheme = 0;
	std::wstring host;
	unsigned short port = 0;
};

struct WinHttpSystemProxyResult final {
	std::vector<WinHttpSystemProxyEntry> entries;
	//! Facade-private native allocation; callers must always return this result
	//! through FreeProxyResult, including failed result retrieval paths.
	void* nativeResult = nullptr;
};

//! Narrow seam for the allocation-owning, cancellable WinHTTP API sequence.
//! The concrete implementation does not consult environment variables.
class IWinHttpSystemProxyFacade {
public:
	virtual ~IWinHttpSystemProxyFacade() = default;

	virtual bool ReadCurrentUserProxyConfig(WinHttpCurrentUserProxyConfig& config) noexcept = 0;
	virtual WinHttpSystemProxyHandle OpenAsyncSession() noexcept = 0;
	virtual bool SetHighAutoLogonPolicy(WinHttpSystemProxyHandle session) noexcept = 0;
	//! Installs the session callback before resolver creation so the child
	//! resolver inherits it. Implementations must request HANDLE_CLOSING in
	//! addition to completion and request-error notifications.
	virtual bool SetStatusCallback(WinHttpSystemProxyHandle session) noexcept = 0;
	virtual bool CreateProxyResolver(WinHttpSystemProxyHandle session, WinHttpSystemProxyHandle& resolver) noexcept = 0;
	//! Associates a stable non-null context with the resolver. The binding must
	//! remain alive through the resolver's final HANDLE_CLOSING callback.
	virtual bool SetHandleContext(
		WinHttpSystemProxyHandle resolver,
		IWinHttpSystemProxyCallbackSink& callbackSink
	) noexcept = 0;
	//! Returns ERROR_IO_PENDING when the asynchronous operation was accepted.
	virtual unsigned long BeginGetProxyForUrl(
		WinHttpSystemProxyHandle resolver,
		const std::wstring& targetUrl,
		const WinHttpAutoProxyOptions& options,
		IWinHttpSystemProxyCallbackSink& callbackSink
	) noexcept = 0;
	virtual bool GetProxyResult(WinHttpSystemProxyHandle resolver, WinHttpSystemProxyResult& result) noexcept = 0;
	virtual void FreeProxyResult(WinHttpSystemProxyResult& result) noexcept = 0;
	virtual bool CloseHandle(WinHttpSystemProxyHandle handle) noexcept = 0;
};

//! Per-call resolver for current-user IE static settings and PAC/WPAD.  Each
//! invocation opens an independent asynchronous WinHTTP session/resolver so a
//! request's cancellation and deadline never affect another request.
class WinHttpSystemProxyResolver final : public config::ISystemProxyResolver {
public:
	WinHttpSystemProxyResolver();
	explicit WinHttpSystemProxyResolver(IWinHttpSystemProxyFacade& facade) noexcept;
	~WinHttpSystemProxyResolver() override;

	WinHttpSystemProxyResolver(const WinHttpSystemProxyResolver&) = delete;
	WinHttpSystemProxyResolver& operator=(const WinHttpSystemProxyResolver&) = delete;

	config::SystemProxyResolution Resolve(
		const ProxyRequest& request,
		std::optional<std::chrono::steady_clock::time_point> deadline,
		const IRequestCancellation* cancellation
	) override;

private:
	class CallbackState;

	std::unique_ptr<IWinHttpSystemProxyFacade> m_ownedFacade;
	IWinHttpSystemProxyFacade* m_facade = nullptr;
};

} // namespace platform::request::win32
