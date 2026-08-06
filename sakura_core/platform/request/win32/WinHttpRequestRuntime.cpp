/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include <sakura/request/win32/WinHttpRequestRuntime.h>

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <limits>
#include <string_view>
#include <thread>
#include <utility>

namespace platform::request::win32 {
namespace {

constexpr DWORD kReadChunkBytes = 64 * 1024;

class InternetHandle final {
public:
	InternetHandle() = default;
	explicit InternetHandle(HINTERNET handle) noexcept : m_handle(handle) {}
	InternetHandle(const InternetHandle&) = delete;
	InternetHandle& operator=(const InternetHandle&) = delete;
	InternetHandle(InternetHandle&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
	InternetHandle& operator=(InternetHandle&& other) noexcept
	{
		if (this != &other) {
			Reset();
			m_handle = std::exchange(other.m_handle, nullptr);
		}
		return *this;
	}
	~InternetHandle() { Reset(); }

	HINTERNET Get() const noexcept { return m_handle; }
	explicit operator bool() const noexcept { return m_handle != nullptr; }

private:
	void Reset() noexcept
	{
		if (m_handle) {
			::WinHttpCloseHandle(m_handle);
			m_handle = nullptr;
		}
	}

	HINTERNET m_handle = nullptr;
};

class WipedWideString final {
public:
	WipedWideString() = default;
	WipedWideString(const WipedWideString&) = delete;
	WipedWideString& operator=(const WipedWideString&) = delete;
	~WipedWideString()
	{
		if (!m_value.empty()) {
			::SecureZeroMemory(m_value.data(), m_value.size() * sizeof(wchar_t));
		}
	}

	std::wstring& Value() noexcept { return m_value; }
	const wchar_t* CStr() const noexcept { return m_value.c_str(); }

private:
	std::wstring m_value;
};

bool IsCancelled(const IRequestCancellation* cancellation) noexcept
{
	return cancellation && cancellation->IsCancellationRequested();
}

bool HasTimedOut(const std::optional<std::chrono::steady_clock::time_point>& deadline) noexcept
{
	return deadline && std::chrono::steady_clock::now() >= *deadline;
}

TransportResult Failure(ETransportFailure failure) noexcept
{
	return { std::nullopt, failure };
}

ETransportFailure FailureForWinHttpError(DWORD error) noexcept
{
	switch (error) {
	case ERROR_WINHTTP_TIMEOUT:
		return ETransportFailure::Timeout;
	case ERROR_WINHTTP_SECURE_FAILURE:
	case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
		return ETransportFailure::TlsCertificateFailure;
	case ERROR_WINHTTP_LOGIN_FAILURE:
		return ETransportFailure::ProxyAuthenticationRequired;
	case ERROR_WINHTTP_INVALID_URL:
	case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
	case ERROR_WINHTTP_HEADER_NOT_FOUND:
	case ERROR_WINHTTP_HEADER_COUNT_EXCEEDED:
		return ETransportFailure::Protocol;
	default:
		return ETransportFailure::Network;
	}
}

bool IsHeaderNameValid(std::wstring_view name) noexcept
{
	if (name.empty()) return false;
	for (const auto character : name) {
		if ((character >= L'0' && character <= L'9') || (character >= L'A' && character <= L'Z') ||
			(character >= L'a' && character <= L'z')) continue;
		switch (character) {
		case L'!': case L'#': case L'$': case L'%': case L'&': case L'\'': case L'*': case L'+':
		case L'-': case L'.': case L'^': case L'_': case L'`': case L'|': case L'~': continue;
		default: return false;
		}
	}
	return true;
}

bool IsRequestShapeValid(const TransportRequest& request) noexcept
{
	if (request.method.empty() || request.url.empty() || request.url.find(L'\0') != std::wstring::npos || request.limits.maxResponseHeaderBytes == 0 ||
		request.limits.maxResponseBodyBytes == 0) return false;
	if (request.limits.timeout && *request.limits.timeout <= std::chrono::milliseconds::zero()) return false;
	if (!IsHeaderNameValid(request.method)) return false;
	for (const auto& header : request.headers) {
		if (!IsHeaderNameValid(header.name) || header.value.find_first_of(L"\r\n") != std::wstring::npos) return false;
	}
	return true;
}

bool IsManualProxyValid(std::wstring_view proxyUrl, std::wstring& proxyName)
{
	if (proxyUrl.empty() || proxyUrl.size() > std::numeric_limits<DWORD>::max() || proxyUrl.find(L'@') != std::wstring_view::npos ||
		proxyUrl.find_first_of(L"\r\n") != std::wstring_view::npos) return false;

	URL_COMPONENTS components{};
	components.dwStructSize = sizeof(components);
	components.dwSchemeLength = static_cast<DWORD>(-1);
	components.dwHostNameLength = static_cast<DWORD>(-1);
	components.dwUrlPathLength = static_cast<DWORD>(-1);
	components.dwExtraInfoLength = static_cast<DWORD>(-1);
	if (!::WinHttpCrackUrl(proxyUrl.data(), static_cast<DWORD>(proxyUrl.size()), 0, &components) ||
		components.nScheme != INTERNET_SCHEME_HTTP ||
		components.dwHostNameLength == 0 || components.nPort == 0 ||
		components.dwExtraInfoLength != 0 ||
		(components.dwUrlPathLength != 0 && !(components.dwUrlPathLength == 1 && components.lpszUrlPath[0] == L'/'))) {
		return false;
	}

	proxyName.assign(components.lpszHostName, components.dwHostNameLength);
	if (proxyName.find(L':') != std::wstring::npos && !proxyName.starts_with(L"[")) {
		proxyName.insert(proxyName.begin(), L'[');
		proxyName.push_back(L']');
	}
	proxyName += L":" + std::to_wstring(components.nPort);
	return true;
}

bool CrackTargetUrl(std::wstring_view url, std::wstring& host, INTERNET_PORT& port, std::wstring& objectName, bool& secure)
{
	if (url.size() > std::numeric_limits<DWORD>::max()) return false;
	URL_COMPONENTS components{};
	components.dwStructSize = sizeof(components);
	components.dwSchemeLength = static_cast<DWORD>(-1);
	components.dwHostNameLength = static_cast<DWORD>(-1);
	components.dwUrlPathLength = static_cast<DWORD>(-1);
	components.dwExtraInfoLength = static_cast<DWORD>(-1);
	if (!::WinHttpCrackUrl(url.data(), static_cast<DWORD>(url.size()), 0, &components) ||
		(components.nScheme != INTERNET_SCHEME_HTTP && components.nScheme != INTERNET_SCHEME_HTTPS) || components.dwHostNameLength == 0) {
		return false;
	}
	host.assign(components.lpszHostName, components.dwHostNameLength);
	port = components.nPort;
	secure = components.nScheme == INTERNET_SCHEME_HTTPS;
	if (components.dwUrlPathLength != 0) objectName.assign(components.lpszUrlPath, components.dwUrlPathLength);
	if (objectName.empty()) objectName = L"/";
	if (components.dwExtraInfoLength != 0) objectName.append(components.lpszExtraInfo, components.dwExtraInfoLength);
	return true;
}

std::optional<std::chrono::steady_clock::time_point> EffectiveDeadline(const TransportRequest& request)
{
	if (request.deadline || !request.limits.timeout) return request.deadline;
	const auto now = std::chrono::steady_clock::now();
	const auto maximum = std::chrono::steady_clock::time_point::max() - now;
	return *request.limits.timeout >= maximum ? std::chrono::steady_clock::time_point::max() : now + *request.limits.timeout;
}

int RemainingTimeoutMilliseconds(const std::optional<std::chrono::steady_clock::time_point>& deadline)
{
	// WinHTTP uses zero for an infinite phase timeout. This does not extend a
	// bounded request: callers with a deadline take the other branch below.
	if (!deadline) return 0;
	const auto duration = *deadline - std::chrono::steady_clock::now();
	if (duration <= std::chrono::steady_clock::duration::zero()) return 0;
	// WinHTTP interprets zero as an infinite timeout. Round a positive sub-ms
	// budget up so a bounded request can never accidentally become unbounded.
	const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(duration);
	return remaining.count() >= std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : static_cast<int>(remaining.count());
}

bool SetRemainingTimeouts(HINTERNET handle, const std::optional<std::chrono::steady_clock::time_point>& deadline)
{
	const int timeout = RemainingTimeoutMilliseconds(deadline);
	return (!deadline || timeout != 0) && ::WinHttpSetTimeouts(handle, timeout, timeout, timeout, timeout) != FALSE;
}

bool DisableAutomaticRedirects(HINTERNET request)
{
	DWORD disabledFeatures = WINHTTP_DISABLE_REDIRECTS;
	return ::WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disabledFeatures, sizeof(disabledFeatures)) != FALSE;
}

bool DisableAutomaticLogon(HINTERNET session)
{
	DWORD policy = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
	return ::WinHttpSetOption(session, WINHTTP_OPTION_AUTOLOGON_POLICY, &policy, sizeof(policy)) != FALSE;
}

bool DecodeCredentialSecret(const RequestCredential& credential, WipedWideString& decoded)
{
	if (credential.secret.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
		credential.userName.find(L'\0') != std::wstring::npos) return false;
	if (credential.secret.empty()) return true;
	const int count = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		reinterpret_cast<const char*>(credential.secret.data()), static_cast<int>(credential.secret.size()), nullptr, 0);
	if (count <= 0) return false;
	decoded.Value().resize(static_cast<std::size_t>(count));
	return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		reinterpret_cast<const char*>(credential.secret.data()), static_cast<int>(credential.secret.size()), decoded.Value().data(), count) == count;
}

bool ApplyCredential(HINTERNET request, const RequestCredential& credential, DWORD target, DWORD scheme)
{
	WipedWideString secret;
	if (!DecodeCredentialSecret(credential, secret)) return false;
	return ::WinHttpSetCredentials(request, target, scheme, credential.userName.c_str(), secret.CStr(), nullptr) != FALSE;
}

bool AppendHeader(std::wstring& allHeaders, const HttpHeader& header)
{
	constexpr std::size_t suffixLength = 4; // ": " + CRLF
	if (header.name.size() > std::numeric_limits<std::size_t>::max() - header.value.size() ||
		header.name.size() + header.value.size() > std::numeric_limits<std::size_t>::max() - suffixLength ||
		allHeaders.size() > std::numeric_limits<std::size_t>::max() - header.name.size() - header.value.size() - suffixLength) return false;
	allHeaders.append(header.name);
	allHeaders.append(L": ");
	allHeaders.append(header.value);
	allHeaders.append(L"\r\n");
	return true;
}

bool ReadResponseHeaders(HINTERNET request, std::size_t limit, std::vector<HttpHeader>& headers)
{
	DWORD bytes = 0;
	if (::WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
		nullptr, &bytes, WINHTTP_NO_HEADER_INDEX) || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;
	if (bytes == 0 || bytes % sizeof(wchar_t) != 0 || bytes > limit || bytes > std::numeric_limits<std::size_t>::max() - sizeof(wchar_t)) return false;
	std::vector<wchar_t> raw(bytes / sizeof(wchar_t) + 1, L'\0');
	if (!::WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
		raw.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) return false;

	std::wstring_view text(raw.data());
	const auto firstLineEnd = text.find(L"\r\n");
	if (firstLineEnd == std::wstring_view::npos) return false;
	text.remove_prefix(firstLineEnd + 2);
	while (!text.empty()) {
		const auto lineEnd = text.find(L"\r\n");
		const auto line = text.substr(0, lineEnd == std::wstring_view::npos ? text.size() : lineEnd);
		if (line.empty()) break;
		const auto colon = line.find(L':');
		if (colon == std::wstring_view::npos || !IsHeaderNameValid(line.substr(0, colon))) return false;
		auto value = line.substr(colon + 1);
		while (!value.empty() && (value.front() == L' ' || value.front() == L'\t')) value.remove_prefix(1);
		headers.push_back({ std::wstring(line.substr(0, colon)), std::wstring(value) });
		if (lineEnd == std::wstring_view::npos) break;
		text.remove_prefix(lineEnd + 2);
	}
	return true;
}

} // namespace

TransportResult WinHttpRequestTransport::Send(const TransportRequest& request, const IRequestCancellation* cancellation)
{
	try {
		if (IsCancelled(cancellation)) return Failure(ETransportFailure::Network);
		if (!IsRequestShapeValid(request)) return Failure(ETransportFailure::Protocol);
		const auto deadline = EffectiveDeadline(request);
		if (HasTimedOut(deadline)) return Failure(ETransportFailure::Timeout);
		if (request.proxy.outcome != EProxySelectionOutcome::Selected) return Failure(ETransportFailure::UnsupportedProxyPolicy);

		DWORD accessType = WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
		std::wstring manualProxyName;
		switch (request.proxy.mode) {
		case EProxyMode::Direct:
			if (request.proxy.proxyUrl) return Failure(ETransportFailure::UnsupportedProxyPolicy);
			accessType = WINHTTP_ACCESS_TYPE_NO_PROXY;
			break;
		case EProxyMode::System:
			if (request.proxy.proxyUrl) return Failure(ETransportFailure::UnsupportedProxyPolicy);
			accessType = request.proxy.bypassed ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
			break;
		case EProxyMode::Manual:
			if (!request.proxy.proxyUrl || request.proxy.bypassed || !IsManualProxyValid(*request.proxy.proxyUrl, manualProxyName)) {
				return Failure(ETransportFailure::UnsupportedProxyPolicy);
			}
			accessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
			break;
		default:
			return Failure(ETransportFailure::UnsupportedProxyPolicy);
		}

		std::wstring host;
		std::wstring objectName;
		INTERNET_PORT port = 0;
		bool secure = false;
		if (!CrackTargetUrl(request.url, host, port, objectName, secure)) return Failure(ETransportFailure::Protocol);
		if (IsCancelled(cancellation)) return Failure(ETransportFailure::Network);
		if (HasTimedOut(deadline)) return Failure(ETransportFailure::Timeout);

		InternetHandle session(::WinHttpOpen(L"Sakura Editor NEXT RequestService", accessType,
			accessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY ? manualProxyName.c_str() : WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS, 0));
		if (!session) return Failure(FailureForWinHttpError(::GetLastError()));
		if (!SetRemainingTimeouts(session.Get(), deadline)) return Failure(HasTimedOut(deadline) ? ETransportFailure::Timeout : ETransportFailure::Network);
		if (!DisableAutomaticLogon(session.Get())) return Failure(FailureForWinHttpError(::GetLastError()));
		if (IsCancelled(cancellation)) return Failure(ETransportFailure::Network);
		if (HasTimedOut(deadline)) return Failure(ETransportFailure::Timeout);

		InternetHandle connection(::WinHttpConnect(session.Get(), host.c_str(), port, 0));
		if (!connection) return Failure(FailureForWinHttpError(::GetLastError()));
		if (IsCancelled(cancellation)) return Failure(ETransportFailure::Network);
		if (HasTimedOut(deadline)) return Failure(ETransportFailure::Timeout);

		InternetHandle winHttpRequest(::WinHttpOpenRequest(connection.Get(), request.method.c_str(), objectName.c_str(), nullptr,
			WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
		if (!winHttpRequest) return Failure(FailureForWinHttpError(::GetLastError()));
		if (!SetRemainingTimeouts(winHttpRequest.Get(), deadline) || !DisableAutomaticRedirects(winHttpRequest.Get())) {
			return Failure(HasTimedOut(deadline) ? ETransportFailure::Timeout : FailureForWinHttpError(::GetLastError()));
		}

		std::wstring headers;
		for (const auto& header : request.headers) {
			if (!AppendHeader(headers, header)) return Failure(ETransportFailure::Protocol);
		}
		if (headers.size() > std::numeric_limits<DWORD>::max() || request.body.size() > std::numeric_limits<DWORD>::max()) return Failure(ETransportFailure::Protocol);
		if (IsCancelled(cancellation)) return Failure(ETransportFailure::Network);
		if (HasTimedOut(deadline)) return Failure(ETransportFailure::Timeout);
		if (!SetRemainingTimeouts(winHttpRequest.Get(), deadline)) return Failure(HasTimedOut(deadline) ? ETransportFailure::Timeout : ETransportFailure::Network);
		if (!::WinHttpSendRequest(winHttpRequest.Get(), headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.data(),
			static_cast<DWORD>(headers.size()), request.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<std::uint8_t*>(request.body.data()),
			static_cast<DWORD>(request.body.size()), static_cast<DWORD>(request.body.size()), 0)) return Failure(FailureForWinHttpError(::GetLastError()));
		if (IsCancelled(cancellation)) return Failure(ETransportFailure::Network);
		if (HasTimedOut(deadline)) return Failure(ETransportFailure::Timeout);
		if (!SetRemainingTimeouts(winHttpRequest.Get(), deadline)) return Failure(HasTimedOut(deadline) ? ETransportFailure::Timeout : ETransportFailure::Network);
		if (!::WinHttpReceiveResponse(winHttpRequest.Get(), nullptr)) return Failure(FailureForWinHttpError(::GetLastError()));
		if (IsCancelled(cancellation)) return Failure(ETransportFailure::Network);
		if (HasTimedOut(deadline)) return Failure(ETransportFailure::Timeout);

		HttpResponse response;
		DWORD statusCode = 0;
		DWORD statusCodeBytes = sizeof(statusCode);
		if (!::WinHttpQueryHeaders(winHttpRequest.Get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeBytes, WINHTTP_NO_HEADER_INDEX)) return Failure(FailureForWinHttpError(::GetLastError()));
		const std::optional<RequestCredential>* credential = nullptr;
		DWORD expectedTarget = 0;
		if (statusCode == 401) {
			credential = &request.serverCredential;
			expectedTarget = WINHTTP_AUTH_TARGET_SERVER;
		} else if (statusCode == 407) {
			credential = &request.proxyCredential;
			expectedTarget = WINHTTP_AUTH_TARGET_PROXY;
		}
		if (credential && *credential) {
			DWORD supportedSchemes = 0;
			DWORD selectedScheme = 0;
			DWORD selectedTarget = 0;
			if (!::WinHttpQueryAuthSchemes(winHttpRequest.Get(), &supportedSchemes, &selectedScheme, &selectedTarget)) return Failure(FailureForWinHttpError(::GetLastError()));
			if (selectedScheme == 0 || selectedTarget != expectedTarget || (supportedSchemes & selectedScheme) == 0) return Failure(ETransportFailure::Protocol);
			if (!ApplyCredential(winHttpRequest.Get(), **credential, selectedTarget, selectedScheme)) return Failure(FailureForWinHttpError(::GetLastError()));
			if (IsCancelled(cancellation)) return Failure(ETransportFailure::Network);
			if (HasTimedOut(deadline)) return Failure(ETransportFailure::Timeout);
			if (!SetRemainingTimeouts(winHttpRequest.Get(), deadline)) return Failure(HasTimedOut(deadline) ? ETransportFailure::Timeout : ETransportFailure::Network);
			if (!::WinHttpSendRequest(winHttpRequest.Get(), headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.data(),
				static_cast<DWORD>(headers.size()), request.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<std::uint8_t*>(request.body.data()),
				static_cast<DWORD>(request.body.size()), static_cast<DWORD>(request.body.size()), 0)) return Failure(FailureForWinHttpError(::GetLastError()));
			if (IsCancelled(cancellation)) return Failure(ETransportFailure::Network);
			if (HasTimedOut(deadline)) return Failure(ETransportFailure::Timeout);
			if (!SetRemainingTimeouts(winHttpRequest.Get(), deadline)) return Failure(HasTimedOut(deadline) ? ETransportFailure::Timeout : ETransportFailure::Network);
			if (!::WinHttpReceiveResponse(winHttpRequest.Get(), nullptr)) return Failure(FailureForWinHttpError(::GetLastError()));
			if (IsCancelled(cancellation)) return Failure(ETransportFailure::Network);
			if (HasTimedOut(deadline)) return Failure(ETransportFailure::Timeout);
			statusCodeBytes = sizeof(statusCode);
			if (!::WinHttpQueryHeaders(winHttpRequest.Get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeBytes, WINHTTP_NO_HEADER_INDEX)) return Failure(FailureForWinHttpError(::GetLastError()));
		}
		response.statusCode = static_cast<int>(statusCode);
		if (!ReadResponseHeaders(winHttpRequest.Get(), request.limits.maxResponseHeaderBytes, response.headers)) {
			DWORD rawBytes = 0;
			::WinHttpQueryHeaders(winHttpRequest.Get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &rawBytes, WINHTTP_NO_HEADER_INDEX);
			return Failure(rawBytes > request.limits.maxResponseHeaderBytes ? ETransportFailure::ResponseHeaderLimitExceeded : ETransportFailure::Protocol);
		}

		// bodySink が設定され、かつ成功応答（2xx）のときだけ本体をメモリーに溜め込まず
		// chunk ごとに sink へ流す。3xx/401/407/429/503 等の制御用応答は呼び出し元
		// （RequestService）がヘッダーだけで判断するため、これらは従来どおり
		// response.body に溜めて返す（sink は無視する）。streamedBytes は sink 経路専用の
		// 「これまでに消費したバイト数」で、response.body.size() の代わりに上限判定へ使う。
		const bool streamToSink = static_cast<bool>(request.bodySink) && response.statusCode >= 200 && response.statusCode < 300;
		std::vector<std::uint8_t> chunkBuffer;
		if (streamToSink) chunkBuffer.resize(kReadChunkBytes);
		std::size_t streamedBytes = 0;

		for (;;) {
			if (IsCancelled(cancellation)) return Failure(ETransportFailure::Network);
			if (HasTimedOut(deadline)) return Failure(ETransportFailure::Timeout);
			if (!SetRemainingTimeouts(winHttpRequest.Get(), deadline)) return Failure(HasTimedOut(deadline) ? ETransportFailure::Timeout : ETransportFailure::Network);
			DWORD available = 0;
			if (!::WinHttpQueryDataAvailable(winHttpRequest.Get(), &available)) return Failure(FailureForWinHttpError(::GetLastError()));
			if (available == 0) break;
			const std::size_t consumedSoFar = streamToSink ? streamedBytes : response.body.size();
			if (static_cast<std::size_t>(available) > request.limits.maxResponseBodyBytes - consumedSoFar) {
				return Failure(ETransportFailure::ResponseBodyLimitExceeded);
			}
			DWORD remaining = available;
			while (remaining != 0) {
				if (IsCancelled(cancellation)) return Failure(ETransportFailure::Network);
				if (HasTimedOut(deadline)) return Failure(ETransportFailure::Timeout);
				if (!SetRemainingTimeouts(winHttpRequest.Get(), deadline)) return Failure(HasTimedOut(deadline) ? ETransportFailure::Timeout : ETransportFailure::Network);
				const DWORD toRead = std::min(remaining, kReadChunkBytes);
				if (streamToSink) {
					const auto previousStreamed = streamedBytes;
					if (toRead > request.limits.maxResponseBodyBytes - previousStreamed) return Failure(ETransportFailure::ResponseBodyLimitExceeded);
					DWORD read = 0;
					if (!::WinHttpReadData(winHttpRequest.Get(), chunkBuffer.data(), toRead, &read)) return Failure(FailureForWinHttpError(::GetLastError()));
					if (read == 0) return Failure(ETransportFailure::Protocol);
					if (!request.bodySink(chunkBuffer.data(), static_cast<std::size_t>(read))) return Failure(ETransportFailure::SinkFailure);
					streamedBytes = previousStreamed + read;
					remaining -= read;
				} else {
					const auto previousSize = response.body.size();
					if (toRead > request.limits.maxResponseBodyBytes - previousSize) return Failure(ETransportFailure::ResponseBodyLimitExceeded);
					response.body.resize(previousSize + toRead);
					DWORD read = 0;
					if (!::WinHttpReadData(winHttpRequest.Get(), response.body.data() + previousSize, toRead, &read)) return Failure(FailureForWinHttpError(::GetLastError()));
					response.body.resize(previousSize + read);
					if (read == 0) return Failure(ETransportFailure::Protocol);
					remaining -= read;
				}
			}
		}
		response.finalUrl = request.url;
		return { std::move(response), ETransportFailure::None };
	} catch (...) {
		return Failure(ETransportFailure::Network);
	}
}

std::chrono::system_clock::time_point Win32RequestClock::Now() const
{
	return std::chrono::system_clock::now();
}

std::chrono::steady_clock::time_point Win32RequestClock::SteadyNow() const
{
	return std::chrono::steady_clock::now();
}

Win32RequestScheduler::Win32RequestScheduler(std::chrono::milliseconds maximumPollInterval) :
	m_maximumPollInterval(std::max(maximumPollInterval, std::chrono::milliseconds(1)))
{
}

bool Win32RequestScheduler::WaitFor(std::chrono::milliseconds delay, const IRequestCancellation* cancellation)
{
	if (IsCancelled(cancellation)) return false;
	if (delay <= std::chrono::milliseconds::zero()) return true;
	const auto now = std::chrono::steady_clock::now();
	const auto maximum = std::chrono::steady_clock::time_point::max() - now;
	const auto target = delay >= maximum ? std::chrono::steady_clock::time_point::max() : now + delay;
	for (;;) {
		if (IsCancelled(cancellation)) return false;
		const auto current = std::chrono::steady_clock::now();
		if (current >= target) return !IsCancelled(cancellation);
		const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(target - current);
		std::this_thread::sleep_for(std::min(m_maximumPollInterval, std::max(remaining, std::chrono::milliseconds(1))));
	}
}

ThreadSafeRetryJitterSource::ThreadSafeRetryJitterSource() :
	ThreadSafeRetryJitterSource((static_cast<std::uint64_t>(std::random_device{}()) << 32) ^ std::random_device{}())
{
}

ThreadSafeRetryJitterSource::ThreadSafeRetryJitterSource(std::uint64_t seed) :
	m_generator(seed)
{
}

double ThreadSafeRetryJitterSource::NextUnitInterval()
{
	std::lock_guard lock(m_mutex);
	return std::generate_canonical<double, std::numeric_limits<double>::digits>(m_generator);
}

} // namespace platform::request::win32
