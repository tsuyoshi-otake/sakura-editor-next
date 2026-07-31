/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include "platform/request/win32/WinHttpSystemProxyResolver.h"

#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace config;
using namespace platform::request::win32;
using platform::request::EProxyMode;
using platform::request::EProxySupport;
using platform::request::IRequestCancellation;
using platform::request::ProxyRequest;

namespace {

class Cancelled final : public IRequestCancellation {
public:
	bool IsCancellationRequested() const noexcept override { return cancelled.load(); }
	std::atomic<bool> cancelled{ false };
};

class FakeFacade final : public IWinHttpSystemProxyFacade {
public:
	bool ReadCurrentUserProxyConfig(WinHttpCurrentUserProxyConfig& value) noexcept override { value = config; return readConfig; }
	WinHttpSystemProxyHandle OpenAsyncSession() noexcept override { ++openCalls; return openSession ? reinterpret_cast<void*>(1) : nullptr; }
	bool SetHighAutoLogonPolicy(WinHttpSystemProxyHandle) noexcept override { ++highPolicyCalls; return highPolicy; }
	bool SetStatusCallback(WinHttpSystemProxyHandle) noexcept override
	{
		setCallbackOrder = ++sequence;
		return setCallback;
	}
	bool CreateProxyResolver(WinHttpSystemProxyHandle, WinHttpSystemProxyHandle& value) noexcept override
	{
		++createResolverCalls;
		createResolverOrder = ++sequence;
		value = (createResolver || returnResolverOnCreateFailure) ? reinterpret_cast<void*>(2) : nullptr;
		return createResolver;
	}
	bool SetHandleContext(WinHttpSystemProxyHandle, IWinHttpSystemProxyCallbackSink& value) noexcept override
	{
		setContextOrder = ++sequence;
		if (setHandleContext) callbackSink = &value;
		return setHandleContext;
	}
	unsigned long BeginGetProxyForUrl(WinHttpSystemProxyHandle, const std::wstring&, const WinHttpAutoProxyOptions& value,
		IWinHttpSystemProxyCallbackSink& sink) noexcept override
	{
		++beginCalls;
		beginOrder = ++sequence;
		options = value;
		if (cancelOnBegin) cancelOnBegin->store(true);
		if (dispatchCallback && beginStatus == ERROR_IO_PENDING) {
			sink.OnWinHttpSystemProxyCallback(callbackStatus, callbackError);
		}
		return beginStatus;
	}
	bool GetProxyResult(WinHttpSystemProxyHandle, WinHttpSystemProxyResult& value) noexcept override
	{
		++getResultCalls;
		value = result;
		value.nativeResult = reinterpret_cast<void*>(3);
		return getResult;
	}
	void FreeProxyResult(WinHttpSystemProxyResult& value) noexcept override { ++freeResultCalls; value = {}; }
	bool CloseHandle(WinHttpSystemProxyHandle value) noexcept override
	{
		if (value == reinterpret_cast<void*>(2)) {
			++closeResolverCalls;
			if (!closeResolver) return false;
			if (callbackSink && dispatchHandleClosing) {
				if (delayHandleClosing) {
					pendingHandleClosing.store(callbackSink, std::memory_order_release);
				} else {
					callbackSink->OnWinHttpSystemProxyCallback(EWinHttpSystemProxyCallbackStatus::HandleClosing, ERROR_SUCCESS);
				}
			}
			return true;
		}
		++closeSessionCalls;
		return closeSession;
	}
	void DispatchPendingHandleClosing() noexcept
	{
		if (auto* sink = pendingHandleClosing.exchange(nullptr, std::memory_order_acq_rel)) {
			sink->OnWinHttpSystemProxyCallback(EWinHttpSystemProxyCallbackStatus::HandleClosing, ERROR_SUCCESS);
		}
	}

	WinHttpCurrentUserProxyConfig config;
	WinHttpSystemProxyResult result;
	WinHttpAutoProxyOptions options;
	bool readConfig = true;
	bool openSession = true;
	bool highPolicy = true;
	bool setCallback = true;
	bool createResolver = true;
	bool returnResolverOnCreateFailure = false;
	bool setHandleContext = true;
	bool closeResolver = true;
	bool closeSession = true;
	bool getResult = true;
	bool dispatchCallback = true;
	bool dispatchHandleClosing = true;
	bool delayHandleClosing = false;
	std::atomic<bool>* cancelOnBegin = nullptr;
	unsigned long beginStatus = ERROR_IO_PENDING;
	unsigned long callbackError = ERROR_SUCCESS;
	EWinHttpSystemProxyCallbackStatus callbackStatus = EWinHttpSystemProxyCallbackStatus::GetProxyForUrlComplete;
	IWinHttpSystemProxyCallbackSink* callbackSink = nullptr;
	std::atomic<IWinHttpSystemProxyCallbackSink*> pendingHandleClosing{ nullptr };
	int sequence = 0;
	int setCallbackOrder = 0;
	int createResolverOrder = 0;
	int setContextOrder = 0;
	int beginOrder = 0;
	int openCalls = 0;
	int highPolicyCalls = 0;
	int createResolverCalls = 0;
	int beginCalls = 0;
	int getResultCalls = 0;
	int freeResultCalls = 0;
	int closeResolverCalls = 0;
	int closeSessionCalls = 0;
};

ProxyRequest Request(std::wstring url = L"https://service.example.test/resource") { return { std::move(url), EProxySupport::On }; }

} // namespace

TEST(WinHttpSystemProxyResolverTest, UsesCurrentUserStaticProxyAndBypassWithoutOpeningWinHttp)
{
	FakeFacade facade;
	facade.config.proxy = L" https=secure-proxy.example.test:8443; ";
	WinHttpSystemProxyResolver resolver(facade);

	const auto selected = resolver.Resolve(Request(), std::nullopt, nullptr);
	ASSERT_EQ(ESystemProxyResolutionOutcome::Selected, selected.outcome);
	ASSERT_EQ(EProxyMode::Manual, selected.selection.mode);
	ASSERT_TRUE(selected.selection.proxyUrl.has_value());
	EXPECT_EQ(L"http://secure-proxy.example.test:8443", *selected.selection.proxyUrl);
	EXPECT_EQ(0, facade.openCalls);

	facade.config.proxyBypass = L" unrelated.test ; *.example.test ; ";
	const auto bypassed = resolver.Resolve(Request(), std::nullopt, nullptr);
	EXPECT_EQ(EProxyMode::Direct, bypassed.selection.mode);
	EXPECT_TRUE(bypassed.selection.bypassed);
	EXPECT_EQ(0, facade.openCalls);
}

TEST(WinHttpSystemProxyResolverTest, SupportsDefaultPortAndIPv6BypassIncludingLoopbackOptOut)
{
	FakeFacade facade;
	facade.config.proxy = L"http://proxy.example.test";
	WinHttpSystemProxyResolver resolver(facade);

	const auto selected = resolver.Resolve(Request(), std::nullopt, nullptr);
	ASSERT_EQ(ESystemProxyResolutionOutcome::Selected, selected.outcome);
	ASSERT_TRUE(selected.selection.proxyUrl.has_value());
	EXPECT_EQ(L"http://proxy.example.test:80", *selected.selection.proxyUrl);

	const auto loopback = resolver.Resolve(Request(L"https://[::1]/"), std::nullopt, nullptr);
	EXPECT_EQ(EProxyMode::Direct, loopback.selection.mode);
	EXPECT_TRUE(loopback.selection.bypassed);

	facade.config.proxyBypass = L"<-loopback> ; [::1]:443";
	const auto explicitIpv6 = resolver.Resolve(Request(L"https://[::1]/"), std::nullopt, nullptr);
	EXPECT_EQ(EProxyMode::Direct, explicitIpv6.selection.mode);
	EXPECT_TRUE(explicitIpv6.selection.bypassed);

	facade.config.proxyBypass = L"<-loopback>";
	const auto noImplicitLoopback = resolver.Resolve(Request(L"https://[::1]/"), std::nullopt, nullptr);
	EXPECT_EQ(EProxyMode::Manual, noImplicitLoopback.selection.mode);
}

TEST(WinHttpSystemProxyResolverTest, RejectsCredentialsSchemesAndAmbiguousStaticProxyWithoutWinHttp)
{
	for (const auto& proxy : { L"user:secret@proxy.example.test:80", L"https://proxy.example.test:443", L"http=a.example.test:80;http=b.example.test:80" }) {
		FakeFacade facade;
		facade.config.proxy = proxy;
		WinHttpSystemProxyResolver resolver(facade);
		EXPECT_EQ(ESystemProxyResolutionOutcome::InvalidResult, resolver.Resolve(Request(), std::nullopt, nullptr).outcome);
		EXPECT_EQ(0, facade.openCalls);
	}
}

TEST(WinHttpSystemProxyResolverTest, RunsPACWithHighAutoLogonDisabledAndSelectsOneHttpResult)
{
	FakeFacade facade;
	facade.config.autoDetect = true;
	facade.config.autoConfigUrl = L"https://pac.example.test/proxy.pac";
	facade.result.entries = { { true, false, INTERNET_SCHEME_HTTP, L"proxy.example.test", 8080 } };
	WinHttpSystemProxyResolver resolver(facade);

	const auto selected = resolver.Resolve(Request(), std::nullopt, nullptr);
	ASSERT_EQ(ESystemProxyResolutionOutcome::Selected, selected.outcome);
	ASSERT_TRUE(selected.selection.proxyUrl.has_value());
	EXPECT_EQ(L"http://proxy.example.test:8080", *selected.selection.proxyUrl);
	EXPECT_TRUE(facade.options.autoDetect);
	EXPECT_EQ(L"https://pac.example.test/proxy.pac", facade.options.autoConfigUrl);
	EXPECT_FALSE(facade.options.autoLogonIfChallenged);
	EXPECT_EQ(1, facade.highPolicyCalls);
	EXPECT_EQ(1, facade.closeResolverCalls);
	EXPECT_EQ(1, facade.closeSessionCalls);
	EXPECT_EQ(1, facade.freeResultCalls);
	EXPECT_LT(facade.setCallbackOrder, facade.createResolverOrder);
	EXPECT_LT(facade.createResolverOrder, facade.setContextOrder);
	EXPECT_LT(facade.setContextOrder, facade.beginOrder);
}

TEST(WinHttpSystemProxyResolverTest, AutoProxyPrecedesStaticAndKnownAutoFailureFallsBackToStatic)
{
	FakeFacade autoFacade;
	autoFacade.config.autoDetect = true;
	autoFacade.config.proxy = L"https=static-proxy.example.test:8443";
	autoFacade.result.entries = { { true, false, INTERNET_SCHEME_HTTP, L"pac-proxy.example.test", 8080 } };
	WinHttpSystemProxyResolver autoResolver(autoFacade);

	const auto fromPac = autoResolver.Resolve(Request(), std::nullopt, nullptr);
	ASSERT_EQ(ESystemProxyResolutionOutcome::Selected, fromPac.outcome);
	ASSERT_TRUE(fromPac.selection.proxyUrl.has_value());
	EXPECT_EQ(L"http://pac-proxy.example.test:8080", *fromPac.selection.proxyUrl);
	EXPECT_EQ(1, autoFacade.openCalls);

	FakeFacade fallbackFacade;
	fallbackFacade.config.autoDetect = true;
	fallbackFacade.config.proxy = L"https=static-proxy.example.test:8443";
	fallbackFacade.callbackStatus = EWinHttpSystemProxyCallbackStatus::RequestError;
	fallbackFacade.callbackError = ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT;
	WinHttpSystemProxyResolver fallbackResolver(fallbackFacade);

	const auto fallback = fallbackResolver.Resolve(Request(), std::nullopt, nullptr);
	ASSERT_EQ(ESystemProxyResolutionOutcome::Selected, fallback.outcome);
	ASSERT_TRUE(fallback.selection.proxyUrl.has_value());
	EXPECT_EQ(L"http://static-proxy.example.test:8443", *fallback.selection.proxyUrl);
}

TEST(WinHttpSystemProxyResolverTest, RejectsFailoverSocksHttpsAndMalformedPACResultsAndAlwaysFreesThem)
{
	for (const auto scheme : { static_cast<unsigned long>(INTERNET_SCHEME_HTTPS), static_cast<unsigned long>(INTERNET_SCHEME_SOCKS) }) {
		FakeFacade facade;
		facade.config.autoDetect = true;
		facade.result.entries = { { true, false, scheme, L"proxy.example.test", 8080 } };
		WinHttpSystemProxyResolver resolver(facade);
		EXPECT_EQ(ESystemProxyResolutionOutcome::InvalidResult, resolver.Resolve(Request(), std::nullopt, nullptr).outcome);
		EXPECT_EQ(1, facade.freeResultCalls);
	}
	FakeFacade failover;
	failover.config.autoDetect = true;
	failover.result.entries = { { true, false, INTERNET_SCHEME_HTTP, L"a.example.test", 80 }, { false, false, 0, L"", 0 } };
	WinHttpSystemProxyResolver resolver(failover);
	EXPECT_EQ(ESystemProxyResolutionOutcome::InvalidResult, resolver.Resolve(Request(), std::nullopt, nullptr).outcome);
	EXPECT_EQ(1, failover.freeResultCalls);

	FakeFacade malformed;
	malformed.config.autoDetect = true;
	malformed.result.entries = { { true, false, INTERNET_SCHEME_HTTP, L"bad@proxy.example.test", 80 } };
	WinHttpSystemProxyResolver malformedResolver(malformed);
	EXPECT_EQ(ESystemProxyResolutionOutcome::InvalidResult, malformedResolver.Resolve(Request(), std::nullopt, nullptr).outcome);
	EXPECT_EQ(1, malformed.freeResultCalls);
}

TEST(WinHttpSystemProxyResolverTest, SelectsAWellFormedDirectPACResult)
{
	FakeFacade facade;
	facade.config.autoDetect = true;
	facade.result.entries = { { false, true, 0, L"", 0 } };
	WinHttpSystemProxyResolver resolver(facade);

	const auto selected = resolver.Resolve(Request(), std::nullopt, nullptr);
	EXPECT_EQ(ESystemProxyResolutionOutcome::Selected, selected.outcome);
	EXPECT_EQ(EProxyMode::Direct, selected.selection.mode);
	EXPECT_TRUE(selected.selection.bypassed);
	EXPECT_EQ(1, facade.freeResultCalls);
}

TEST(WinHttpSystemProxyResolverTest, ClassifiesKnownAndUnknownCallbackFailuresAndAlwaysFreesResults)
{
	FakeFacade callbackFailure;
	callbackFailure.config.autoDetect = true;
	callbackFailure.callbackStatus = EWinHttpSystemProxyCallbackStatus::RequestError;
	callbackFailure.callbackError = ERROR_WINHTTP_BAD_AUTO_PROXY_SCRIPT;
	WinHttpSystemProxyResolver callbackResolver(callbackFailure);
	EXPECT_EQ(ESystemProxyResolutionOutcome::Unavailable, callbackResolver.Resolve(Request(), std::nullopt, nullptr).outcome);
	EXPECT_EQ(0, callbackFailure.getResultCalls);
	EXPECT_EQ(0, callbackFailure.freeResultCalls);
	EXPECT_EQ(1, callbackFailure.closeResolverCalls);

	FakeFacade unknownFailure;
	unknownFailure.config.autoDetect = true;
	unknownFailure.callbackStatus = EWinHttpSystemProxyCallbackStatus::RequestError;
	unknownFailure.callbackError = ERROR_ACCESS_DENIED;
	WinHttpSystemProxyResolver unknownResolver(unknownFailure);
	EXPECT_EQ(ESystemProxyResolutionOutcome::InvalidResult, unknownResolver.Resolve(Request(), std::nullopt, nullptr).outcome);
	EXPECT_EQ(0, unknownFailure.getResultCalls);

	FakeFacade resultFailure;
	resultFailure.config.autoDetect = true;
	resultFailure.getResult = false;
	WinHttpSystemProxyResolver resultResolver(resultFailure);
	EXPECT_EQ(ESystemProxyResolutionOutcome::InvalidResult, resultResolver.Resolve(Request(), std::nullopt, nullptr).outcome);
	EXPECT_EQ(1, resultFailure.getResultCalls);
	EXPECT_EQ(1, resultFailure.freeResultCalls);
}

TEST(WinHttpSystemProxyResolverTest, SetupAndImmediateApiFailuresHaveExplicitInvalidTerminalsAndCloseReturnedHandles)
{
	FakeFacade openFailure;
	openFailure.config.autoDetect = true;
	openFailure.openSession = false;
	WinHttpSystemProxyResolver openResolver(openFailure);
	EXPECT_EQ(ESystemProxyResolutionOutcome::InvalidResult, openResolver.Resolve(Request(), std::nullopt, nullptr).outcome);

	FakeFacade createFailure;
	createFailure.config.autoDetect = true;
	createFailure.createResolver = false;
	createFailure.returnResolverOnCreateFailure = true;
	WinHttpSystemProxyResolver createResolver(createFailure);
	EXPECT_EQ(ESystemProxyResolutionOutcome::InvalidResult, createResolver.Resolve(Request(), std::nullopt, nullptr).outcome);
	EXPECT_EQ(1, createFailure.closeResolverCalls);

	FakeFacade contextFailure;
	contextFailure.config.autoDetect = true;
	contextFailure.setHandleContext = false;
	WinHttpSystemProxyResolver contextResolver(contextFailure);
	EXPECT_EQ(ESystemProxyResolutionOutcome::InvalidResult, contextResolver.Resolve(Request(), std::nullopt, nullptr).outcome);
	EXPECT_EQ(1, contextFailure.closeResolverCalls);

	FakeFacade beginFailure;
	beginFailure.config.autoDetect = true;
	beginFailure.beginStatus = ERROR_INVALID_HANDLE;
	WinHttpSystemProxyResolver beginResolver(beginFailure);
	EXPECT_EQ(ESystemProxyResolutionOutcome::InvalidResult, beginResolver.Resolve(Request(), std::nullopt, nullptr).outcome);
	EXPECT_EQ(1, beginFailure.closeResolverCalls);
}

TEST(WinHttpSystemProxyResolverTest, CancellationAndDeadlineHaveExplicitTerminalsWithoutStartingAResolver)
{
	FakeFacade facade;
	facade.config.autoDetect = true;
	WinHttpSystemProxyResolver resolver(facade);
	Cancelled cancellation;
	cancellation.cancelled.store(true);
	EXPECT_EQ(ESystemProxyResolutionOutcome::Cancelled, resolver.Resolve(Request(), std::nullopt, &cancellation).outcome);
	EXPECT_EQ(ESystemProxyResolutionOutcome::DeadlineExceeded,
		resolver.Resolve(Request(), std::chrono::steady_clock::now() - std::chrono::milliseconds(1), nullptr).outcome);
	EXPECT_EQ(0, facade.openCalls);
}

TEST(WinHttpSystemProxyResolverTest, CancellationDuringPendingPACResolutionClosesOnlyTheCallOwnedResolverThenQuiesces)
{
	FakeFacade facade;
	facade.config.autoDetect = true;
	facade.dispatchCallback = false;
	Cancelled cancellation;
	facade.cancelOnBegin = &cancellation.cancelled;
	WinHttpSystemProxyResolver resolver(facade);

	EXPECT_EQ(ESystemProxyResolutionOutcome::Cancelled, resolver.Resolve(Request(), std::nullopt, &cancellation).outcome);
	EXPECT_EQ(1, facade.beginCalls);
	EXPECT_EQ(1, facade.closeResolverCalls);
	EXPECT_EQ(1, facade.closeSessionCalls);
	EXPECT_EQ(0, facade.getResultCalls);
}

TEST(WinHttpSystemProxyResolverTest, DeadlineDuringPendingPACResolutionClosesResolverAndReturnsDeadlineTerminal)
{
	FakeFacade facade;
	facade.config.autoDetect = true;
	facade.dispatchCallback = false;
	WinHttpSystemProxyResolver resolver(facade);

	EXPECT_EQ(ESystemProxyResolutionOutcome::DeadlineExceeded,
		resolver.Resolve(Request(), std::chrono::steady_clock::now() + std::chrono::milliseconds(5), nullptr).outcome);
	EXPECT_EQ(1, facade.closeResolverCalls);
	EXPECT_EQ(1, facade.closeSessionCalls);
}

TEST(WinHttpSystemProxyResolverTest, WaitsForFinalHandleClosingBeforeCallbackStateCanBeReleased)
{
	FakeFacade facade;
	facade.config.autoDetect = true;
	facade.delayHandleClosing = true;
	facade.result.entries = { { false, false, 0, L"", 0 } };
	WinHttpSystemProxyResolver resolver(facade);

	auto resolved = std::async(std::launch::async, [&] { return resolver.Resolve(Request(), std::nullopt, nullptr); });
	const auto observationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	while (facade.pendingHandleClosing.load(std::memory_order_acquire) == nullptr
		&& std::chrono::steady_clock::now() < observationDeadline) {
		std::this_thread::yield();
	}
	const bool closeWasPending = facade.pendingHandleClosing.load(std::memory_order_acquire) != nullptr;
	EXPECT_TRUE(closeWasPending);
	if (closeWasPending) {
		EXPECT_EQ(std::future_status::timeout, resolved.wait_for(std::chrono::milliseconds(10)));
	}
	facade.DispatchPendingHandleClosing();
	ASSERT_EQ(std::future_status::ready, resolved.wait_for(std::chrono::seconds(1)));
	EXPECT_EQ(ESystemProxyResolutionOutcome::Selected, resolved.get().outcome);
}
