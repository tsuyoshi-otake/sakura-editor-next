/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "extension/CExtensionHostProcess.h"
#include "extension/CExtensionPipeTransport.h"
#include "extension/CExtensionRpcProtocol.h"
#include "extension/CExtensionSecretStorage.h"
#include "extension/CExtensionWorkbenchDispatcher.h"

#include <picojson/picojson.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "util/string_ex.h"

namespace {

using namespace std::chrono_literals;

std::filesystem::path FindRepositoryRoot()
{
	std::array<wchar_t, 32768> modulePath{};
	const DWORD length = ::GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
	std::vector<std::filesystem::path> candidates = { std::filesystem::current_path() };
	if (length > 0 && length < modulePath.size()) {
		candidates.emplace_back(std::filesystem::path(std::wstring_view(modulePath.data(), length)).parent_path());
	}
	for (auto candidate : candidates) {
		for (int parent = 0; parent < 8 && !candidate.empty(); ++parent) {
			if (std::filesystem::is_regular_file(candidate / L"src/exthost/src/extension-host.cjs")) {
				return candidate;
			}
			const auto next = candidate.parent_path();
			if (next == candidate) {
				break;
			}
			candidate = next;
		}
	}
	return {};
}

std::filesystem::path FindNodeExecutable()
{
	std::array<wchar_t, 32768> path{};
	const DWORD length = ::SearchPathW(
		nullptr, L"node.exe", nullptr, static_cast<DWORD>(path.size()), path.data(), nullptr);
	if (length == 0 || length >= path.size()) {
		return {};
	}
	return std::filesystem::path(std::wstring_view(path.data(), length));
}

std::filesystem::path FindSecurityShim(const std::filesystem::path& repositoryRoot)
{
	std::array<wchar_t, 32768> modulePath{};
	const DWORD length = ::GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
	if (length > 0 && length < modulePath.size()) {
		const auto builtShim = std::filesystem::path(std::wstring_view(modulePath.data(), length))
			.parent_path() / L"exthost/sakura_exthost_security.node";
		if (std::filesystem::is_regular_file(builtShim)) {
			return builtShim;
		}
	}
	const auto developmentShim = repositoryRoot / L"src/exthost/dist/sakura_exthost_security.node";
	return std::filesystem::is_regular_file(developmentShim) ? developmentShim : std::filesystem::path{};
}

class QueuedPipeSink final : public IExtensionPipeTransportSink {
public:
	void OnExtensionPipeBytes(std::vector<std::uint8_t> bytes) noexcept override
	{
		std::lock_guard lock(m_mutex);
		m_chunks.emplace_back(std::move(bytes));
		m_condition.notify_all();
	}

	void OnExtensionPipeClosed(std::uint32_t errorCode, std::wstring diagnostic) noexcept override
	{
		std::lock_guard lock(m_mutex);
		m_closed = true;
		m_closeError = errorCode;
		m_closeDiagnostic = std::move(diagnostic);
		m_condition.notify_all();
	}

	std::optional<std::vector<std::uint8_t>> WaitForChunk(std::chrono::milliseconds timeout)
	{
		std::unique_lock lock(m_mutex);
		if (!m_condition.wait_for(lock, timeout, [&] { return !m_chunks.empty() || m_closed; })) {
			return std::nullopt;
		}
		if (m_chunks.empty()) {
			return std::nullopt;
		}
		auto result = std::move(m_chunks.front());
		m_chunks.pop_front();
		return result;
	}

private:
	std::mutex m_mutex;
	std::condition_variable m_condition;
	std::deque<std::vector<std::uint8_t>> m_chunks;
	bool m_closed = false;
	std::uint32_t m_closeError = 0;
	std::wstring m_closeDiagnostic;
};

bool SendFrame(CExtensionPipeTransport& transport, const std::string& frame)
{
	const auto* bytes = reinterpret_cast<const std::uint8_t*>(frame.data());
	return transport.Send(std::span(bytes, frame.size()), 2s).success;
}

std::string NarrowForDiagnostic(std::wstring_view value)
{
	std::string result;
	result.reserve(value.size());
	for (const wchar_t character : value) {
		result.push_back(character >= 0x20 && character <= 0x7e ? static_cast<char>(character) : '?');
	}
	return result;
}

class RpcPump final {
public:
	using IncomingHandler = std::function<bool(const SExtensionRpcMessage&)>;

	RpcPump(QueuedPipeSink& sink, CExtensionPipeTransport& transport)
		: m_sink(sink)
		, m_transport(transport)
	{
	}

	void SetIncomingHandler(IncomingHandler handler)
	{
		m_incomingHandler = std::move(handler);
	}

	bool WaitFor(
		const std::function<bool(const SExtensionRpcMessage&)>& predicate,
		SExtensionRpcMessage& result,
		std::chrono::milliseconds timeout = 5s)
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		for (;;) {
			for (auto it = m_messages.begin(); it != m_messages.end(); ++it) {
				if (predicate(*it)) {
					result = std::move(*it);
					m_messages.erase(it);
					return true;
				}
			}

			const auto now = std::chrono::steady_clock::now();
			if (now >= deadline) {
				return false;
			}
			const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
			const auto chunk = m_sink.WaitForChunk(remaining);
			if (!chunk) {
				return false;
			}

			const auto* characters = reinterpret_cast<const char*>(chunk->data());
			auto received = m_protocol.Feed(std::string_view(characters, chunk->size()));
			if (received.IsTerminal()) {
				return false;
			}
			for (auto& message : received.messages) {
				if (message.eKind == EExtensionRpcMessageKind::Request && message.sMethod == "client/echo") {
					SExtensionRpcOutbound response;
					std::string error;
					const std::string resultJson = message.sParamsJson.empty() ? "null" : message.sParamsJson;
					if (!m_protocol.CreateSuccessResponse(message.sIdJson, resultJson, response, error) ||
						!SendFrame(m_transport, response.frame)) {
						return false;
					}
					continue;
				}
				if (m_incomingHandler && m_incomingHandler(message)) {
					continue;
				}
				m_messages.emplace_back(std::move(message));
			}
		}
	}

	bool Request(std::string_view method, std::string_view paramsJson, SExtensionRpcOutbound& request)
	{
		std::string error;
		return m_protocol.CreateRequest(method, paramsJson, request, error) &&
			SendFrame(m_transport, request.frame);
	}

	bool Respond(
		const SExtensionRpcMessage& request,
		const SExtensionWorkbenchDispatchResult& result)
	{
		if (request.eKind != EExtensionRpcMessageKind::Request || request.sIdJson.empty()) {
			return false;
		}
		SExtensionRpcOutbound response;
		std::string error;
		const bool encoded = result.success
			? m_protocol.CreateSuccessResponse(request.sIdJson, result.resultJson, response, error)
			: m_protocol.CreateErrorResponse(
				request.sIdJson, result.errorCode, result.errorMessage, {}, response, error);
		return encoded && SendFrame(m_transport, response.frame);
	}

private:
	QueuedPipeSink& m_sink;
	CExtensionPipeTransport& m_transport;
	CExtensionRpcProtocol m_protocol;
	std::deque<SExtensionRpcMessage> m_messages;
	IncomingHandler m_incomingHandler;
};

class ScopedTemporaryDirectory final {
public:
	ScopedTemporaryDirectory()
	{
		static std::atomic_uint64_t sequence = 0;
		m_path = std::filesystem::temp_directory_path() /
			(L"sakura-extension-integration-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
				std::to_wstring(::GetTickCount64()) + L"-" + std::to_wstring(++sequence));
		std::filesystem::create_directories(m_path);
	}

	~ScopedTemporaryDirectory()
	{
		std::error_code error;
		std::filesystem::remove_all(m_path, error);
	}

	const std::filesystem::path& Get() const noexcept { return m_path; }

private:
	std::filesystem::path m_path;
};

const picojson::object* ParseObject(const std::string& json, picojson::value& storage)
{
	if (!picojson::parse(storage, json).empty() || !storage.is<picojson::object>()) {
		return nullptr;
	}
	return &storage.get<picojson::object>();
}

} // namespace

TEST(CExtensionHostIntegration, LaunchesSecureNodeHostAndPerformsFullDuplexRpc)
{
	const auto repositoryRoot = FindRepositoryRoot();
	if (repositoryRoot.empty()) {
		GTEST_SKIP() << "repository root not found";
	}
	const auto nodeExecutable = FindNodeExecutable();
	if (nodeExecutable.empty()) {
		GTEST_SKIP() << "node.exe not found on PATH";
	}
	const auto securityShim = FindSecurityShim(repositoryRoot);
	if (securityShim.empty()) {
		GTEST_SKIP() << "extension host security shim not built";
	}

	const std::wstring profileHash(32, L'a');
	const std::wstring bootId(32, L'b');
	const std::wstring pipeName = L"\\\\.\\pipe\\sakura-exthost-" + profileHash + L"-" + bootId;
	CExtensionHostProcess process;
	SExtensionHostLaunchOptions options;
	options.nodeExecutable = nodeExecutable;
	options.hostBundle = repositoryRoot / L"src/exthost/src/extension-host.cjs";
	options.securityShim = securityShim;
	options.workingDirectory = repositoryRoot / L"src/exthost";
	options.profileHash = profileHash;
	options.bootId = bootId;
	options.pipeName = pipeName;
	options.generation = 7;
	options.brokerProcessId = ::GetCurrentProcessId();

	const auto started = process.Start(options);
	ASSERT_TRUE(started.success) << started.errorCode;
	ASSERT_NE(0u, started.processId);

	QueuedPipeSink sink;
	CExtensionPipeTransport transport(sink);
	const auto connected = transport.Connect(pipeName, started.processId, 5s);
	ASSERT_TRUE(connected.success) << connected.errorCode << " "
		<< NarrowForDiagnostic(connected.diagnostic);
	EXPECT_EQ(started.processId, connected.serverProcessId);

	RpcPump pump(sink, transport);
	SExtensionRpcMessage hello;
	ASSERT_TRUE(pump.WaitFor(
		[](const auto& message) {
			return message.eKind == EExtensionRpcMessageKind::Notification && message.sMethod == "host/hello";
		},
		hello));
	picojson::value helloJson;
	const auto* helloObject = ParseObject(hello.sParamsJson, helloJson);
	ASSERT_NE(nullptr, helloObject);
	EXPECT_EQ(7.0, helloObject->at("generation").get<double>());
	EXPECT_EQ(static_cast<double>(started.processId), helloObject->at("processId").get<double>());
	EXPECT_EQ(std::string(32, 'b'), helloObject->at("bootId").get<std::string>());

	SExtensionRpcOutbound pingRequest;
	ASSERT_TRUE(pump.Request("host/ping", R"({"nonce":"native-roundtrip"})", pingRequest));
	SExtensionRpcMessage ping;
	ASSERT_TRUE(pump.WaitFor(
		[&](const auto& message) {
			return message.eKind == EExtensionRpcMessageKind::SuccessResponse &&
				message.sIdJson == pingRequest.sIdJson;
		},
		ping));
	picojson::value pingJson;
	const auto* pingObject = ParseObject(ping.sResultJson, pingJson);
	ASSERT_NE(nullptr, pingObject);
	EXPECT_EQ("native-roundtrip", pingObject->at("nonce").get<std::string>());

	SExtensionRpcOutbound duplexRequest;
	ASSERT_TRUE(pump.Request("host/requestClientEcho", R"({"value":"from-native"})", duplexRequest));
	SExtensionRpcMessage duplex;
	ASSERT_TRUE(pump.WaitFor(
		[&](const auto& message) {
			return message.eKind == EExtensionRpcMessageKind::SuccessResponse &&
				message.sIdJson == duplexRequest.sIdJson;
		},
		duplex));
	picojson::value duplexJson;
	const auto* duplexObject = ParseObject(duplex.sResultJson, duplexJson);
	ASSERT_NE(nullptr, duplexObject);
	EXPECT_EQ("from-native", duplexObject->at("value").get<std::string>());

	SExtensionRpcOutbound quiesceRequest;
	ASSERT_TRUE(pump.Request("host/quiesce", R"({"generation":7})", quiesceRequest));
	SExtensionRpcMessage quiesce;
	ASSERT_TRUE(pump.WaitFor(
		[&](const auto& message) {
			return message.eKind == EExtensionRpcMessageKind::SuccessResponse &&
				message.sIdJson == quiesceRequest.sIdJson;
		},
		quiesce));
	EXPECT_TRUE(process.WaitForExit(2s));
	transport.Close();
	process.Terminate(ERROR_SUCCESS);
}

TEST(CExtensionHostIntegration, BridgesVsCodeStatusBarAndTreeViewIntoNativeWorkbenchModels)
{
	const auto repositoryRoot = FindRepositoryRoot();
	if (repositoryRoot.empty()) {
		GTEST_SKIP() << "repository root not found";
	}
	const auto nodeExecutable = FindNodeExecutable();
	if (nodeExecutable.empty()) {
		GTEST_SKIP() << "node.exe not found on PATH";
	}
	const auto securityShim = FindSecurityShim(repositoryRoot);
	if (securityShim.empty()) {
		GTEST_SKIP() << "extension host security shim not built";
	}
	const auto extensionRoot = repositoryRoot / L"src/exthost/test/fixtures/native-workbench";
	ASSERT_TRUE(std::filesystem::is_regular_file(extensionRoot / L"package.json"));
	ASSERT_TRUE(std::filesystem::is_regular_file(extensionRoot / L"extension.js"));

	const std::wstring profileHash(32, L'c');
	const std::wstring bootId(32, L'd');
	const std::wstring pipeName = L"\\\\.\\pipe\\sakura-exthost-" + profileHash + L"-" + bootId;
	CExtensionHostProcess process;
	SExtensionHostLaunchOptions options;
	options.nodeExecutable = nodeExecutable;
	options.hostBundle = repositoryRoot / L"src/exthost/src/extension-host.cjs";
	options.securityShim = securityShim;
	options.workingDirectory = repositoryRoot / L"src/exthost";
	options.profileHash = profileHash;
	options.bootId = bootId;
	options.pipeName = pipeName;
	options.generation = 8;
	options.brokerProcessId = ::GetCurrentProcessId();

	const auto started = process.Start(options);
	ASSERT_TRUE(started.success) << started.errorCode;

	QueuedPipeSink sink;
	CExtensionPipeTransport transport(sink);
	const auto connected = transport.Connect(pipeName, started.processId, 5s);
	ASSERT_TRUE(connected.success) << connected.errorCode << " "
		<< NarrowForDiagnostic(connected.diagnostic);

	RpcPump pump(sink, transport);
	SExtensionRpcMessage hello;
	ASSERT_TRUE(pump.WaitFor(
		[](const auto& message) {
			return message.eKind == EExtensionRpcMessageKind::Notification && message.sMethod == "host/hello";
		},
		hello));

	ScopedTemporaryDirectory temporaryDirectory;
	CExtensionContextKeys contextKeys;
	CExtensionCommandPalette commands;
	CExtensionStatusBar statusBar;
	CExtensionNotificationCenter notifications;
	CExtensionViewRegistry views;
	CExtensionSecretStorage secrets(temporaryDirectory.Get() / L"secrets");
	CExtensionDiagnostics diagnostics;
	CExtensionQuickInput quickInput;
	CExtensionOutputChannel output;
	CExtensionProgressCenter progress;
	CExtensionWorkbenchDispatcher dispatcher(
		contextKeys, commands, statusBar, notifications, views, secrets,
		diagnostics, quickInput, output, progress);
	std::vector<std::string> dispatchFailures;
	pump.SetIncomingHandler([&](const SExtensionRpcMessage& message) {
		const auto result = dispatcher.Dispatch(message);
		if (!result.handled) return false;
		if (!result.success) dispatchFailures.push_back(result.errorMessage);
		if (message.eKind == EExtensionRpcMessageKind::Request && !pump.Respond(message, result)) {
			dispatchFailures.emplace_back("failed to send native workbench response");
		}
		return true;
	});

	picojson::array extensionPaths;
	extensionPaths.emplace_back(picojson::value(wcstou8s(extensionRoot.wstring())));
	picojson::object registrationParams;
	registrationParams["extensions"] = picojson::value(std::move(extensionPaths));
	SExtensionRpcOutbound registrationRequest;
	ASSERT_TRUE(pump.Request(
		"host/registerExtensions", picojson::value(std::move(registrationParams)).serialize(), registrationRequest));
	SExtensionRpcMessage registration;
	ASSERT_TRUE(pump.WaitFor(
		[&](const auto& message) {
			return message.eKind == EExtensionRpcMessageKind::SuccessResponse &&
				message.sIdJson == registrationRequest.sIdJson;
		},
		registration));
	picojson::value registrationJson;
	const auto* registrationObject = ParseObject(registration.sResultJson, registrationJson);
	ASSERT_NE(nullptr, registrationObject);
	ASSERT_TRUE(registrationObject->at("failed").is<picojson::array>());
	EXPECT_TRUE(registrationObject->at("failed").get<picojson::array>().empty());
	EXPECT_TRUE(commands.Contains(L"sakuraTest.native.run"));

	SExtensionRpcOutbound activationRequest;
	ASSERT_TRUE(pump.Request(
		"host/activateByEvent", R"({"event":"onStartupFinished"})", activationRequest));
	SExtensionRpcMessage activation;
	ASSERT_TRUE(pump.WaitFor(
		[&](const auto& message) {
			return (message.eKind == EExtensionRpcMessageKind::SuccessResponse ||
				message.eKind == EExtensionRpcMessageKind::ErrorResponse) &&
				message.sIdJson == activationRequest.sIdJson;
		},
		activation));
	ASSERT_EQ(EExtensionRpcMessageKind::SuccessResponse, activation.eKind)
		<< activation.error.nCode << " " << activation.error.sMessage;
	EXPECT_TRUE(dispatchFailures.empty()) << (dispatchFailures.empty() ? "" : dispatchFailures.front());

	const auto statusItems = statusBar.Snapshot();
	ASSERT_EQ(1u, statusItems.size());
	EXPECT_EQ(L"sakuraTest.native.status", statusItems[0].itemId);
	EXPECT_EQ(L"$(extensions) VSX Ready", statusItems[0].text);
	EXPECT_EQ(L"Native workbench bridge is connected", statusItems[0].tooltip);
	EXPECT_EQ(L"sakuraTest.native.run", statusItems[0].command);
	EXPECT_TRUE(statusItems[0].visible);

	const auto registeredViews = views.Views();
	ASSERT_EQ(1u, registeredViews.size());
	EXPECT_EQ(L"sakuraTest.native.view", registeredViews[0].viewId);
	EXPECT_EQ(L"Open VSX Test View", registeredViews[0].title);
	EXPECT_EQ(L"Connected", registeredViews[0].description);

	picojson::object childrenParams;
	childrenParams["handle"] = picojson::value(wcstou8s(registeredViews[0].handle));
	SExtensionRpcOutbound childrenRequest;
	ASSERT_TRUE(pump.Request(
		"extension/views/getChildren", picojson::value(std::move(childrenParams)).serialize(), childrenRequest));
	SExtensionRpcMessage children;
	ASSERT_TRUE(pump.WaitFor(
		[&](const auto& message) {
			return message.eKind == EExtensionRpcMessageKind::SuccessResponse &&
				message.sIdJson == childrenRequest.sIdJson;
		},
		children));
	const auto applied = dispatcher.ApplyTreeChildrenResult(
		registeredViews[0].handle, {}, L"sakura-test.native-workbench", 8, children.sResultJson);
	ASSERT_TRUE(applied.success) << applied.errorMessage;
	const auto rootItems = views.Children(registeredViews[0].handle);
	ASSERT_EQ(1u, rootItems.size());
	EXPECT_EQ(L"Open VSX tree item", rootItems[0].label);
	EXPECT_EQ(L"Native sidebar", rootItems[0].description);
	EXPECT_EQ(L"sakuraTest.native.run", rootItems[0].command);

	SExtensionRpcOutbound commandRequest;
	ASSERT_TRUE(pump.Request(
		"extension/commands/execute", R"({"command":"sakuraTest.native.run","args":[]})", commandRequest));
	SExtensionRpcMessage commandResult;
	ASSERT_TRUE(pump.WaitFor(
		[&](const auto& message) {
			return message.eKind == EExtensionRpcMessageKind::SuccessResponse &&
				message.sIdJson == commandRequest.sIdJson;
		},
		commandResult));
	picojson::value commandJson;
	const auto* commandObject = ParseObject(commandResult.sResultJson, commandJson);
	ASSERT_NE(nullptr, commandObject);
	EXPECT_EQ("native-command-ok", commandObject->at("value").get<std::string>());

	constexpr std::string_view documentId = "4242:1";
	picojson::object snapshot;
	snapshot["documentId"] = picojson::value(std::string(documentId));
	snapshot["uri"] = picojson::value("file:///C:/sakura-native-workbench.txt");
	snapshot["fileName"] = picojson::value("C:\\sakura-native-workbench.txt");
	snapshot["languageId"] = picojson::value("plaintext");
	snapshot["version"] = picojson::value(1.0);
	snapshot["isDirty"] = picojson::value(false);
	snapshot["isUntitled"] = picojson::value(false);
	snapshot["text"] = picojson::value("native formatter");
	snapshot["eol"] = picojson::value(1.0);
	snapshot["encoding"] = picojson::value("utf8");
	picojson::object openParams;
	openParams["snapshot"] = picojson::value(std::move(snapshot));
	SExtensionRpcOutbound openRequest;
	ASSERT_TRUE(pump.Request(
		"extension/workspace/didOpen", picojson::value(std::move(openParams)).serialize(), openRequest));
	SExtensionRpcMessage openResult;
	ASSERT_TRUE(pump.WaitFor(
		[&](const auto& message) {
			return message.eKind == EExtensionRpcMessageKind::SuccessResponse &&
				message.sIdJson == openRequest.sIdJson;
		},
		openResult));

	picojson::object formatParams;
	formatParams["kind"] = picojson::value("formatDocument");
	formatParams["documentId"] = picojson::value(std::string(documentId));
	picojson::object formatOptions;
	formatOptions["insertSpaces"] = picojson::value(true);
	formatOptions["tabSize"] = picojson::value(4.0);
	formatParams["options"] = picojson::value(std::move(formatOptions));
	SExtensionRpcOutbound formatRequest;
	ASSERT_TRUE(pump.Request(
		"extension/languages/provide", picojson::value(std::move(formatParams)).serialize(), formatRequest));
	SExtensionRpcMessage formatResult;
	ASSERT_TRUE(pump.WaitFor(
		[&](const auto& message) {
			return message.eKind == EExtensionRpcMessageKind::SuccessResponse &&
				message.sIdJson == formatRequest.sIdJson;
		},
		formatResult));
	picojson::value formatJson;
	const auto* formatObject = ParseObject(formatResult.sResultJson, formatJson);
	ASSERT_NE(nullptr, formatObject);
	ASSERT_TRUE(formatObject->at("value").is<picojson::array>());
	const auto& formatEdits = formatObject->at("value").get<picojson::array>();
	ASSERT_EQ(1u, formatEdits.size());
	ASSERT_TRUE(formatEdits[0].is<picojson::object>());
	EXPECT_EQ("NATIVE FORMATTER", formatEdits[0].get<picojson::object>().at("newText").get<std::string>());
	EXPECT_EQ(1.0, formatObject->at("expectedVersion").get<double>());

	SExtensionRpcOutbound quiesceRequest;
	ASSERT_TRUE(pump.Request("host/quiesce", R"({"generation":8})", quiesceRequest));
	SExtensionRpcMessage quiesce;
	ASSERT_TRUE(pump.WaitFor(
		[&](const auto& message) {
			return message.eKind == EExtensionRpcMessageKind::SuccessResponse &&
				message.sIdJson == quiesceRequest.sIdJson;
		},
		quiesce));
	EXPECT_TRUE(process.WaitForExit(2s));
	transport.Close();
	process.Terminate(ERROR_SUCCESS);
}
