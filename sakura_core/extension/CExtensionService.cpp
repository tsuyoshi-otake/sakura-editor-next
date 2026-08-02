/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionService.h"
#include "extension/CExtensionWorkbenchServiceBridge.h"

#include "config/system_constants.h"
#include "extension/CExtensionManager.h"
#include "extension/CExtensionQuickInputDialog.h"
#include "workbench/IWorkbenchRuntime.h"
#include "_os/CClipboard.h"
#include "util/string_ex.h"

#include <CommCtrl.h>
#include <picojson/picojson.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <span>
#include <utility>

namespace {

using namespace std::chrono_literals;

constexpr std::size_t kMaximumQueuedTasks = 2048;
constexpr std::size_t kMaximumPendingRequests = 512;
constexpr std::size_t kMaximumExtensionClipboardTextCodeUnits = 8u * 1024u * 1024u;
constexpr auto kExtensionPipeConnectTimeout = 500ms;
constexpr std::wstring_view kFormatDocumentCommand = L"editor.action.formatDocument";

class CUnavailableExtensionSecretStorage final : public IExtensionSecretSessionStorage {
public:
	SExtensionSecretStorageResult Store(
		std::wstring_view, std::wstring_view, std::wstring_view) override
	{
		return Unsupported();
	}
	SExtensionSecretReadResult Get(std::wstring_view, std::wstring_view) override
	{
		SExtensionSecretReadResult result;
		static_cast<SExtensionSecretStorageResult&>(result) = Unsupported();
		return result;
	}
	SExtensionSecretStorageResult Delete(std::wstring_view, std::wstring_view) override
	{
		return Unsupported();
	}
	SExtensionSecretStorageResult BindSession(std::string_view, std::uint64_t) override
	{
		return { true, EExtensionSecretStorageStatus::Success, ERROR_SUCCESS, {} };
	}
	SExtensionSecretStorageResult ClearSession() noexcept override
	{
		return { true, EExtensionSecretStorageStatus::Success, ERROR_SUCCESS, {} };
	}
	void Stop() noexcept override {}

private:
	static SExtensionSecretStorageResult Unsupported()
	{
		return { false, EExtensionSecretStorageStatus::Unsupported, ERROR_NOT_SUPPORTED,
			L"Secret Vault backend is not configured" };
	}
};

const picojson::value* Find(const picojson::object& object, const char* key)
{
	const auto found = object.find(key);
	return found == object.end() ? nullptr : &found->second;
}

bool ParseObject(std::string_view json, picojson::object& object)
{
	picojson::value value;
	const auto error = picojson::parse(value, std::string(json));
	if (!error.empty() || !value.is<picojson::object>()) return false;
	object = value.get<picojson::object>();
	return true;
}

bool JsonUInt64(const picojson::object& object, const char* key, std::uint64_t& result)
{
	const auto* value = Find(object, key);
	if (!value || !value->is<double>()) return false;
	const double number = value->get<double>();
	if (!std::isfinite(number) || std::floor(number) != number || number <= 0 ||
		number > static_cast<double>((std::numeric_limits<std::uint64_t>::max)())) return false;
	result = static_cast<std::uint64_t>(number);
	return true;
}

bool JsonUInt32(const picojson::object& object, const char* key, std::uint32_t& result)
{
	std::uint64_t parsed = 0;
	const auto* value = Find(object, key);
	if (!value || !value->is<double>()) return false;
	const double number = value->get<double>();
	if (!std::isfinite(number) || std::floor(number) != number || number < 0 ||
		number > static_cast<double>((std::numeric_limits<std::uint32_t>::max)())) return false;
	parsed = static_cast<std::uint64_t>(number);
	result = static_cast<std::uint32_t>(parsed);
	return true;
}

std::optional<SExtensionDocumentId> ParseDocumentId(std::string_view value)
{
	const auto separator = value.find(':');
	if (separator == std::string_view::npos || separator == 0 || separator + 1 >= value.size()) return std::nullopt;
	SExtensionDocumentId id;
	const auto first = std::from_chars(value.data(), value.data() + separator, id.editorProcessId);
	const auto second = std::from_chars(value.data() + separator + 1, value.data() + value.size(), id.localDocumentId);
	if (first.ec != std::errc{} || first.ptr != value.data() + separator ||
		second.ec != std::errc{} || second.ptr != value.data() + value.size() || !id.IsValid()) return std::nullopt;
	return id;
}

const char* ApplyEditReason(EExtensionApplyEditStatus status) noexcept
{
	switch (status) {
	case EExtensionApplyEditStatus::Applied: return "Applied";
	case EExtensionApplyEditStatus::UnknownDocument: return "UnknownDocument";
	case EExtensionApplyEditStatus::VersionMismatch: return "VersionMismatch";
	case EExtensionApplyEditStatus::InvalidRange: return "InvalidRange";
	case EExtensionApplyEditStatus::OverlappingEdits: return "OverlappingEdits";
	case EExtensionApplyEditStatus::CommandReentry: return "CommandReentry";
	}
	return "InvalidRange";
}

std::string Utf8Path(const std::filesystem::path& path)
{
	return wcstou8s(path.wstring());
}

std::span<const std::uint8_t> Bytes(std::string_view value)
{
	return { reinterpret_cast<const std::uint8_t*>(value.data()), value.size() };
}

picojson::object SerializeDocumentSnapshot(const SExtensionDocumentSnapshot& snapshot)
{
	picojson::object value;
	value["documentId"] = picojson::value(snapshot.id.ToString());
	value["uri"] = picojson::value(wcstou8s(snapshot.uri));
	if (const auto path = ExtensionFilePathFromUri(snapshot.uri)) {
		value["fileName"] = picojson::value(Utf8Path(*path));
	} else {
		value["fileName"] = picojson::value(std::string{});
	}
	value["isUntitled"] = picojson::value(snapshot.uri.starts_with(L"untitled:"));
	value["languageId"] = picojson::value(wcstou8s(snapshot.languageId.empty() ? L"plaintext" : snapshot.languageId));
	value["version"] = picojson::value(static_cast<double>(snapshot.version));
	value["isDirty"] = picojson::value(snapshot.dirty);
	value["isClosed"] = picojson::value(false);
	value["eol"] = picojson::value("crlf");
	value["encoding"] = picojson::value("utf8");
	value["text"] = picojson::value(wcstou8s(snapshot.text));
	return value;
}

picojson::object SerializePosition(const SExtensionTextPosition& position)
{
	picojson::object value;
	value["line"] = picojson::value(static_cast<double>(position.line));
	value["character"] = picojson::value(static_cast<double>(position.character));
	return value;
}

bool SendBrokerMessage(HWND broker, UINT message, WPARAM wParam, LPARAM lParam, DWORD_PTR& result)
{
	result = 0;
	return broker && ::IsWindow(broker) && ::SendMessageTimeoutW(
		broker, message, wParam, lParam, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result) != 0;
}

} // namespace

CExtensionService::CExtensionService(
	HWND editorWindow,
	HWND brokerWindow,
	std::filesystem::path profileDirectory,
	std::shared_ptr<CExtensionViewRegistry> views,
	std::unique_ptr<IExtensionSecretSessionStorage> secrets,
	workbench::problems::MarkerService* markerService,
	workbench::output::OutputService* outputService,
	workbench::IWorkbenchRuntime* workbenchRuntime,
	std::filesystem::path extensionSelectionPath,
	bool defaultProfileExtensionsWhenMissing)
	: m_editorWindow(editorWindow)
	, m_brokerWindow(brokerWindow)
	, m_profileDirectory(std::move(profileDirectory))
	, m_extensionProfileState(std::move(extensionSelectionPath))
	, m_defaultProfileExtensionsWhenMissing(defaultProfileExtensionsWhenMissing)
	, m_views(views ? std::move(views) : std::make_shared<CExtensionViewRegistry>())
	, m_secrets(secrets ? std::move(secrets)
		: std::unique_ptr<IExtensionSecretSessionStorage>(
			std::make_unique<CUnavailableExtensionSecretStorage>()))
	, m_workbenchServiceBridge(markerService || outputService || workbenchRuntime
		? std::make_unique<CExtensionWorkbenchServiceBridge>(markerService, outputService, workbenchRuntime,
			128, workbenchRuntime ? workbenchRuntime->Scm() : nullptr) : nullptr)
{
	const auto tick = static_cast<std::uint64_t>(::GetTickCount64());
	const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(this));
	m_reconnectJitterState = static_cast<std::uint32_t>(
		tick ^ (tick >> 32) ^ address ^ (address >> 32) ^ ::GetCurrentProcessId());
	if (m_reconnectJitterState == 0) m_reconnectJitterState = 0x9e3779b9u;
	RegisterBuiltInCommands();
}

CExtensionService::~CExtensionService()
{
	Shutdown();
}

void CExtensionService::Start()
{
	bool expected = false;
	if (!m_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
	m_stopping.store(false, std::memory_order_release);
	m_worker = std::thread(&CExtensionService::WorkerMain, this);
	Enqueue([this]() { WorkerInitialize(); });
}

void CExtensionService::Shutdown() noexcept
{
	if (!m_started.load(std::memory_order_acquire)) return;
	m_stopping.store(true, std::memory_order_release);
	m_taskReady.notify_all();
	if (m_worker.joinable()) m_worker.join();
	m_started.store(false, std::memory_order_release);
}

void CExtensionService::ExecuteCommand(std::wstring_view command, std::string_view argumentsJson)
{
	if (command.empty()) return;
	SubmitClientAction({
		.kind = ClientActionKind::ExecuteCommand,
		.first = std::wstring(command),
		.json = std::string(argumentsJson),
	});
}

void CExtensionService::RequestTreeChildren(std::wstring_view viewHandle, std::wstring_view parentHandle)
{
	if (viewHandle.empty()) return;
	SubmitClientAction({
		.kind = ClientActionKind::TreeChildren,
		.first = std::wstring(viewHandle),
		.second = std::wstring(parentHandle),
	});
}

void CExtensionService::NotifyTreeSelection(
	std::wstring_view viewHandle,
	const std::vector<std::wstring>& itemHandles)
{
	if (viewHandle.empty()) return;
	SubmitClientAction({
		.kind = ClientActionKind::TreeSelection,
		.first = std::wstring(viewHandle),
		.handles = itemHandles,
	});
}

void CExtensionService::NotifyTreeCheckbox(
	std::wstring_view viewHandle,
	std::wstring_view itemHandle,
	bool checked)
{
	if (viewHandle.empty() || itemHandle.empty()) return;
	SubmitClientAction({
		.kind = ClientActionKind::TreeCheckbox,
		.first = std::wstring(viewHandle),
		.second = std::wstring(itemHandle),
		.checked = checked,
	});
}

void CExtensionService::RequestScmInputChange(
	const std::string_view handle, const std::string_view value, const bool global)
{
	if (handle.empty() || value.size() > (1U << 20)) return;
	Start();
	Enqueue([this, handle = std::string(handle), value = std::string(value), global]() {
		if (!m_registered) return;
		picojson::object params;
		params["handle"] = picojson::value(handle);
		params["value"] = picojson::value(value);
		params["global"] = picojson::value(global);
		(void)SendRequestWorker("extension/scm/inputChange", picojson::value(std::move(params)).serialize(),
			{ .kind = PendingKind::ScmInputChange });
	});
}

void CExtensionService::NotifyViewVisibility(bool visible)
{
	Start();
	Enqueue([this, visible]() {
		m_sidebarVisible = visible;
		if (!m_registered) {
			if (visible) RequestReconnectWorker();
			return;
		}
		if (visible) ActivateContributedViewsWorker();
		NotifyRegisteredViewsVisibilityWorker();
	});
}

void CExtensionService::RequestInstalledExtensionRescan()
{
	Start();
	Enqueue([this]() { RescanInstalledExtensionsWorker(); });
}

void CExtensionService::OpenDocument(SExtensionDocumentSnapshot snapshot)
{
	if (!snapshot.id.IsValid() || snapshot.uri.empty() || snapshot.version == 0) return;
	Enqueue([this, snapshot = std::move(snapshot)]() mutable { OpenDocumentWorker(std::move(snapshot)); });
}

void CExtensionService::ChangeDocument(SExtensionDocumentSnapshot snapshot)
{
	if (!snapshot.id.IsValid() || snapshot.uri.empty() || snapshot.version == 0) return;
	Enqueue([this, snapshot = std::move(snapshot)]() mutable { ChangeDocumentWorker(std::move(snapshot)); });
}

void CExtensionService::SaveDocument(SExtensionDocumentSnapshot snapshot)
{
	if (!snapshot.id.IsValid() || snapshot.uri.empty() || snapshot.version == 0) return;
	Enqueue([this, snapshot = std::move(snapshot)]() mutable { SaveDocumentWorker(std::move(snapshot)); });
}

void CExtensionService::CloseDocument(SExtensionDocumentId id)
{
	if (!id.IsValid()) return;
	Enqueue([this, id]() { CloseDocumentWorker(id); });
}

void CExtensionService::SetActiveEditor(SExtensionDocumentId id, SExtensionTextPosition caret)
{
	Enqueue([this, id, caret]() {
		m_activeDocument = id.IsValid() ? std::optional(id) : std::nullopt;
		m_activeCaret = caret;
		if (m_registered) SendActiveEditorWorker();
	});
}

void CExtensionService::SetWindowState(bool focused)
{
	Enqueue([this, focused]() {
		m_windowFocused = focused;
		if (!m_registered) return;
		picojson::object params;
		params["focused"] = picojson::value(focused);
		params["active"] = picojson::value(true);
		(void)SendRequestWorker("extension/window/didChangeState",
			picojson::value(std::move(params)).serialize(), { .kind = PendingKind::DocumentEvent });
	});
}

void CExtensionService::SetApplyEditHandler(ApplyEditHandler handler)
{
	m_applyEditHandler = std::move(handler);
}

void CExtensionService::SetEditorOptionsHandler(EditorOptionsHandler handler)
{
	m_editorOptionsHandler = std::move(handler);
}

std::vector<SExtensionDiagnostic> CExtensionService::DiagnosticsForUri(std::wstring_view uri) const
{
	return m_diagnostics.ForUri(uri);
}

std::vector<SExtensionProblem> CExtensionService::Problems() const
{
	return m_diagnostics.Problems();
}

std::vector<SExtensionOutputChannel> CExtensionService::OutputChannels() const
{
	return m_output.Snapshot();
}

std::vector<SExtensionProgress> CExtensionService::ProgressItems() const
{
	return m_progress.Snapshot();
}

std::vector<SExtensionNotification> CExtensionService::PendingNotifications() const
{
	return m_notifications.Pending();
}

void CExtensionService::ResolveNotification(
	std::uint64_t id, std::optional<std::size_t> selectedAction)
{
	if (id == 0) return;
	Start();
	Enqueue([this, id, selectedAction]() {
		ResolveNotificationWorker(id, selectedAction);
	});
}

std::vector<SExtensionStatusBarItem> CExtensionService::StatusBarItems() const
{
	auto items = m_statusBar.Snapshot();
	for (const auto& progress : m_progress.Snapshot()) {
		std::wstring text = L"$(sync~spin) ";
		text += progress.title;
		if (!progress.message.empty()) text += L": " + progress.message;
		if (progress.increment > 0) text += L" " + std::to_wstring(static_cast<int>(progress.increment)) + L"%";
		items.push_back({
			.handle = L"progress:" + progress.handle,
			.itemId = L"sakura.extension.progress",
			.extensionId = progress.extensionId,
			.generation = progress.generation,
			.alignment = EExtensionStatusBarAlignment::Left,
			.priority = 10000,
			.text = std::move(text),
			.tooltip = progress.title,
			.accessibilityLabel = progress.title,
			.visible = true,
		});
	}
	return items;
}

std::vector<SExtensionCommandPaletteItem> CExtensionService::SearchCommands(
	std::wstring_view query,
	std::size_t maximumResults) const
{
	return m_commands.Search(query, m_contextKeys, maximumResults);
}

LRESULT CExtensionService::HandleNotificationPrompt(LPARAM promptPointer) noexcept
{
	auto* prompt = reinterpret_cast<NotificationPrompt*>(promptPointer);
	if (!prompt || !prompt->notification) return 0;
	const auto& notification = *prompt->notification;
	std::vector<TASKDIALOG_BUTTON> buttons;
	buttons.reserve(notification.actions.size());
	for (std::size_t index = 0; index < notification.actions.size(); ++index) {
		buttons.push_back({ 1000 + static_cast<int>(index), notification.actions[index].c_str() });
	}
	TASKDIALOGCONFIG config{};
	config.cbSize = sizeof(config);
	config.hwndParent = m_editorWindow;
	config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
	config.dwCommonButtons = buttons.empty() ? TDCBF_OK_BUTTON : 0;
	config.pszWindowTitle = L"Sakura Editor NEXT";
	config.pszMainInstruction = notification.message.c_str();
	config.pszContent = notification.detail.empty() ? nullptr : notification.detail.c_str();
	config.pszMainIcon = notification.severity == EExtensionNotificationSeverity::Error
		? TD_ERROR_ICON : notification.severity == EExtensionNotificationSeverity::Warning
			? TD_WARNING_ICON : TD_INFORMATION_ICON;
	config.cButtons = static_cast<UINT>(buttons.size());
	config.pButtons = buttons.empty() ? nullptr : buttons.data();
	int selected = 0;
	if (SUCCEEDED(::TaskDialogIndirect(&config, &selected, nullptr, nullptr)) && selected >= 1000 &&
		static_cast<std::size_t>(selected - 1000) < notification.actions.size()) {
		prompt->selected = static_cast<std::size_t>(selected - 1000);
	}
	return 1;
}

LRESULT CExtensionService::HandleQuickInputPrompt(LPARAM promptPointer) noexcept
{
	auto* prompt = reinterpret_cast<QuickInputPrompt*>(promptPointer);
	if (!prompt || !prompt->request) return 0;
	CExtensionQuickInputDialog dialog(*prompt->request);
	prompt->completion = dialog.DoModal(m_editorWindow);
	return 1;
}

LRESULT CExtensionService::HandleApplyEditPrompt(LPARAM promptPointer) noexcept
{
	auto* prompt = reinterpret_cast<ApplyEditPrompt*>(promptPointer);
	if (!prompt || !prompt->edits || !m_applyEditHandler) return 0;
	try {
		prompt->completion = m_applyEditHandler(*prompt->edits);
		return 1;
	} catch (...) {
		prompt->completion = {};
		return 0;
	}
}

LRESULT CExtensionService::HandleEditorOptionsPrompt(LPARAM promptPointer) noexcept
{
	auto* prompt = reinterpret_cast<EditorOptionsPrompt*>(promptPointer);
	if (!prompt || !prompt->options || !m_editorOptionsHandler) return 0;
	try {
		prompt->applied = m_editorOptionsHandler(*prompt->options);
		return prompt->applied ? 1 : 0;
	} catch (...) {
		prompt->applied = false;
		return 0;
	}
}

void CExtensionService::OnExtensionPipeBytes(std::vector<std::uint8_t> bytes) noexcept
{
	if (m_stopping.load(std::memory_order_acquire)) return;
	try {
		const auto attemptToken = m_pipeCallbackAttemptToken.load(std::memory_order_acquire);
		const auto generation = m_pipeCallbackGeneration.load(std::memory_order_acquire);
		Enqueue([this, attemptToken, generation, bytes = std::move(bytes)]() mutable {
			HandlePipeBytesWorker(attemptToken, generation, std::move(bytes));
		});
	} catch (...) {
	}
}

void CExtensionService::OnExtensionPipeClosed(std::uint32_t errorCode, std::wstring diagnostic) noexcept
{
	if (m_stopping.load(std::memory_order_acquire)) return;
	try {
		const auto attemptToken = m_pipeCallbackAttemptToken.load(std::memory_order_acquire);
		const auto generation = m_pipeCallbackGeneration.load(std::memory_order_acquire);
		Enqueue([this, attemptToken, generation, errorCode, diagnostic = std::move(diagnostic)]() mutable {
			HandlePipeClosedWorker(attemptToken, generation, errorCode, std::move(diagnostic));
		});
	} catch (...) {
	}
}

void CExtensionService::Enqueue(std::function<void()> task)
{
	if (!task || m_stopping.load(std::memory_order_acquire)) return;
	{
		std::lock_guard lock(m_taskMutex);
		if (m_stopping.load(std::memory_order_relaxed)) return;
		if (m_tasks.size() >= kMaximumQueuedTasks) {
			m_taskQueueOverloaded.store(true, std::memory_order_release);
		} else {
			m_tasks.emplace_back(std::move(task));
		}
	}
	m_taskReady.notify_one();
}

void CExtensionService::WorkerMain() noexcept
{
	try {
		for (;;) {
			std::function<void()> task;
			bool overloaded = false;
			bool documentTimerExpired = false;
			bool reconnectTimerExpired = false;
			{
				std::unique_lock lock(m_taskMutex);
				const auto documentReadyTime = m_documentEvents.NextReadyTime();
				const auto reconnectReadyTime = m_reconnectPolicy.NextDeadline();
				std::optional<CExtensionClientReconnectPolicy::TimePoint> readyTime;
				if (documentReadyTime && reconnectReadyTime) readyTime = std::min(*documentReadyTime, *reconnectReadyTime);
				else readyTime = documentReadyTime ? documentReadyTime : reconnectReadyTime;
				const auto ready = [this]() {
					return m_stopping.load(std::memory_order_acquire) ||
						m_taskQueueOverloaded.load(std::memory_order_acquire) || !m_tasks.empty();
				};
				if (readyTime) {
					(void)m_taskReady.wait_until(lock, *readyTime, ready);
				} else {
					m_taskReady.wait(lock, ready);
				}
				if (m_stopping.load(std::memory_order_acquire)) break;
				const auto now = CExtensionClientReconnectPolicy::Clock::now();
				documentTimerExpired = documentReadyTime && now >= *documentReadyTime;
				reconnectTimerExpired = reconnectReadyTime && now >= *reconnectReadyTime;
				if (!documentTimerExpired && !reconnectTimerExpired) {
					overloaded = m_taskQueueOverloaded.exchange(false, std::memory_order_acq_rel);
					if (overloaded) {
						m_tasks.clear();
					} else {
						task = std::move(m_tasks.front());
						m_tasks.pop_front();
					}
				}
			}
			if (documentTimerExpired || reconnectTimerExpired) {
				if (documentTimerExpired) DrainDocumentChangesWorker(CExtensionEventAggregator::Clock::now());
				if (reconnectTimerExpired) ProcessReconnectDeadlineWorker(CExtensionClientReconnectPolicy::Clock::now());
				continue;
			}
			if (overloaded) {
				const bool reconnect = m_connected || m_awaitingHello;
				FailConnectionWorker(ERROR_NOT_ENOUGH_MEMORY,
					L"Extension RPC task queue limit exceeded", reconnect);
				if (!reconnect) RequestReconnectWorker();
				continue;
			}
			try { task(); } catch (...) {}
		}
	} catch (...) {
	}
	if (m_registered) {
		for (const auto& snapshot : m_documents.Snapshots()) SendDocumentCloseWorker(snapshot.id, true);
	}
	m_connected = false;
	m_awaitingHello = false;
	m_registered = false;
	if (m_transport) m_transport->Close();
	m_transport.reset();
	m_protocol.reset();
	if (m_secrets) {
		(void)m_secrets->ClearSession();
		m_secrets->Stop();
	}
	ReleaseHostLeaseWorker();
	m_sharedState.Close();
	m_pipeCallbackAttemptToken.store(0, std::memory_order_release);
	m_pipeCallbackGeneration.store(0, std::memory_order_release);
	m_reconnectPolicy.Shutdown();
}

std::vector<std::filesystem::path> CExtensionService::LoadInstalledExtensionRootsWorker() const
{
	const auto profileState = m_extensionProfileState.Load();
	if (profileState.status == CExtensionProfileState::EStatus::Invalid ||
		profileState.status == CExtensionProfileState::EStatus::IoError) {
		return {};
	}

	CExtensionManager manager;
	std::vector<std::filesystem::path> roots;
	for (const auto& installed : manager.EnumInstalled()) {
		if (!CExtensionProfileState::IsEnabled(
			profileState, installed.sUniqueId, m_defaultProfileExtensionsWhenMissing)) {
			continue;
		}
		const auto root = installed.dir / CExtensionManager::kVsixContentDir;
		std::error_code error;
		if (std::filesystem::is_regular_file(root / CExtensionManager::kManifestFileName, error) && !error) {
			roots.push_back(root);
		}
	}
	return roots;
}

void CExtensionService::WorkerInitialize()
{
	m_installedRoots = LoadInstalledExtensionRootsWorker();
	if (!m_installedRoots.empty()) RequestReconnectWorker();
}

void CExtensionService::RequestReconnectWorker()
{
	if (m_stopping.load(std::memory_order_acquire) || m_installedRoots.empty()) return;
	if (m_reconnectPolicy.RequestReconnect(CExtensionClientReconnectPolicy::Clock::now())) m_taskReady.notify_one();
}

void CExtensionService::RescanInstalledExtensionsWorker()
{
	if (m_stopping.load(std::memory_order_acquire)) return;

	const auto roots = LoadInstalledExtensionRootsWorker();
	if (roots == m_installedRoots) return;
	m_installedRoots = roots;

	const bool sessionActive = m_connected || m_awaitingHello;
	if (sessionActive) {
		FailConnectionWorker(ERROR_CANCELLED, L"Active extension profile changed", true);
	}
	m_reconnectPolicy.Cancel();
	if (m_installedRoots.empty()) {
		ReleaseHostLeaseWorker();
		return;
	}
	RequestReconnectWorker();
}

void CExtensionService::ProcessReconnectDeadlineWorker(CExtensionClientReconnectPolicy::TimePoint now)
{
	if (m_reconnectPolicy.IsHelloTimedOut(now)) {
		FailConnectionWorker(ERROR_TIMEOUT, L"Extension host hello handshake timed out", true);
		return;
	}
	if (const auto attempt = m_reconnectPolicy.TakeDueReconnect(now)) EnsureConnectedWorker(*attempt);
}

double CExtensionService::NextReconnectJitter() noexcept
{
	m_reconnectJitterState = m_reconnectJitterState * 1664525u + 1013904223u;
	return static_cast<double>(m_reconnectJitterState) / static_cast<double>((std::numeric_limits<std::uint32_t>::max)());
}

void CExtensionService::EnsureConnectedWorker(std::uint64_t attemptToken)
{
	if (m_stopping.load(std::memory_order_acquire) ||
		m_reconnectPolicy.GetState() != CExtensionClientReconnectPolicy::State::Connecting ||
		m_reconnectPolicy.GetActiveToken() != attemptToken) return;
	if (m_installedRoots.empty()) {
		m_reconnectPolicy.Cancel();
		ReleaseHostLeaseWorker();
		return;
	}
	// The control process owns the authorization inventory. Refresh it from the
	// profile-scoped installation immediately before acquiring a PID lease, so a
	// stale install/uninstall snapshot can never authorize SecretStorage.
	DWORD_PTR inventoryRefreshed = 0;
	if (!SendBrokerMessage(m_brokerWindow, MYWM_EXTENSION_HOST_REFRESH_INVENTORY,
		0, 0, inventoryRefreshed) || inventoryRefreshed == 0) {
		(void)m_reconnectPolicy.OnConnectFailure(
			attemptToken, CExtensionClientReconnectPolicy::Clock::now(), NextReconnectJitter());
		return;
	}
	if (!m_leaseAcquired) {
		DWORD_PTR acquired = 0;
		if (!SendBrokerMessage(m_brokerWindow, MYWM_EXTENSION_HOST_ACQUIRE,
			static_cast<WPARAM>(::GetCurrentProcessId()), reinterpret_cast<LPARAM>(m_editorWindow), acquired) || acquired == 0) {
			(void)m_reconnectPolicy.OnConnectFailure(attemptToken, CExtensionClientReconnectPolicy::Clock::now(), NextReconnectJitter());
			return;
		}
		m_leaseAcquired = true;
	}
	if (!m_leaseAcquired) {
		(void)m_reconnectPolicy.OnConnectFailure(attemptToken, CExtensionClientReconnectPolicy::Clock::now(), NextReconnectJitter());
		return;
	}

	std::wstring diagnostic;
	if (!m_sharedState.OpenForEditor(m_profileDirectory, diagnostic)) {
		(void)m_reconnectPolicy.OnConnectFailure(attemptToken, CExtensionClientReconnectPolicy::Clock::now(), NextReconnectJitter());
		return;
	}
	const auto snapshot = m_sharedState.Read();
	if (!snapshot || snapshot->generation == 0 || snapshot->hostProcessId == 0 || snapshot->pipeName.empty() ||
		(snapshot->state != EExtensionHostState::Starting && snapshot->state != EExtensionHostState::Ready)) {
		(void)m_reconnectPolicy.OnConnectFailure(attemptToken, CExtensionClientReconnectPolicy::Clock::now(), NextReconnectJitter());
		return;
	}
	m_connectionSnapshot = *snapshot;
	m_protocol = std::make_unique<CExtensionRpcProtocol>();
	m_transport = std::make_unique<CExtensionPipeTransport>(
		static_cast<IExtensionPipeTransportSink&>(*this));
	m_connectionAttemptToken = attemptToken;
	m_pipeCallbackAttemptToken.store(attemptToken, std::memory_order_release);
	m_pipeCallbackGeneration.store(m_connectionSnapshot.generation, std::memory_order_release);
	const auto connected = m_transport->Connect(
		m_connectionSnapshot.pipeName, m_connectionSnapshot.hostProcessId, kExtensionPipeConnectTimeout);
	if (!connected.success) {
		(void)m_reconnectPolicy.OnConnectFailure(attemptToken, CExtensionClientReconnectPolicy::Clock::now(), NextReconnectJitter());
		FailConnectionWorker(connected.errorCode, connected.diagnostic, true);
		return;
	}
	if (!m_reconnectPolicy.BeginHello(attemptToken, m_connectionSnapshot.generation,
		CExtensionClientReconnectPolicy::Clock::now())) {
		(void)m_reconnectPolicy.OnConnectFailure(attemptToken, CExtensionClientReconnectPolicy::Clock::now(), NextReconnectJitter());
		FailConnectionWorker(ERROR_INVALID_STATE, L"Stale extension host connect attempt", true);
		return;
	}
	m_awaitingHello = true;
}

void CExtensionService::HandlePipeBytesWorker(
	std::uint64_t attemptToken,
	std::uint64_t connectionGeneration,
	std::vector<std::uint8_t> bytes)
{
	if (!m_reconnectPolicy.AcceptsPipeEvent(attemptToken, connectionGeneration) || !m_protocol || bytes.empty()) return;
	const auto received = m_protocol->Feed(std::string_view(
		reinterpret_cast<const char*>(bytes.data()), bytes.size()));
	for (const auto& message : received.messages) HandleMessageWorker(message);
	if (received.IsTerminal()) {
		FailConnectionWorker(ERROR_INVALID_DATA, u8stowcs(received.diagnostic), true);
	}
}

void CExtensionService::HandlePipeClosedWorker(
	std::uint64_t attemptToken,
	std::uint64_t connectionGeneration,
	std::uint32_t errorCode,
	std::wstring diagnostic)
{
	if (!m_reconnectPolicy.AcceptsPipeEvent(attemptToken, connectionGeneration)) return;
	FailConnectionWorker(errorCode, std::move(diagnostic), true);
}

void CExtensionService::HandleMessageWorker(const SExtensionRpcMessage& message)
{
	if (message.eKind == EExtensionRpcMessageKind::SuccessResponse ||
		message.eKind == EExtensionRpcMessageKind::ErrorResponse) {
		HandleResponseWorker(message);
		return;
	}
	if (message.sMethod == "host/hello") {
		if (!HandleHelloWorker(message)) FailConnectionWorker(ERROR_INVALID_DATA, L"Invalid extension host handshake", true);
		return;
	}
	if (!m_connected || !m_dispatcher) return;
	if (message.eKind == EExtensionRpcMessageKind::Request && message.sMethod == "workspace/applyEdit") {
		(void)HandleApplyEditRequestWorker(message);
		return;
	}
	if (message.eKind == EExtensionRpcMessageKind::Request && message.sMethod == "workbench/commands/execute") {
		(void)HandleBuiltInCommandRequestWorker(message);
		return;
	}
	if (message.eKind == EExtensionRpcMessageKind::Request &&
		(message.sMethod == "env/clipboard/readText" || message.sMethod == "env/clipboard/writeText")) {
		(void)HandleEnvironmentClipboardRequestWorker(message);
		return;
	}
	if (message.eKind == EExtensionRpcMessageKind::Notification &&
		message.sMethod == "workspace/document/versionGap" && HandleDocumentVersionGapWorker(message)) return;
	if (message.eKind == EExtensionRpcMessageKind::Notification &&
		message.sMethod == "window/editor/setOptions" && HandleEditorOptionsNotificationWorker(message)) return;
	const auto result = m_dispatcher->Dispatch(message);
	if (result.handled) {
		if (message.eKind == EExtensionRpcMessageKind::Request && !result.responseDeferred) {
			(void)SendResponseWorker(message, result);
		}
		PostWorkbenchChanges(result.changes);
		if (result.success && message.sMethod == "workbench/views/register" && m_sidebarVisible) {
			picojson::object params;
			if (ParseObject(message.sParamsJson, params)) {
				const auto* handle = Find(params, "handle");
				if (handle && handle->is<std::string>()) {
					NotifyViewVisibilityWorker(u8stowcs(handle->get<std::string>()));
				}
			}
		}
		return;
	}
	if (message.eKind == EExtensionRpcMessageKind::Request) {
		SExtensionWorkbenchDispatchResult missing;
		missing.handled = true;
		missing.success = false;
		missing.errorCode = -32601;
		std::string extensionId = "unknown-extension";
		picojson::object params;
		if (ParseObject(message.sParamsJson, params)) {
			if (const auto* value = Find(params, "extensionId");
				value && value->is<std::string>() && !value->get<std::string>().empty()) {
				extensionId = value->get<std::string>();
			}
		}
		missing.errorMessage = "UnsupportedCapability: " + extensionId + " requires " + message.sMethod;
		(void)SendResponseWorker(message, missing);
	}
}

bool CExtensionService::HandleEnvironmentClipboardRequestWorker(const SExtensionRpcMessage& message)
{
	SExtensionWorkbenchDispatchResult response;
	response.handled = true;
	response.errorCode = -32602;

	CClipboard clipboard(m_editorWindow);
	if (!clipboard) {
		response.errorCode = -32001;
		response.errorMessage = "clipboard could not be opened";
		(void)SendResponseWorker(message, response);
		return true;
	}

	if (message.sMethod == "env/clipboard/readText") {
		std::wstring text;
		CEol eol;
		// VS Code returns an empty string when the clipboard has no text. A false
		// GetText result therefore represents an empty clipboard, not an RPC error.
		(void)clipboard.GetText(&text, nullptr, nullptr, eol);
		if (text.size() > kMaximumExtensionClipboardTextCodeUnits) {
			response.errorCode = -32002;
			response.errorMessage = "clipboard text exceeds the supported size";
		} else {
			picojson::object result;
			result["value"] = picojson::value(wcstou8s(text));
			response.success = true;
			response.resultJson = picojson::value(result).serialize();
		}
	} else {
		picojson::object params;
		if (!ParseObject(message.sParamsJson, params)) {
			response.errorMessage = "clipboard request parameters must be an object";
		} else {
			const auto* value = Find(params, "value");
			if (!value || !value->is<std::string>()) {
				response.errorMessage = "clipboard value must be a string";
			} else {
				const std::wstring text = u8stowcs(value->get<std::string>());
				if (text.size() > kMaximumExtensionClipboardTextCodeUnits) {
					response.errorCode = -32002;
					response.errorMessage = "clipboard text exceeds the supported size";
				} else if (!clipboard.SetText(text.c_str(), text.size(), false, false)) {
					response.errorCode = -32001;
					response.errorMessage = "clipboard text could not be written";
				} else {
					response.success = true;
				}
			}
		}
	}

	(void)SendResponseWorker(message, response);
	return true;
}

bool CExtensionService::QueueNotificationRequestWorker(
	const SExtensionNotification& notification, const SExtensionRpcMessage& request)
{
	if (notification.id == 0 || !m_editorWindow || m_stopping.load(std::memory_order_acquire)) {
		return false;
	}
	if (request.eKind != EExtensionRpcMessageKind::Request) return true;
	if (request.sIdJson.empty()) return false;
	return m_pendingNotificationRequests.emplace(notification.id, request).second;
}

void CExtensionService::ResolveNotificationWorker(
	std::uint64_t id, std::optional<std::size_t> selectedAction)
{
	if (!m_notifications.Resolve(id, selectedAction)) return;
	const auto completion = m_notifications.TakeCompletion(id);
	const auto pending = m_pendingNotificationRequests.find(id);
	if (pending != m_pendingNotificationRequests.end()) {
		SExtensionWorkbenchDispatchResult response;
		response.handled = true;
		response.success = true;
		response.changes = EExtensionWorkbenchChange::Notifications;
		picojson::object result;
		if (completion && completion->selectedAction &&
			*completion->selectedAction < completion->actions.size()) {
			result["selectedIndex"] = picojson::value(
				static_cast<double>(*completion->selectedAction));
		}
		response.resultJson = picojson::value(std::move(result)).serialize();
		(void)SendResponseWorker(pending->second, response);
		m_pendingNotificationRequests.erase(pending);
	}
	PostWorkbenchChanges(EExtensionWorkbenchChange::Notifications);
}

bool CExtensionService::HandleDocumentVersionGapWorker(const SExtensionRpcMessage& message)
{
	picojson::object params;
	if (!ParseObject(message.sParamsJson, params)) return false;
	const auto* value = Find(params, "documentId");
	if (!value || !value->is<std::string>()) return false;
	const auto documentId = ParseDocumentId(value->get<std::string>());
	if (!documentId) return false;
	// The shared host broadcasts extension notifications to every editor client.
	// Only the process owning this document can answer; other clients terminate the
	// notification without treating it as an unsupported API.
	DrainDocumentChangesWorker(CExtensionEventAggregator::Clock::time_point::max());
	const auto snapshot = m_documents.Snapshot(*documentId);
	if (snapshot) SendDocumentSnapshotWorker("extension/workspace/didChange", *snapshot, true);
	return true;
}

bool CExtensionService::HandleEditorOptionsNotificationWorker(const SExtensionRpcMessage& message)
{
	picojson::object params;
	if (!ParseObject(message.sParamsJson, params)) return false;
	const auto* documentIdValue = Find(params, "documentId");
	const auto* optionsValue = Find(params, "options");
	if (!documentIdValue || !documentIdValue->is<std::string>() ||
		!optionsValue || !optionsValue->is<picojson::object>()) return false;
	const auto documentId = ParseDocumentId(documentIdValue->get<std::string>());
	if (!documentId) return false;
	SExtensionNativeEditorOptions nativeOptions{ .documentId = *documentId };
	const auto& options = optionsValue->get<picojson::object>();
	if (const auto* tabSize = Find(options, "tabSize"); tabSize && tabSize->is<double>()) {
		const double number = tabSize->get<double>();
		if (!std::isfinite(number) || std::floor(number) != number || number < 1 || number > 256) return false;
		nativeOptions.tabSize = static_cast<std::uint32_t>(number);
	}
	if (const auto* insertSpaces = Find(options, "insertSpaces"); insertSpaces && insertSpaces->is<bool>()) {
		nativeOptions.insertSpaces = insertSpaces->get<bool>();
	}
	if (!nativeOptions.tabSize && !nativeOptions.insertSpaces) return false;
	(void)ApplyEditorOptionsOnUi(nativeOptions);
	return true;
}

bool CExtensionService::HandleBuiltInCommandRequestWorker(const SExtensionRpcMessage& message)
{
	picojson::object params;
	const auto fail = [&](int code, std::string text) {
		SExtensionWorkbenchDispatchResult response;
		response.handled = true;
		response.success = false;
		response.errorCode = code;
		response.errorMessage = std::move(text);
		return SendResponseWorker(message, response);
	};
	if (!ParseObject(message.sParamsJson, params)) return fail(-32602, "command params must be an object");
	const auto* command = Find(params, "command");
	if (!command || !command->is<std::string>() || command->get<std::string>().empty()) {
		return fail(-32602, "command must be a non-empty string");
	}
	if (u8stowcs(command->get<std::string>()) != kFormatDocumentCommand) {
		return fail(-32601, "unsupported native command: " + command->get<std::string>());
	}
	RequestFormatDocumentWorker();
	SExtensionWorkbenchDispatchResult response;
	response.handled = true;
	response.success = true;
	response.resultJson = R"({"value":null})";
	return SendResponseWorker(message, response);
}

bool CExtensionService::ParseApplyEditWorker(
	std::string_view paramsJson,
	std::vector<SExtensionDocumentEdit>& edits,
	std::string& error) const
{
	picojson::object params;
	if (!ParseObject(paramsJson, params)) {
		error = "workspace/applyEdit params must be an object";
		return false;
	}
	const auto* entriesValue = Find(params, "edits");
	const auto* versionsValue = Find(params, "expectedVersions");
	if (!entriesValue || !entriesValue->is<picojson::array>() ||
		!versionsValue || !versionsValue->is<picojson::object>()) {
		error = "workspace/applyEdit requires edits and expectedVersions";
		return false;
	}
	const auto& entries = entriesValue->get<picojson::array>();
	const auto& versions = versionsValue->get<picojson::object>();
	if (entries.empty() || entries.size() > 256) {
		error = "workspace/applyEdit document count is out of range";
		return false;
	}
	const auto snapshots = m_documents.Snapshots();
	std::unordered_set<SExtensionDocumentId> seen;
	std::size_t totalEdits = 0;
	std::size_t totalText = 0;
	edits.clear();
	edits.reserve(entries.size());
	for (const auto& entryValue : entries) {
		if (!entryValue.is<picojson::object>()) {
			error = "workspace/applyEdit edit entry must be an object";
			return false;
		}
		const auto& entry = entryValue.get<picojson::object>();
		std::optional<SExtensionDocumentId> id;
		if (const auto* documentId = Find(entry, "documentId"); documentId && documentId->is<std::string>()) {
			id = ParseDocumentId(documentId->get<std::string>());
		}
		const auto* uriValue = Find(entry, "uri");
		const std::wstring uri = uriValue && uriValue->is<std::string>()
			? u8stowcs(uriValue->get<std::string>()) : std::wstring{};
		if (!id && !uri.empty()) {
			for (const auto& snapshot : snapshots) {
				if (snapshot.uri == uri) { id = snapshot.id; break; }
			}
		}
		if (!id || !seen.emplace(*id).second) {
			error = "workspace/applyEdit references an unknown or duplicate document";
			return false;
		}
		const auto snapshot = m_documents.Snapshot(*id);
		if (!snapshot || (!uri.empty() && uri != snapshot->uri)) {
			error = "workspace/applyEdit document identity does not match the native snapshot";
			return false;
		}
		const auto versionFound = versions.find(id->ToString());
		if (versionFound == versions.end() || !versionFound->second.is<double>()) {
			error = "workspace/applyEdit is missing an expected document version";
			return false;
		}
		const double versionNumber = versionFound->second.get<double>();
		if (!std::isfinite(versionNumber) || std::floor(versionNumber) != versionNumber || versionNumber <= 0 ||
			versionNumber > static_cast<double>((std::numeric_limits<std::uint64_t>::max)())) {
			error = "workspace/applyEdit expected version is invalid";
			return false;
		}
		const auto* textEditsValue = Find(entry, "edits");
		if (!textEditsValue || !textEditsValue->is<picojson::array>()) {
			error = "workspace/applyEdit text edits must be an array";
			return false;
		}
		SExtensionDocumentEdit documentEdit{
			.documentId = *id,
			.expectedVersion = static_cast<std::uint64_t>(versionNumber),
		};
		for (const auto& textEditValue : textEditsValue->get<picojson::array>()) {
			if (++totalEdits > 10000 || !textEditValue.is<picojson::object>()) {
				error = "workspace/applyEdit text edit count is out of range";
				return false;
			}
			const auto& textEdit = textEditValue.get<picojson::object>();
			const auto* rangeValue = Find(textEdit, "range");
			const auto* newTextValue = Find(textEdit, "newText");
			if (!rangeValue || !rangeValue->is<picojson::object>() ||
				!newTextValue || !newTextValue->is<std::string>()) {
				error = "workspace/applyEdit text edit requires range and newText";
				return false;
			}
			const auto& range = rangeValue->get<picojson::object>();
			const auto* startValue = Find(range, "start");
			const auto* endValue = Find(range, "end");
			if (!startValue || !startValue->is<picojson::object>() ||
				!endValue || !endValue->is<picojson::object>()) {
				error = "workspace/applyEdit range is invalid";
				return false;
			}
			SExtensionTextEdit textEditResult;
			if (!JsonUInt32(startValue->get<picojson::object>(), "line", textEditResult.range.start.line) ||
				!JsonUInt32(startValue->get<picojson::object>(), "character", textEditResult.range.start.character) ||
				!JsonUInt32(endValue->get<picojson::object>(), "line", textEditResult.range.end.line) ||
				!JsonUInt32(endValue->get<picojson::object>(), "character", textEditResult.range.end.character)) {
				error = "workspace/applyEdit range position is invalid";
				return false;
			}
			textEditResult.newText = u8stowcs(newTextValue->get<std::string>());
			totalText += textEditResult.newText.size();
			if (totalText > 8 * 1024 * 1024) {
				error = "workspace/applyEdit replacement text exceeds the bounded limit";
				return false;
			}
			if (const auto* newEol = Find(textEdit, "newEol"); newEol && newEol->is<double>()) {
				const int eol = static_cast<int>(newEol->get<double>());
				if (eol != 1 && eol != 2) { error = "workspace/applyEdit newEol is invalid"; return false; }
				documentEdit.crlf = eol == 2;
			}
			documentEdit.edits.push_back(std::move(textEditResult));
		}
		if (const auto* newEol = Find(entry, "newEol"); newEol && newEol->is<double>()) {
			const int eol = static_cast<int>(newEol->get<double>());
			if (eol != 1 && eol != 2) { error = "workspace/applyEdit newEol is invalid"; return false; }
			documentEdit.crlf = eol == 2;
		}
		edits.push_back(std::move(documentEdit));
	}
	return true;
}

bool CExtensionService::HandleApplyEditRequestWorker(const SExtensionRpcMessage& message)
{
	std::vector<SExtensionDocumentEdit> edits;
	std::string parseError;
	if (!ParseApplyEditWorker(message.sParamsJson, edits, parseError)) {
		SExtensionWorkbenchDispatchResult failure;
		failure.handled = true;
		failure.success = false;
		failure.errorCode = -32602;
		failure.errorMessage = std::move(parseError);
		return SendResponseWorker(message, failure);
	}
	auto completion = ApplyEditOnUi(edits);
	if (completion.result.Applied()) {
		for (const auto& snapshot : completion.snapshots) {
			const auto updated = m_documents.Change(snapshot);
			if (updated != EExtensionDocumentUpdateResult::Applied) (void)m_documents.Open(snapshot);
			SendDocumentSnapshotWorker("extension/workspace/didChange", snapshot, true);
		}
	}
	picojson::object result;
	result["applied"] = picojson::value(completion.result.Applied());
	result["reason"] = picojson::value(ApplyEditReason(completion.result.status));
	result["undoUnit"] = picojson::value(static_cast<double>(completion.result.undoUnit));
	picojson::array snapshots;
	for (const auto& snapshot : completion.snapshots) {
		snapshots.emplace_back(SerializeDocumentSnapshot(snapshot));
	}
	if (completion.snapshots.size() == 1) result["snapshot"] = snapshots.front();
	result["snapshots"] = picojson::value(std::move(snapshots));
	SExtensionWorkbenchDispatchResult response;
	response.handled = true;
	response.success = true;
	response.resultJson = picojson::value(std::move(result)).serialize();
	return SendResponseWorker(message, response);
}

void CExtensionService::HandleResponseWorker(const SExtensionRpcMessage& message)
{
	const auto found = m_pendingRequests.find(message.sIdJson);
	if (found == m_pendingRequests.end()) return;
	const auto pending = found->second;
	m_pendingRequests.erase(found);
	if (message.eKind == EExtensionRpcMessageKind::SuccessResponse) {
		if (pending.kind == PendingKind::RegisterExtensions) {
			HandleRegistrationResultWorker(message.sResultJson);
		} else if (pending.kind == PendingKind::FormatDocument) {
			HandleFormatDocumentResponseWorker(pending, message.sResultJson);
		} else if (pending.kind == PendingKind::TreeChildren && m_dispatcher) {
			const auto result = m_dispatcher->ApplyTreeChildrenResult(
				pending.viewHandle, pending.parentHandle, pending.extensionId, pending.generation, message.sResultJson);
			PostWorkbenchChanges(result.changes);
		}
		return;
	}
	if (pending.kind == PendingKind::TreeChildren && m_dispatcher) {
		const auto result = m_dispatcher->ApplyTreeChildrenResult(
			pending.viewHandle, pending.parentHandle, pending.extensionId, pending.generation, "{\"items\":[]}");
		PostWorkbenchChanges(result.changes);
	}
}

void CExtensionService::HandleFormatDocumentResponseWorker(
	const PendingRequest& pending,
	std::string_view resultJson)
{
	const auto current = m_documents.Snapshot(pending.documentId);
	if (!current || current->version != pending.expectedVersion) return;
	picojson::object providerResult;
	if (!ParseObject(resultJson, providerResult)) return;
	const auto* value = Find(providerResult, "value");
	if (!value || value->is<picojson::null>()) return;
	if (!value->is<picojson::array>() || value->get<picojson::array>().empty()) return;
	if (const auto* version = Find(providerResult, "expectedVersion");
		!version || !version->is<double>() || version->get<double>() != static_cast<double>(pending.expectedVersion)) return;

	picojson::object entry;
	entry["documentId"] = picojson::value(pending.documentId.ToString());
	entry["uri"] = picojson::value(wcstou8s(current->uri));
	entry["edits"] = *value;
	picojson::object versions;
	versions[pending.documentId.ToString()] = picojson::value(static_cast<double>(pending.expectedVersion));
	picojson::object request;
	request["expectedVersions"] = picojson::value(std::move(versions));
	request["edits"] = picojson::value(picojson::array{ picojson::value(std::move(entry)) });
	std::vector<SExtensionDocumentEdit> edits;
	std::string parseError;
	if (!ParseApplyEditWorker(picojson::value(std::move(request)).serialize(), edits, parseError)) return;
	auto completion = ApplyEditOnUi(edits);
	if (!completion.result.Applied()) return;
	for (const auto& snapshot : completion.snapshots) {
		const auto updated = m_documents.Change(snapshot);
		if (updated != EExtensionDocumentUpdateResult::Applied) (void)m_documents.Open(snapshot);
		SendDocumentSnapshotWorker("extension/workspace/didChange", snapshot, true);
	}
}

bool CExtensionService::HandleHelloWorker(const SExtensionRpcMessage& message)
{
	if (!m_awaitingHello || message.eKind != EExtensionRpcMessageKind::Notification) return false;
	picojson::object hello;
	if (!ParseObject(message.sParamsJson, hello)) return false;
	std::uint64_t protocolVersion = 0;
	std::uint64_t processId = 0;
	std::uint64_t generation = 0;
	const auto* profileHash = Find(hello, "profileHash");
	const auto* bootId = Find(hello, "bootId");
	if (!JsonUInt64(hello, "protocolVersion", protocolVersion) || protocolVersion != 1 ||
		!JsonUInt64(hello, "processId", processId) || processId != m_connectionSnapshot.hostProcessId ||
		!JsonUInt64(hello, "generation", generation) || generation != m_connectionSnapshot.generation ||
		!profileHash || !profileHash->is<std::string>() || u8stowcs(profileHash->get<std::string>()) != m_connectionSnapshot.profileHash ||
		!bootId || !bootId->is<std::string>() || u8stowcs(bootId->get<std::string>()) != m_connectionSnapshot.bootId) return false;
	if (m_connectionSnapshot.state == EExtensionHostState::Starting) {
		DWORD_PTR accepted = 0;
		if (!SendBrokerMessage(m_brokerWindow, MYWM_EXTENSION_HOST_ACCEPT,
			static_cast<WPARAM>(generation), static_cast<LPARAM>(processId), accepted) || accepted == 0) {
			const auto current = m_sharedState.Read();
			if (!current || current->state != EExtensionHostState::Ready || current->generation != generation) return false;
		}
	}
	const auto sessionId = wcstou8s(m_connectionSnapshot.extensionHostSessionId);
	if (!m_secrets || !m_secrets->BindSession(sessionId, generation).success) return false;
	if (!m_reconnectPolicy.OnHello(m_connectionAttemptToken, generation)) {
		m_secrets->ClearSession();
		return false;
	}
	m_awaitingHello = false;
	m_connected = true;
	m_registered = false;
	ResetDispatcherWorker();
	SendRegisterExtensionsWorker();
	return true;
}

void CExtensionService::SendRegisterExtensionsWorker()
{
	picojson::array extensions;
	for (const auto& root : m_installedRoots) extensions.emplace_back(Utf8Path(root));
	picojson::object params;
	params["extensions"] = picojson::value(std::move(extensions));
	(void)SendRequestWorker("host/registerExtensions", picojson::value(std::move(params)).serialize(),
		{ .kind = PendingKind::RegisterExtensions });
}

void CExtensionService::HandleRegistrationResultWorker(std::string_view resultJson)
{
	picojson::object result;
	if (!ParseObject(resultJson, result)) return;
	m_contributedViewIds.clear();
	m_requestedViewActivations.clear();
	if (const auto* registered = Find(result, "registered"); registered && registered->is<picojson::array>()) {
		for (const auto& extension : registered->get<picojson::array>()) {
			if (!extension.is<picojson::object>()) continue;
			const auto* views = Find(extension.get<picojson::object>(), "views");
			if (!views || !views->is<picojson::array>()) continue;
			for (const auto& view : views->get<picojson::array>()) {
				if (!view.is<picojson::object>()) continue;
				const auto* id = Find(view.get<picojson::object>(), "id");
				if (id && id->is<std::string>() && !id->get<std::string>().empty()) {
					m_contributedViewIds.push_back(u8stowcs(id->get<std::string>()));
				}
			}
		}
	}
	m_registered = true;
	for (const auto& snapshot : m_documents.Snapshots()) {
		SendDocumentSnapshotWorker("extension/workspace/didOpen", snapshot);
	}
	SendActiveEditorWorker();
	{
		picojson::object params;
		params["focused"] = picojson::value(m_windowFocused);
		params["active"] = picojson::value(true);
		(void)SendRequestWorker("extension/window/didChangeState",
			picojson::value(std::move(params)).serialize(), { .kind = PendingKind::DocumentEvent });
	}
	SendActivateEventWorker(L"onStartupFinished");
	if (m_sidebarVisible) ActivateContributedViewsWorker();
	DrainDeferredActionsWorker();
}

void CExtensionService::SendActivateEventWorker(std::wstring_view event)
{
	picojson::object params;
	params["event"] = picojson::value(wcstou8s(std::wstring(event)));
	(void)SendRequestWorker("host/activateByEvent", picojson::value(std::move(params)).serialize(),
		{ .kind = PendingKind::ActivateEvent });
}

void CExtensionService::ActivateContributedViewsWorker()
{
	for (const auto& viewId : m_contributedViewIds) {
		if (m_requestedViewActivations.emplace(viewId).second) {
			SendActivateEventWorker(L"onView:" + viewId);
		}
	}
}

void CExtensionService::NotifyViewVisibilityWorker(std::wstring_view handle)
{
	if (handle.empty()) return;
	picojson::object params;
	params["handle"] = picojson::value(wcstou8s(std::wstring(handle)));
	params["visible"] = picojson::value(m_sidebarVisible);
	(void)SendRequestWorker("extension/views/didChangeVisibility",
		picojson::value(std::move(params)).serialize(), { .kind = PendingKind::ViewEvent });
}

void CExtensionService::NotifyRegisteredViewsVisibilityWorker()
{
	for (const auto& view : m_views->Views()) {
		NotifyViewVisibilityWorker(view.handle);
	}
}

void CExtensionService::SubmitClientAction(ClientAction action)
{
	Start();
	Enqueue([this, action = std::move(action)]() mutable { RunClientActionWorker(std::move(action)); });
}

void CExtensionService::RunClientActionWorker(ClientAction action)
{
	if (!m_registered) {
		if (m_deferredActions.size() < 256) m_deferredActions.emplace_back(std::move(action));
		RequestReconnectWorker();
		return;
	}
	picojson::object params;
	PendingRequest pending;
	switch (action.kind) {
	case ClientActionKind::ExecuteCommand: {
		if (action.first == kFormatDocumentCommand) {
			RequestFormatDocumentWorker();
			break;
		}
		params["command"] = picojson::value(wcstou8s(action.first));
		picojson::value arguments;
		const auto error = picojson::parse(arguments, action.json.empty() ? "[]" : action.json);
		if (!error.empty() || !arguments.is<picojson::array>()) arguments = picojson::value(picojson::array{});
		params["args"] = std::move(arguments);
		pending.kind = PendingKind::ExecuteCommand;
		(void)SendRequestWorker("extension/commands/execute", picojson::value(std::move(params)).serialize(), std::move(pending));
		break;
	}
	case ClientActionKind::TreeChildren: {
		const auto owner = FindView(action.first);
		if (!owner) return;
		params["handle"] = picojson::value(wcstou8s(action.first));
		if (!action.second.empty()) params["parentHandle"] = picojson::value(wcstou8s(action.second));
		pending.kind = PendingKind::TreeChildren;
		pending.viewHandle = action.first;
		pending.parentHandle = action.second;
		pending.extensionId = owner->extensionId;
		pending.generation = owner->generation;
		(void)SendRequestWorker("extension/views/getChildren", picojson::value(std::move(params)).serialize(), std::move(pending));
		break;
	}
	case ClientActionKind::TreeSelection: {
		params["handle"] = picojson::value(wcstou8s(action.first));
		picojson::array handles;
		for (const auto& handle : action.handles) handles.emplace_back(wcstou8s(handle));
		params["itemHandles"] = picojson::value(std::move(handles));
		(void)SendRequestWorker("extension/views/didSelect", picojson::value(std::move(params)).serialize(),
			{ .kind = PendingKind::ViewEvent });
		break;
	}
	case ClientActionKind::TreeCheckbox: {
		params["handle"] = picojson::value(wcstou8s(action.first));
		picojson::object item;
		item["handle"] = picojson::value(wcstou8s(action.second));
		item["state"] = picojson::value(static_cast<double>(action.checked ? 1 : 0));
		params["items"] = picojson::value(picojson::array{ picojson::value(std::move(item)) });
		(void)SendRequestWorker("extension/views/didChangeCheckboxState",
			picojson::value(std::move(params)).serialize(), { .kind = PendingKind::ViewEvent });
		break;
	}
	}
}

void CExtensionService::RequestFormatDocumentWorker()
{
	if (!m_activeDocument) return;
	const auto snapshot = m_documents.Snapshot(*m_activeDocument);
	if (!snapshot) return;
	picojson::object options;
	options["insertSpaces"] = picojson::value(true);
	options["tabSize"] = picojson::value(4.0);
	picojson::object params;
	params["kind"] = picojson::value("formatDocument");
	params["documentId"] = picojson::value(snapshot->id.ToString());
	params["uri"] = picojson::value(wcstou8s(snapshot->uri));
	params["options"] = picojson::value(std::move(options));
	(void)SendRequestWorker("extension/languages/provide", picojson::value(std::move(params)).serialize(), {
		.kind = PendingKind::FormatDocument,
		.documentId = snapshot->id,
		.expectedVersion = snapshot->version,
	});
}

void CExtensionService::RegisterBuiltInCommands()
{
	(void)m_commands.Register({
		.id = std::wstring(kFormatDocumentCommand),
		.title = L"Format Document",
		.category = L"Editor",
		.builtIn = true,
	});
}

void CExtensionService::DrainDeferredActionsWorker()
{
	auto actions = std::move(m_deferredActions);
	m_deferredActions.clear();
	for (auto& action : actions) RunClientActionWorker(std::move(action));
}

void CExtensionService::OpenDocumentWorker(SExtensionDocumentSnapshot snapshot)
{
	if (const auto current = m_documents.Snapshot(snapshot.id); current && current->uri != snapshot.uri) {
		DrainDocumentChangesWorker(CExtensionEventAggregator::Clock::time_point::max());
		if (m_registered) SendDocumentCloseWorker(snapshot.id);
		(void)m_documents.Close(snapshot.id);
	}
	if (m_documents.Open(snapshot) != EExtensionDocumentUpdateResult::Applied) return;
	if (m_registered) SendDocumentSnapshotWorker("extension/workspace/didOpen", snapshot);
}

void CExtensionService::ChangeDocumentWorker(SExtensionDocumentSnapshot snapshot)
{
	const auto result = m_documents.Change(snapshot);
	if (result == EExtensionDocumentUpdateResult::Applied) {
		m_documentEvents.Enqueue(std::move(snapshot));
		return;
	}
	if (result == EExtensionDocumentUpdateResult::UnknownDocument) {
		if (m_documents.Open(snapshot) == EExtensionDocumentUpdateResult::Applied && m_registered) {
			SendDocumentSnapshotWorker("extension/workspace/didOpen", snapshot);
		}
		return;
	}
	if (result == EExtensionDocumentUpdateResult::VersionGap) {
		// A bounded UI queue may deliberately drop intermediate versions under overload.
		// A complete snapshot is the explicit recovery state; do not leave the model stale.
		if (m_documents.Open(snapshot) == EExtensionDocumentUpdateResult::Applied && m_registered) {
			SendDocumentSnapshotWorker("extension/workspace/didChange", snapshot, true);
		}
	}
}

void CExtensionService::SaveDocumentWorker(SExtensionDocumentSnapshot snapshot)
{
	DrainDocumentChangesWorker(CExtensionEventAggregator::Clock::time_point::max());
	snapshot.dirty = false;
	const auto result = m_documents.Save(snapshot);
	if (result == EExtensionDocumentUpdateResult::UnknownDocument) {
		snapshot.dirty = false;
		if (m_documents.Open(snapshot) == EExtensionDocumentUpdateResult::Applied && m_registered) {
			SendDocumentSnapshotWorker("extension/workspace/didOpen", snapshot);
		}
		return;
	}
	if (result != EExtensionDocumentUpdateResult::Applied) return;
	if (m_registered) SendDocumentSnapshotWorker("extension/workspace/didSave", snapshot);
}

void CExtensionService::CloseDocumentWorker(SExtensionDocumentId id)
{
	DrainDocumentChangesWorker(CExtensionEventAggregator::Clock::time_point::max());
	if (!m_documents.Snapshot(id)) return;
	if (m_registered) SendDocumentCloseWorker(id);
	(void)m_documents.Close(id);
	if (m_activeDocument == id) {
		m_activeDocument.reset();
		if (m_registered) SendActiveEditorWorker();
	}
}

void CExtensionService::DrainDocumentChangesWorker(CExtensionEventAggregator::Clock::time_point now)
{
	for (auto& event : m_documentEvents.DrainReady(now)) {
		if (m_registered) {
			SendDocumentSnapshotWorker("extension/workspace/didChange", event.snapshot,
				event.snapshotOnly, event.coalescedChanges);
		}
	}
}

void CExtensionService::SendDocumentSnapshotWorker(
	std::string_view method,
	const SExtensionDocumentSnapshot& snapshot,
	bool snapshotOnly,
	std::size_t coalescedChanges)
{
	picojson::object params;
	params["snapshot"] = picojson::value(SerializeDocumentSnapshot(snapshot));
	params["snapshotOnly"] = picojson::value(snapshotOnly);
	params["coalescedChanges"] = picojson::value(static_cast<double>(coalescedChanges));
	(void)SendRequestWorker(method, picojson::value(std::move(params)).serialize(),
		{ .kind = PendingKind::DocumentEvent });
}

void CExtensionService::SendDocumentCloseWorker(const SExtensionDocumentId& id, bool notification)
{
	picojson::object params;
	params["documentId"] = picojson::value(id.ToString());
	const auto json = picojson::value(std::move(params)).serialize();
	if (notification) {
		(void)SendNotificationWorker("extension/workspace/didClose", json);
	} else {
		(void)SendRequestWorker("extension/workspace/didClose", json, { .kind = PendingKind::DocumentEvent });
	}
}

void CExtensionService::SendActiveEditorWorker()
{
	picojson::object params;
	if (m_activeDocument && m_documents.Snapshot(*m_activeDocument)) {
		picojson::object editor;
		editor["documentId"] = picojson::value(m_activeDocument->ToString());
		picojson::object selection;
		selection["anchor"] = picojson::value(SerializePosition(m_activeCaret));
		selection["active"] = picojson::value(SerializePosition(m_activeCaret));
		editor["selections"] = picojson::value(picojson::array{ picojson::value(std::move(selection)) });
		params["editor"] = picojson::value(std::move(editor));
	} else {
		params["editor"] = picojson::value();
	}
	(void)SendRequestWorker("extension/window/didChangeActiveTextEditor",
		picojson::value(std::move(params)).serialize(), { .kind = PendingKind::DocumentEvent });
}

bool CExtensionService::SendRequestWorker(
	std::string_view method,
	std::string_view paramsJson,
	PendingRequest pending)
{
	if (!m_connected || !m_protocol || !m_transport) return false;
	if (m_pendingRequests.size() >= kMaximumPendingRequests) {
		FailConnectionWorker(ERROR_NOT_ENOUGH_MEMORY,
			L"Extension RPC pending request limit exceeded", true);
		return false;
	}
	SExtensionRpcOutbound outbound;
	std::string error;
	if (!m_protocol->CreateRequest(method, paramsJson, outbound, error)) return false;
	m_pendingRequests.emplace(outbound.sIdJson, std::move(pending));
	if (SendOutboundWorker(outbound)) return true;
	m_pendingRequests.erase(outbound.sIdJson);
	return false;
}

bool CExtensionService::SendNotificationWorker(std::string_view method, std::string_view paramsJson)
{
	if (!m_connected || !m_protocol || !m_transport) return false;
	SExtensionRpcOutbound outbound;
	std::string error;
	return m_protocol->CreateNotification(method, paramsJson, outbound, error) && SendOutboundWorker(outbound);
}

bool CExtensionService::SendResponseWorker(
	const SExtensionRpcMessage& request,
	const SExtensionWorkbenchDispatchResult& result)
{
	if (!m_protocol) return false;
	SExtensionRpcOutbound outbound;
	std::string error;
	const bool created = result.success
		? m_protocol->CreateSuccessResponse(request.sIdJson, result.resultJson, outbound, error)
		: m_protocol->CreateErrorResponse(request.sIdJson, result.errorCode,
			result.errorMessage.empty() ? "extension workbench request failed" : result.errorMessage, {}, outbound, error);
	return created && SendOutboundWorker(outbound);
}

bool CExtensionService::SendOutboundWorker(const SExtensionRpcOutbound& outbound)
{
	if (!m_transport || outbound.frame.empty()) return false;
	const auto result = m_transport->Send(Bytes(outbound.frame));
	if (!result.success) FailConnectionWorker(result.errorCode, result.diagnostic, true);
	return result.success;
}

void CExtensionService::FailConnectionWorker(
	std::uint32_t errorCode,
	std::wstring diagnostic,
	bool notifyBroker)
{
	const auto generation = m_connectionSnapshot.generation;
	const auto scheduled = m_reconnectPolicy.OnFailure(m_connectionAttemptToken, generation,
		CExtensionClientReconnectPolicy::Clock::now(), NextReconnectJitter());
	m_connected = false;
	m_awaitingHello = false;
	m_registered = false;
	m_requestedViewActivations.clear();
	if (m_protocol) (void)m_protocol->CloseHostLost(wcstou8s(diagnostic));
	m_pipeCallbackAttemptToken.store(0, std::memory_order_release);
	m_pipeCallbackGeneration.store(0, std::memory_order_release);
	if (m_transport) m_transport->Close();
	m_transport.reset();
	m_protocol.reset();
	m_connectionAttemptToken = 0;
	if (m_secrets) (void)m_secrets->ClearSession();
	m_connectionSnapshot = {};
	m_pendingRequests.clear();
	ClearWorkbenchWorker();
	if (notifyBroker && generation != 0) {
		DWORD_PTR ignored = 0;
		(void)SendBrokerMessage(m_brokerWindow, MYWM_EXTENSION_HOST_LOST,
			static_cast<WPARAM>(generation), static_cast<LPARAM>(errorCode), ignored);
	}
	if (scheduled) m_taskReady.notify_one();
}

void CExtensionService::ReleaseHostLeaseWorker() noexcept
{
	if (!m_leaseAcquired) return;
	DWORD_PTR ignored = 0;
	(void)SendBrokerMessage(m_brokerWindow, MYWM_EXTENSION_HOST_RELEASE,
		static_cast<WPARAM>(::GetCurrentProcessId()), reinterpret_cast<LPARAM>(m_editorWindow), ignored);
	m_leaseAcquired = false;
}

void CExtensionService::ClearWorkbenchWorker()
{
	if (m_workbenchServiceBridge && !m_workbenchServiceBridge->DisposeAll(m_diagnostics, m_output)) return;
	m_contextKeys.Clear();
	m_commands.Clear();
	RegisterBuiltInCommands();
	m_statusBar.Clear();
	m_notifications.Clear();
	m_pendingNotificationRequests.clear();
	m_diagnostics.Clear();
	m_quickInput.Clear();
	m_output.ClearAll();
	m_progress.Clear();
	m_views->Clear();
	ResetDispatcherWorker();
	PostWorkbenchChanges(EExtensionWorkbenchChange::Commands | EExtensionWorkbenchChange::StatusBar |
		EExtensionWorkbenchChange::Views | EExtensionWorkbenchChange::Notifications |
		EExtensionWorkbenchChange::Diagnostics | EExtensionWorkbenchChange::QuickInput |
		EExtensionWorkbenchChange::Output | EExtensionWorkbenchChange::Progress);
}

void CExtensionService::ResetDispatcherWorker()
{
	m_dispatcher = std::make_unique<CExtensionWorkbenchDispatcher>(
		m_contextKeys, m_commands, m_statusBar, m_notifications, *m_views, *m_secrets,
		m_diagnostics, m_quickInput, m_output, m_progress, m_workbenchServiceBridge.get());
	m_dispatcher->SetNotificationHandler([this](const SExtensionNotification& notification) {
		return ShowNotificationOnUi(notification);
	});
	m_dispatcher->SetDeferredNotificationHandler(
		[this](const SExtensionNotification& notification, const SExtensionRpcMessage& request) {
			return QueueNotificationRequestWorker(notification, request);
		});
	m_dispatcher->SetQuickInputHandler([this](const SExtensionQuickInputRequest& request) {
		return ShowQuickInputOnUi(request);
	});
}

void CExtensionService::PostWorkbenchChanges(EExtensionWorkbenchChange changes) const noexcept
{
	if (changes == EExtensionWorkbenchChange::None || !m_editorWindow) return;
	(void)::PostMessageW(m_editorWindow, MYWM_EXTENSION_WORKBENCH_CHANGED,
		static_cast<WPARAM>(changes), 0);
}

std::optional<std::size_t> CExtensionService::ShowNotificationOnUi(
	const SExtensionNotification& notification)
{
	if (!m_editorWindow || m_stopping.load(std::memory_order_acquire)) return std::nullopt;
	NotificationPrompt prompt{ &notification, std::nullopt };
	DWORD_PTR ignored = 0;
	if (::SendMessageTimeoutW(m_editorWindow, MYWM_EXTENSION_NOTIFICATION_PROMPT, 0,
		reinterpret_cast<LPARAM>(&prompt), SMTO_ABORTIFHUNG | SMTO_BLOCK, 60 * 1000, &ignored) == 0) {
		return std::nullopt;
	}
	return prompt.selected;
}

SExtensionQuickInputCompletion CExtensionService::ShowQuickInputOnUi(
	const SExtensionQuickInputRequest& request)
{
	QuickInputPrompt prompt{ &request, { .id = request.id, .state = EExtensionQuickInputState::HostLost } };
	DWORD_PTR ignored = 0;
	if (!m_editorWindow || m_stopping.load(std::memory_order_acquire) ||
		::SendMessageTimeoutW(m_editorWindow, MYWM_EXTENSION_QUICK_INPUT_PROMPT, 0,
			reinterpret_cast<LPARAM>(&prompt), SMTO_ABORTIFHUNG | SMTO_BLOCK, 5 * 60 * 1000, &ignored) == 0) {
		return { .id = request.id, .state = EExtensionQuickInputState::HostLost };
	}
	return std::move(prompt.completion);
}

CExtensionService::NativeApplyEditResult CExtensionService::ApplyEditOnUi(
	const std::vector<SExtensionDocumentEdit>& edits)
{
	ApplyEditPrompt prompt{ &edits, {} };
	DWORD_PTR ignored = 0;
	if (!m_editorWindow || m_stopping.load(std::memory_order_acquire) ||
		::SendMessageTimeoutW(m_editorWindow, MYWM_EXTENSION_APPLY_EDIT_PROMPT, 0,
			reinterpret_cast<LPARAM>(&prompt), SMTO_ABORTIFHUNG | SMTO_BLOCK, 30 * 1000, &ignored) == 0) {
		return {};
	}
	return std::move(prompt.completion);
}

bool CExtensionService::ApplyEditorOptionsOnUi(const SExtensionNativeEditorOptions& options)
{
	EditorOptionsPrompt prompt{ &options, false };
	DWORD_PTR ignored = 0;
	if (!m_editorWindow || m_stopping.load(std::memory_order_acquire) ||
		::SendMessageTimeoutW(m_editorWindow, MYWM_EXTENSION_EDITOR_OPTIONS_PROMPT, 0,
			reinterpret_cast<LPARAM>(&prompt), SMTO_ABORTIFHUNG | SMTO_BLOCK, 30 * 1000, &ignored) == 0) {
		return false;
	}
	return prompt.applied;
}

std::optional<SExtensionViewDescriptor> CExtensionService::FindView(std::wstring_view handle) const
{
	for (const auto& view : m_views->Views()) {
		if (view.handle == handle) return view;
	}
	return std::nullopt;
}
