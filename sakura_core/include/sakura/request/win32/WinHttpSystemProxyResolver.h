/*! @file
 * @brief Current-user WinHTTP/PAC system proxy resolver contract and adapter.
 */
/*
    Copyright (C) 2026, Sakura Editor Organization

    SPDX-License-Identifier: Zlib
*/

#pragma once

#include <sakura/request/RequestService.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace platform::request::win32 {

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
	unsigned long scheme = 0;
	std::wstring host;
	unsigned short port = 0;
};

struct WinHttpSystemProxyResult final {
	std::vector<WinHttpSystemProxyEntry> entries;
	void* nativeResult = nullptr;
};

class IWinHttpSystemProxyFacade {
public:
	virtual ~IWinHttpSystemProxyFacade() = default;

	virtual bool ReadCurrentUserProxyConfig(WinHttpCurrentUserProxyConfig& config) noexcept = 0;
	virtual WinHttpSystemProxyHandle OpenAsyncSession() noexcept = 0;
	virtual bool SetHighAutoLogonPolicy(WinHttpSystemProxyHandle session) noexcept = 0;
	virtual bool SetStatusCallback(WinHttpSystemProxyHandle session) noexcept = 0;
	virtual bool CreateProxyResolver(WinHttpSystemProxyHandle session, WinHttpSystemProxyHandle& resolver) noexcept = 0;
	virtual bool SetHandleContext(
		WinHttpSystemProxyHandle resolver,
		IWinHttpSystemProxyCallbackSink& callbackSink
	) noexcept = 0;
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

class WinHttpSystemProxyResolver final : public ISystemProxyResolver {
public:
	WinHttpSystemProxyResolver();
	explicit WinHttpSystemProxyResolver(IWinHttpSystemProxyFacade& facade) noexcept;
	~WinHttpSystemProxyResolver() override;

	WinHttpSystemProxyResolver(const WinHttpSystemProxyResolver&) = delete;
	WinHttpSystemProxyResolver& operator=(const WinHttpSystemProxyResolver&) = delete;

	SystemProxyResolution Resolve(
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
