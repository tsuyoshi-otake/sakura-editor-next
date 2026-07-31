/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"
#include "platform/request/win32/WinHttpSystemProxyResolver.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <limits>
#include <mutex>
#include <new>
#include <string_view>
#include <utility>

namespace platform::request::win32 {
namespace {

constexpr unsigned long kErrorIoPending = ERROR_IO_PENDING;
constexpr std::chrono::milliseconds kCancellationPollInterval(25);

config::SystemProxyResolution Resolution(config::ESystemProxyResolutionOutcome outcome) noexcept
{
	return { outcome, {} };
}

config::SystemProxyResolution Direct(bool bypassed = false) noexcept
{
	return { config::ESystemProxyResolutionOutcome::Selected,
		{ EProxyMode::Direct, std::nullopt, bypassed, EProxySelectionOutcome::Selected } };
}

config::SystemProxyResolution Manual(std::wstring proxyUrl)
{
	return { config::ESystemProxyResolutionOutcome::Selected,
		{ EProxyMode::Manual, std::move(proxyUrl), false, EProxySelectionOutcome::Selected } };
}

bool IsCancelled(const IRequestCancellation* cancellation) noexcept
{
	return cancellation != nullptr && cancellation->IsCancellationRequested();
}

bool IsDeadlineExceeded(const std::optional<std::chrono::steady_clock::time_point>& deadline) noexcept
{
	return deadline.has_value() && std::chrono::steady_clock::now() >= *deadline;
}

bool IsAsciiSpace(wchar_t value) noexcept
{
	return value == L' ' || (value >= L'\t' && value <= L'\r');
}

std::wstring_view TrimAsciiSpace(std::wstring_view value) noexcept
{
	while (!value.empty() && IsAsciiSpace(value.front())) value.remove_prefix(1);
	while (!value.empty() && IsAsciiSpace(value.back())) value.remove_suffix(1);
	return value;
}

std::wstring Lowered(std::wstring_view value)
{
	std::wstring result;
	result.reserve(value.size());
	for (const auto character : value) {
		result.push_back(static_cast<wchar_t>(std::towlower(character)));
	}
	return result;
}

bool ParseTarget(std::wstring_view url, std::wstring& scheme, std::wstring& host, unsigned short& port)
{
	if (url.empty() || url.size() > std::numeric_limits<DWORD>::max() || url.find(L'\0') != std::wstring_view::npos) return false;
	URL_COMPONENTS components{};
	components.dwStructSize = sizeof(components);
	components.dwSchemeLength = static_cast<DWORD>(-1);
	components.dwHostNameLength = static_cast<DWORD>(-1);
	if (!::WinHttpCrackUrl(url.data(), static_cast<DWORD>(url.size()), 0, &components) || components.dwHostNameLength == 0) return false;
	if (components.nScheme == INTERNET_SCHEME_HTTP) scheme = L"http";
	else if (components.nScheme == INTERNET_SCHEME_HTTPS) scheme = L"https";
	else return false;
	host.assign(components.lpszHostName, components.dwHostNameLength);
	// WinHttpCrackUrl preserves the brackets around an IPv6 literal.  The
	// bypass grammar compares host names without URI authority delimiters.
	if (host.size() >= 2 && host.front() == L'[' && host.back() == L']') {
		host = host.substr(1, host.size() - 2);
	}
	port = components.nPort;
	return port != 0;
}

bool ParsePort(std::wstring_view value, unsigned short& port) noexcept
{
	if (value.empty() || value.size() > 5) return false;
	unsigned long parsed = 0;
	for (const auto character : value) {
		if (character < L'0' || character > L'9') return false;
		parsed = parsed * 10 + static_cast<unsigned long>(character - L'0');
		if (parsed > 65535) return false;
	}
	if (parsed == 0) return false;
	port = static_cast<unsigned short>(parsed);
	return true;
}

bool IsValidHost(std::wstring_view host) noexcept
{
	if (host.empty() || host.size() > 253 || host.find_first_of(L"@/?#\\;=\r\n") != std::wstring_view::npos) return false;
	for (const auto character : host) {
		if (IsAsciiSpace(character) || character < 0x21) return false;
	}
	return true;
}

bool ParseHttpProxyAuthority(std::wstring_view value, std::wstring& proxyUrl)
{
	value = TrimAsciiSpace(value);
	if (value.empty()) return false;
	auto lowered = Lowered(value);
	if (lowered.starts_with(L"http://")) {
		value.remove_prefix(7);
	} else if (lowered.find(L"://") != std::wstring::npos) {
		return false;
	}
	if (value.empty() || value.find_first_of(L"@/?#\\;=\r\n") != std::wstring_view::npos) return false;
	std::wstring_view host;
	std::wstring_view portText;
	unsigned short port = 80;
	if (value.front() == L'[') {
		const auto close = value.find(L']');
		if (close == std::wstring_view::npos) return false;
		host = value.substr(0, close + 1);
		if (close + 1 < value.size()) {
			if (value[close + 1] != L':' || close + 2 >= value.size()) return false;
			portText = value.substr(close + 2);
			if (!ParsePort(portText, port)) return false;
		}
	} else {
		const auto colon = value.rfind(L':');
		if (colon == std::wstring_view::npos) {
			host = value;
		} else {
			if (value.find(L':') != colon) return false;
			host = value.substr(0, colon);
			portText = value.substr(colon + 1);
			if (!ParsePort(portText, port)) return false;
		}
	}
	if (!IsValidHost(host)) return false;
	proxyUrl.assign(L"http://");
	proxyUrl.append(host);
	proxyUrl.push_back(L':');
	proxyUrl.append(std::to_wstring(port));
	URL_COMPONENTS components{};
	components.dwStructSize = sizeof(components);
	components.dwHostNameLength = static_cast<DWORD>(-1);
	return ::WinHttpCrackUrl(proxyUrl.data(), static_cast<DWORD>(proxyUrl.size()), 0, &components) != FALSE
		&& components.nScheme == INTERNET_SCHEME_HTTP && components.dwHostNameLength != 0 && components.nPort == port;
}

enum class EBypassMatch : std::uint8_t { NoMatch, Match, Invalid };

bool IsLoopbackHost(std::wstring_view host) noexcept
{
	return host == L"localhost" || host == L"loopback" || host == L"127.0.0.1" || host == L"::1";
}

EBypassMatch MatchesBypassToken(
	std::wstring_view rawToken,
	std::wstring_view targetHost,
	unsigned short targetPort,
	bool& disablesImplicitLoopback
)
{
	rawToken = TrimAsciiSpace(rawToken);
	if (rawToken.empty()) return EBypassMatch::NoMatch;
	auto token = Lowered(rawToken);
	auto host = Lowered(targetHost);
	if (token == L"*") return EBypassMatch::Match;
	if (token == L"<local>") return host.find(L'.') == std::wstring::npos ? EBypassMatch::Match : EBypassMatch::NoMatch;
	if (token == L"<-loopback>") {
		disablesImplicitLoopback = true;
		return EBypassMatch::NoMatch;
	}
	if (token.front() == L'<') return EBypassMatch::Invalid;

	std::wstring_view tokenHost(token);
	if (token.front() == L'[') {
		const auto close = token.find(L']');
		if (close == std::wstring::npos) return EBypassMatch::Invalid;
		tokenHost = std::wstring_view(token).substr(1, close - 1);
		if (close + 1 < token.size()) {
			if (token[close + 1] != L':' || close + 2 >= token.size()) return EBypassMatch::Invalid;
			unsigned short tokenPort = 0;
			if (!ParsePort(std::wstring_view(token).substr(close + 2), tokenPort)) return EBypassMatch::Invalid;
			if (tokenPort != targetPort) return EBypassMatch::NoMatch;
		}
	} else if (const auto colon = token.rfind(L':'); colon != std::wstring::npos && token.find(L':') == colon) {
		unsigned short tokenPort = 0;
		if (!ParsePort(std::wstring_view(token).substr(colon + 1), tokenPort)) return EBypassMatch::Invalid;
		if (tokenPort != targetPort) return EBypassMatch::NoMatch;
		tokenHost = std::wstring_view(token).substr(0, colon);
	}
	if (tokenHost.empty() || tokenHost.find_first_of(L"@/?#\\;=,\r\n") != std::wstring_view::npos) return EBypassMatch::Invalid;
	const auto wildcard = tokenHost.find(L'*');
	if (wildcard != std::wstring_view::npos) {
		if (wildcard != 0 || tokenHost.find(L'*', 1) != std::wstring_view::npos || tokenHost.size() == 1) {
			return EBypassMatch::Invalid;
		}
		const auto suffix = tokenHost.substr(1);
		return host.size() > suffix.size() && host.ends_with(suffix) ? EBypassMatch::Match : EBypassMatch::NoMatch;
	}
	if (tokenHost.find(L'?') != std::wstring_view::npos) return EBypassMatch::Invalid;
	if (tokenHost.starts_with(L".")) {
		return host == tokenHost.substr(1) || host.ends_with(tokenHost) ? EBypassMatch::Match : EBypassMatch::NoMatch;
	}
	return host == tokenHost ? EBypassMatch::Match : EBypassMatch::NoMatch;
}

EBypassMatch IsBypassed(std::wstring_view bypasses, std::wstring_view host, unsigned short port)
{
	bool matched = false;
	bool disablesImplicitLoopback = false;
	std::size_t start = 0;
	while (start <= bypasses.size()) {
		const auto end = bypasses.find_first_of(L";, \t\r\n", start);
		const auto token = bypasses.substr(start, end == std::wstring_view::npos ? bypasses.size() - start : end - start);
		const auto result = MatchesBypassToken(token, host, port, disablesImplicitLoopback);
		if (result == EBypassMatch::Invalid) return result;
		if (result == EBypassMatch::Match) matched = true;
		if (end == std::wstring_view::npos) break;
		start = end + 1;
	}
	if (matched || (!disablesImplicitLoopback && IsLoopbackHost(Lowered(host)))) return EBypassMatch::Match;
	return EBypassMatch::NoMatch;
}

enum class EStaticProxyParse : std::uint8_t { None, Valid, Invalid };

EStaticProxyParse SelectStaticProxy(std::wstring_view rawProxy, std::wstring_view targetScheme, std::wstring& proxyUrl)
{
	rawProxy = TrimAsciiSpace(rawProxy);
	if (rawProxy.empty()) return EStaticProxyParse::None;
	std::optional<std::wstring_view> selected;
	std::optional<std::wstring_view> generic;
	bool seenHttp = false;
	bool seenHttps = false;
	bool seenFtp = false;
	std::size_t start = 0;
	while (start < rawProxy.size()) {
		while (start < rawProxy.size() && (rawProxy[start] == L';' || IsAsciiSpace(rawProxy[start]))) ++start;
		if (start == rawProxy.size()) break;
		const auto end = rawProxy.find_first_of(L"; \t\r\n", start);
		auto item = TrimAsciiSpace(rawProxy.substr(start, end == std::wstring_view::npos ? rawProxy.size() - start : end - start));
		if (item.empty()) return EStaticProxyParse::Invalid;
		const auto equal = item.find(L'=');
		if (equal == std::wstring_view::npos) {
			if (generic.has_value()) return EStaticProxyParse::Invalid;
			generic = item;
		} else {
			if (equal == 0 || item.find(L'=', equal + 1) != std::wstring_view::npos) return EStaticProxyParse::Invalid;
			const auto scheme = Lowered(item.substr(0, equal));
			if (scheme != L"http" && scheme != L"https" && scheme != L"ftp") return EStaticProxyParse::Invalid;
			bool* seen = scheme == L"http" ? &seenHttp : scheme == L"https" ? &seenHttps : &seenFtp;
			if (*seen) return EStaticProxyParse::Invalid;
			*seen = true;
			if (scheme == targetScheme) {
				selected = item.substr(equal + 1);
			}
		}
		if (end == std::wstring_view::npos) break;
		start = end + 1;
	}
	const auto choice = selected.has_value() ? selected : generic;
	if (!choice.has_value()) return EStaticProxyParse::None;
	return ParseHttpProxyAuthority(*choice, proxyUrl) ? EStaticProxyParse::Valid : EStaticProxyParse::Invalid;
}

class RealWinHttpSystemProxyFacade final : public IWinHttpSystemProxyFacade {
public:
	bool ReadCurrentUserProxyConfig(WinHttpCurrentUserProxyConfig& config) noexcept override
	{
		WINHTTP_CURRENT_USER_IE_PROXY_CONFIG native{};
		if (!::WinHttpGetIEProxyConfigForCurrentUser(&native)) return false;
		struct FreeCurrentUserConfig final {
			WINHTTP_CURRENT_USER_IE_PROXY_CONFIG& value;
			~FreeCurrentUserConfig()
			{
				if (value.lpszAutoConfigUrl) ::GlobalFree(value.lpszAutoConfigUrl);
				if (value.lpszProxy) ::GlobalFree(value.lpszProxy);
				if (value.lpszProxyBypass) ::GlobalFree(value.lpszProxyBypass);
			}
		} freeNative{ native };
		try {
			WinHttpCurrentUserProxyConfig next;
			next.autoDetect = native.fAutoDetect != FALSE;
			if (native.lpszAutoConfigUrl) next.autoConfigUrl = native.lpszAutoConfigUrl;
			if (native.lpszProxy) next.proxy = native.lpszProxy;
			if (native.lpszProxyBypass) next.proxyBypass = native.lpszProxyBypass;
			config = std::move(next);
			return true;
		} catch (...) {
			return false;
		}
	}

	WinHttpSystemProxyHandle OpenAsyncSession() noexcept override
	{
		return ::WinHttpOpen(L"Sakura Editor NEXT SystemProxyResolver", WINHTTP_ACCESS_TYPE_NO_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
	}

	bool SetHighAutoLogonPolicy(WinHttpSystemProxyHandle session) noexcept override
	{
		DWORD policy = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
		return ::WinHttpSetOption(static_cast<HINTERNET>(session), WINHTTP_OPTION_AUTOLOGON_POLICY, &policy, sizeof(policy)) != FALSE;
	}

	bool SetStatusCallback(WinHttpSystemProxyHandle session) noexcept override
	{
		return ::WinHttpSetStatusCallback(static_cast<HINTERNET>(session), &StatusCallback,
			WINHTTP_CALLBACK_FLAG_REQUEST_ERROR | WINHTTP_CALLBACK_FLAG_GETPROXYFORURL_COMPLETE
				| WINHTTP_CALLBACK_FLAG_HANDLES,
			0) != WINHTTP_INVALID_STATUS_CALLBACK;
	}

	bool CreateProxyResolver(WinHttpSystemProxyHandle session, WinHttpSystemProxyHandle& resolver) noexcept override
	{
		HINTERNET native = nullptr;
		if (::WinHttpCreateProxyResolver(static_cast<HINTERNET>(session), &native) != ERROR_SUCCESS) return false;
		resolver = native;
		return true;
	}

	bool SetHandleContext(WinHttpSystemProxyHandle resolver, IWinHttpSystemProxyCallbackSink& callbackSink) noexcept override
	{
		DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&callbackSink);
		return ::WinHttpSetOption(static_cast<HINTERNET>(resolver), WINHTTP_OPTION_CONTEXT_VALUE,
			&context, sizeof(context)) != FALSE;
	}

	unsigned long BeginGetProxyForUrl(WinHttpSystemProxyHandle resolver, const std::wstring& targetUrl,
		const WinHttpAutoProxyOptions& options, IWinHttpSystemProxyCallbackSink& callbackSink) noexcept override
	{
		WINHTTP_AUTOPROXY_OPTIONS native{};
		if (options.autoDetect) {
			native.dwFlags |= WINHTTP_AUTOPROXY_AUTO_DETECT;
			native.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
		}
		if (!options.autoConfigUrl.empty()) {
			native.dwFlags |= WINHTTP_AUTOPROXY_CONFIG_URL;
			native.lpszAutoConfigUrl = const_cast<wchar_t*>(options.autoConfigUrl.c_str());
		}
		native.fAutoLogonIfChallenged = options.autoLogonIfChallenged ? TRUE : FALSE;
		return ::WinHttpGetProxyForUrlEx(static_cast<HINTERNET>(resolver), targetUrl.c_str(), &native,
			reinterpret_cast<DWORD_PTR>(&callbackSink));
	}

	bool GetProxyResult(WinHttpSystemProxyHandle resolver, WinHttpSystemProxyResult& result) noexcept override
	{
		auto* native = new (std::nothrow) WINHTTP_PROXY_RESULT{};
		if (!native) return false;
		result.nativeResult = native;
		if (::WinHttpGetProxyResult(static_cast<HINTERNET>(resolver), native) != ERROR_SUCCESS) return false;
		if (native->cEntries != 0 && native->pEntries == nullptr) return false;
		try {
			result.entries.reserve(native->cEntries);
			for (DWORD index = 0; index < native->cEntries; ++index) {
				const auto& entry = native->pEntries[index];
				result.entries.push_back({ entry.fProxy != FALSE, entry.fBypass != FALSE,
					static_cast<unsigned long>(entry.ProxyScheme), entry.pwszProxy ? entry.pwszProxy : L"", entry.ProxyPort });
			}
			return true;
		} catch (...) {
			result.entries.clear();
			return false;
		}
	}

	void FreeProxyResult(WinHttpSystemProxyResult& result) noexcept override
	{
		if (auto* native = static_cast<WINHTTP_PROXY_RESULT*>(result.nativeResult)) {
			::WinHttpFreeProxyResult(native);
			delete native;
		}
		result = {};
	}

	bool CloseHandle(WinHttpSystemProxyHandle handle) noexcept override
	{
		return !handle || ::WinHttpCloseHandle(static_cast<HINTERNET>(handle)) != FALSE;
	}

private:
	static void CALLBACK StatusCallback(HINTERNET, DWORD_PTR context, DWORD status, LPVOID information, DWORD informationLength) noexcept
	{
		if (context == 0) return;
		auto* callbackSink = reinterpret_cast<IWinHttpSystemProxyCallbackSink*>(context);
		if (status == WINHTTP_CALLBACK_STATUS_GETPROXYFORURL_COMPLETE) {
			callbackSink->OnWinHttpSystemProxyCallback(EWinHttpSystemProxyCallbackStatus::GetProxyForUrlComplete, ERROR_SUCCESS);
		} else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
			const auto* result = informationLength >= sizeof(WINHTTP_ASYNC_RESULT)
				? static_cast<const WINHTTP_ASYNC_RESULT*>(information)
				: nullptr;
			callbackSink->OnWinHttpSystemProxyCallback(EWinHttpSystemProxyCallbackStatus::RequestError,
				result ? result->dwError : ERROR_GEN_FAILURE);
		} else if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
			callbackSink->OnWinHttpSystemProxyCallback(EWinHttpSystemProxyCallbackStatus::HandleClosing, ERROR_SUCCESS);
		}
	}
};

} // namespace

class WinHttpSystemProxyResolver::CallbackState final : public IWinHttpSystemProxyCallbackSink {
public:
	enum class ECompletion : std::uint8_t { Pending, Complete, Failed, Cancelled, DeadlineExceeded };
	struct WaitResult final {
		ECompletion completion = ECompletion::Pending;
		unsigned long error = ERROR_SUCCESS;
	};

	static std::shared_ptr<CallbackState> Create()
	{
		auto state = std::shared_ptr<CallbackState>(new CallbackState());
		state->m_selfHold = state;
		return state;
	}

	WaitResult Wait(const std::optional<std::chrono::steady_clock::time_point>& deadline, const IRequestCancellation* cancellation)
	{
		std::unique_lock lock(m_mutex);
		while (m_completion == ECompletion::Pending) {
			if (IsCancelled(cancellation)) return { ECompletion::Cancelled, ERROR_WINHTTP_OPERATION_CANCELLED };
			if (IsDeadlineExceeded(deadline)) return { ECompletion::DeadlineExceeded, ERROR_TIMEOUT };
			const auto wake = deadline.has_value()
				? std::min(*deadline, std::chrono::steady_clock::now() + kCancellationPollInterval)
				: std::chrono::steady_clock::now() + kCancellationPollInterval;
			m_changed.wait_until(lock, wake, [this] { return m_completion != ECompletion::Pending; });
		}
		return { m_completion, m_error };
	}

	void WaitForHandleClosing() noexcept
	{
		std::unique_lock lock(m_mutex);
		m_changed.wait(lock, [this] { return m_handleClosing; });
	}

	//! Breaks the self-hold only before the state has ever been associated with a
	//! native handle. Once SetHandleContext succeeds, HANDLE_CLOSING owns release.
	void DisarmBeforeBinding() noexcept
	{
		std::lock_guard lock(m_mutex);
		m_selfHold.reset();
	}

private:
	CallbackState() = default;

	void OnWinHttpSystemProxyCallback(EWinHttpSystemProxyCallbackStatus status, unsigned long error) noexcept override
	{
		std::shared_ptr<CallbackState> closingKeepAlive;
		{
			std::lock_guard lock(m_mutex);
			if (status == EWinHttpSystemProxyCallbackStatus::HandleClosing) {
				m_handleClosing = true;
				closingKeepAlive = std::move(m_selfHold);
			} else if (m_completion == ECompletion::Pending) {
				m_completion = status == EWinHttpSystemProxyCallbackStatus::GetProxyForUrlComplete
					? ECompletion::Complete
					: ECompletion::Failed;
				m_error = error;
			}
		}
		m_changed.notify_all();
	}

	std::mutex m_mutex;
	std::condition_variable m_changed;
	ECompletion m_completion = ECompletion::Pending;
	unsigned long m_error = ERROR_SUCCESS;
	bool m_handleClosing = false;
	std::shared_ptr<CallbackState> m_selfHold;
};

namespace {

bool IsAutoProxyUnavailableError(unsigned long error) noexcept
{
	return error == ERROR_WINHTTP_AUTODETECTION_FAILED
		|| error == ERROR_WINHTTP_AUTO_PROXY_SERVICE_ERROR
		|| error == ERROR_WINHTTP_BAD_AUTO_PROXY_SCRIPT
		|| error == ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT;
}

config::SystemProxyResolution MapAutoProxyFailure(
	unsigned long error,
	const std::optional<std::chrono::steady_clock::time_point>& deadline,
	const IRequestCancellation* cancellation
) noexcept
{
	if (IsCancelled(cancellation)) return Resolution(config::ESystemProxyResolutionOutcome::Cancelled);
	if (IsDeadlineExceeded(deadline)) return Resolution(config::ESystemProxyResolutionOutcome::DeadlineExceeded);
	if (IsAutoProxyUnavailableError(error)) return Resolution(config::ESystemProxyResolutionOutcome::Unavailable);
	return Resolution(config::ESystemProxyResolutionOutcome::InvalidResult);
}

config::SystemProxyResolution ResolveStaticProxy(
	EStaticProxyParse parsed,
	std::wstring proxyUrl,
	std::wstring_view bypasses,
	std::wstring_view targetHost,
	unsigned short targetPort
)
{
	if (parsed == EStaticProxyParse::Invalid) return Resolution(config::ESystemProxyResolutionOutcome::InvalidResult);
	// An empty static proxy configuration is the system's authoritative "no
	// proxy applies here" answer (VS Code's Electron resolver would report
	// DIRECT), not a failure to resolve one -- so this is a selection.
	if (parsed == EStaticProxyParse::None) return Resolution(config::ESystemProxyResolutionOutcome::NoProxyRequired);
	const auto bypass = IsBypassed(bypasses, targetHost, targetPort);
	if (bypass == EBypassMatch::Invalid) return Resolution(config::ESystemProxyResolutionOutcome::InvalidResult);
	if (bypass == EBypassMatch::Match) return Direct(true);
	return Manual(std::move(proxyUrl));
}

} // namespace

WinHttpSystemProxyResolver::WinHttpSystemProxyResolver()
	: m_ownedFacade(std::make_unique<RealWinHttpSystemProxyFacade>())
	, m_facade(m_ownedFacade.get())
{
}

WinHttpSystemProxyResolver::WinHttpSystemProxyResolver(IWinHttpSystemProxyFacade& facade) noexcept
	: m_facade(&facade)
{
}

WinHttpSystemProxyResolver::~WinHttpSystemProxyResolver() = default;

config::SystemProxyResolution WinHttpSystemProxyResolver::Resolve(
	const ProxyRequest& request,
	std::optional<std::chrono::steady_clock::time_point> deadline,
	const IRequestCancellation* cancellation
)
{
	if (IsCancelled(cancellation)) return Resolution(config::ESystemProxyResolutionOutcome::Cancelled);
	if (IsDeadlineExceeded(deadline)) return Resolution(config::ESystemProxyResolutionOutcome::DeadlineExceeded);

	std::wstring targetScheme;
	std::wstring targetHost;
	unsigned short targetPort = 0;
	if (!ParseTarget(request.targetUrl, targetScheme, targetHost, targetPort)) return Resolution(config::ESystemProxyResolutionOutcome::InvalidResult);

	WinHttpCurrentUserProxyConfig config;
	if (!m_facade->ReadCurrentUserProxyConfig(config)) return Resolution(config::ESystemProxyResolutionOutcome::Unavailable);
	if (IsCancelled(cancellation)) return Resolution(config::ESystemProxyResolutionOutcome::Cancelled);
	if (IsDeadlineExceeded(deadline)) return Resolution(config::ESystemProxyResolutionOutcome::DeadlineExceeded);

	std::wstring staticProxy;
	const auto staticProxyParse = SelectStaticProxy(config.proxy, targetScheme, staticProxy);
	const bool hasAutoProxy = config.autoDetect || !config.autoConfigUrl.empty();
	if (!hasAutoProxy) {
		return ResolveStaticProxy(staticProxyParse, std::move(staticProxy), config.proxyBypass, targetHost, targetPort);
	}
	if (!config.autoConfigUrl.empty()) {
		std::wstring pacScheme;
		std::wstring pacHost;
		unsigned short pacPort = 0;
		if (!ParseTarget(config.autoConfigUrl, pacScheme, pacHost, pacPort)) {
			return Resolution(config::ESystemProxyResolutionOutcome::InvalidResult);
		}
	}

	auto session = m_facade->OpenAsyncSession();
	if (!session) return Resolution(config::ESystemProxyResolutionOutcome::InvalidResult);
	struct CloseSession final {
		IWinHttpSystemProxyFacade& facade;
		WinHttpSystemProxyHandle handle;
		~CloseSession() { (void)facade.CloseHandle(handle); }
	} closeSession{ *m_facade, session };
	if (!m_facade->SetHighAutoLogonPolicy(session)) return Resolution(config::ESystemProxyResolutionOutcome::InvalidResult);
	if (!m_facade->SetStatusCallback(session)) return Resolution(config::ESystemProxyResolutionOutcome::InvalidResult);

	WinHttpSystemProxyHandle resolver = nullptr;
	if (!m_facade->CreateProxyResolver(session, resolver) || !resolver) {
		if (resolver) (void)m_facade->CloseHandle(resolver);
		return Resolution(config::ESystemProxyResolutionOutcome::InvalidResult);
	}
	auto callbackState = CallbackState::Create();
	if (!m_facade->SetHandleContext(resolver, *callbackState)) {
		(void)m_facade->CloseHandle(resolver);
		callbackState->DisarmBeforeBinding();
		return Resolution(config::ESystemProxyResolutionOutcome::InvalidResult);
	}

	const auto closeResolver = [&]() noexcept {
		if (resolver) {
			const auto handle = resolver;
			resolver = nullptr;
			if (!m_facade->CloseHandle(handle)) return false;
			// HANDLE_CLOSING is the documented final callback for this resolver.
			// Waiting for it closes the late-callback window before state teardown.
			callbackState->WaitForHandleClosing();
		}
		return true;
	};

	WinHttpAutoProxyOptions options{ config.autoDetect, config.autoConfigUrl, false };
	const auto beginStatus = m_facade->BeginGetProxyForUrl(resolver, request.targetUrl, options, *callbackState);
	if (beginStatus != kErrorIoPending) {
		auto outcome = MapAutoProxyFailure(beginStatus, deadline, cancellation);
		if (!closeResolver()) return Resolution(config::ESystemProxyResolutionOutcome::InvalidResult);
		if (outcome.outcome == config::ESystemProxyResolutionOutcome::Unavailable) {
			return ResolveStaticProxy(staticProxyParse, std::move(staticProxy), config.proxyBypass, targetHost, targetPort);
		}
		return outcome;
	}

	config::SystemProxyResolution outcome = Resolution(config::ESystemProxyResolutionOutcome::InvalidResult);
	const auto completion = callbackState->Wait(deadline, cancellation);
	if (completion.completion == CallbackState::ECompletion::Cancelled || IsCancelled(cancellation)) {
		outcome = Resolution(config::ESystemProxyResolutionOutcome::Cancelled);
	} else if (completion.completion == CallbackState::ECompletion::DeadlineExceeded || IsDeadlineExceeded(deadline)) {
		outcome = Resolution(config::ESystemProxyResolutionOutcome::DeadlineExceeded);
	} else if (completion.completion == CallbackState::ECompletion::Failed) {
		outcome = MapAutoProxyFailure(completion.error, deadline, cancellation);
	} else if (completion.completion == CallbackState::ECompletion::Complete) {
		WinHttpSystemProxyResult result;
		struct FreeResult final { IWinHttpSystemProxyFacade& facade; WinHttpSystemProxyResult& result; ~FreeResult() { facade.FreeProxyResult(result); } } freeResult{ *m_facade, result };
		if (!m_facade->GetProxyResult(resolver, result)) {
			outcome = Resolution(config::ESystemProxyResolutionOutcome::InvalidResult);
		} else if (result.entries.size() != 1) {
			outcome = Resolution(config::ESystemProxyResolutionOutcome::InvalidResult);
		} else {
			const auto& entry = result.entries.front();
			if (!entry.isProxy) {
				// ProxyScheme is meaningful only when fProxy is true. WinHTTP's
				// public header does not define an "unknown" sentinel for DIRECT.
				outcome = (entry.host.empty() && entry.port == 0)
					? Direct(entry.bypassed)
					: Resolution(config::ESystemProxyResolutionOutcome::InvalidResult);
			} else if (entry.bypassed || entry.scheme != INTERNET_SCHEME_HTTP || entry.port == 0) {
				outcome = Resolution(config::ESystemProxyResolutionOutcome::InvalidResult);
			} else {
				std::wstring proxyUrl;
				const auto authority = entry.host.find(L':') == std::wstring::npos
					? entry.host + L":" + std::to_wstring(entry.port)
					: (entry.host.starts_with(L"[") ? entry.host : L"[" + entry.host + L"]") + L":" + std::to_wstring(entry.port);
				outcome = ParseHttpProxyAuthority(authority, proxyUrl) ? Manual(std::move(proxyUrl))
					: Resolution(config::ESystemProxyResolutionOutcome::InvalidResult);
			}
		}
	}
	if (!closeResolver()) return Resolution(config::ESystemProxyResolutionOutcome::InvalidResult);
	if (outcome.outcome == config::ESystemProxyResolutionOutcome::Unavailable) {
		return ResolveStaticProxy(staticProxyParse, std::move(staticProxy), config.proxyBypass, targetHost, targetPort);
	}
	return outcome;
}

} // namespace platform::request::win32
