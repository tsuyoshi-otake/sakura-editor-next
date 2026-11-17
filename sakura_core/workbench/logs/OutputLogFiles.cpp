/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "workbench/logs/OutputLogFiles.h"

#include "workbench/logs/LogFileSystem.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <limits>

namespace workbench::logs {
namespace {

[[nodiscard]] std::string_view LevelName(output::EOutputLogLevel level) noexcept
{
	switch (level) {
	case output::EOutputLogLevel::Trace: return "trace";
	case output::EOutputLogLevel::Debug: return "debug";
	case output::EOutputLogLevel::Info: return "info";
	case output::EOutputLogLevel::Warning: return "warning";
	case output::EOutputLogLevel::Error: return "error";
	}
	return "info";
}

[[nodiscard]] std::wstring Utf8ToWide(std::string_view value)
{
	if (value.empty()) return {};
	const int length = ::MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
	if (length <= 0) return {};
	std::wstring wide(static_cast<std::size_t>(length), L'\0');
	(void)::MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(), length);
	return wide;
}

//! One path segment Windows accepts, derived from a display name or owner ID.
[[nodiscard]] std::wstring FileNameSegment(std::string_view value, std::wstring_view fallback)
{
	constexpr std::size_t kMaximumSegmentCharacters = 96;
	std::wstring segment = Utf8ToWide(value);
	for (auto& character : segment) {
		if (character < 0x20 || std::wstring_view(L"<>:\"/\\|?*").find(character) != std::wstring_view::npos) {
			character = L'_';
		}
	}
	if (segment.size() > kMaximumSegmentCharacters) segment.resize(kMaximumSegmentCharacters);
	while (!segment.empty() && (segment.back() == L'.' || segment.back() == L' ')) segment.pop_back();
	if (segment.empty()) segment = fallback;
	// Device names stay reserved whatever the extension and whatever spaces
	// precede it, so `CON.log` and `NUL .x.log` both fail.
	std::wstring stem = segment.substr(0, segment.find(L'.'));
	while (!stem.empty() && stem.back() == L' ') stem.pop_back();
	std::ranges::transform(stem, stem.begin(), [](wchar_t c) { return static_cast<wchar_t>(::towupper(c)); });
	static constexpr std::wstring_view kReserved[] = {
		L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$",
		L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
		L"COM\u00B9", L"COM\u00B2", L"COM\u00B3",
		L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9",
		L"LPT\u00B9", L"LPT\u00B2", L"LPT\u00B3",
	};
	if (std::ranges::find(kReserved, std::wstring_view(stem)) != std::end(kReserved)) segment.insert(0, 1, L'_');
	return segment;
}

//! A file stem ending in `.<digits>` would be the rotation backup of another
//! channel's file (`Foo.1` is `Foo.log`'s backup `Foo.1.log`).
[[nodiscard]] std::wstring WithoutBackupSuffix(std::wstring stem)
{
	const auto dot = stem.find_last_of(L'.');
	if (dot == std::wstring::npos || dot + 1 == stem.size()) return stem;
	if (std::all_of(stem.begin() + static_cast<std::ptrdiff_t>(dot) + 1, stem.end(),
			[](wchar_t c) { return c >= L'0' && c <= L'9'; })) {
		stem[dot] = L'_';
	}
	return stem;
}

[[nodiscard]] std::filesystem::path BackupPathFor(const std::filesystem::path& file)
{
	auto backup = file;
	backup.replace_extension();
	backup += L".1";
	backup += file.extension();
	return backup;
}

} // namespace

std::string FormatOutputLogLine(const LogTimestamp& timestamp, output::EOutputLogLevel level, std::string_view message)
{
	char prefix[32]{};
	(void)std::snprintf(prefix, sizeof(prefix), "%04d-%02d-%02d %02d:%02d:%02d.%03d ",
		timestamp.year, timestamp.month, timestamp.day, timestamp.hour, timestamp.minute, timestamp.second,
		timestamp.millisecond);
	std::string line(prefix);
	line += '[';
	line += LevelName(level);
	line += "] ";
	line += message;
	line += '\n';
	return line;
}

std::filesystem::path OutputLogRelativePath(std::string_view ownerId, std::string_view label)
{
	std::filesystem::path file = WithoutBackupSuffix(FileNameSegment(label, L"output")) + L".log";
	constexpr std::string_view kCorePrefix = "core.";
	if (ownerId.substr(0, kCorePrefix.size()) == kCorePrefix) return file;
	const std::string_view extension = ownerId == "workbench.scm.git" ? std::string_view("vscode.git") : ownerId;
	return std::filesystem::path(L"exthost") / FileNameSegment(extension, L"extension") / file;
}

COutputLogFileWriter::COutputLogFileWriter(std::filesystem::path windowFolder, std::shared_ptr<ILogFileSystem> files,
	OutputLogFileWriterLimits limits, FailingChanged onFailingChanged)
	: m_windowFolder(std::move(windowFolder))
	, m_files(std::move(files))
	, m_limits(limits)
	, m_onFailingChanged(std::move(onFailingChanged))
{
}

COutputLogFileWriter::~COutputLogFileWriter()
{
	Stop();
}

bool COutputLogFileWriter::Start(BeforeFirstWrite beforeFirstWrite) noexcept
{
	std::lock_guard lock(m_mutex);
	if (m_started || m_stopping || !m_files) return m_started;
	try {
		m_worker = std::thread([this, beforeFirstWrite = std::move(beforeFirstWrite)] {
			if (beforeFirstWrite) {
				try {
					beforeFirstWrite([this] { return m_stopRequested.load(); });
				} catch (...) {
				}
			}
			WorkerMain();
		});
		m_started = true;
	} catch (...) {
		m_started = false;
	}
	return m_started;
}

void COutputLogFileWriter::Append(const std::filesystem::path& relativePath, std::string bytes) noexcept
{
	if (bytes.empty()) return;
	try {
		std::lock_guard lock(m_mutex);
		if (!m_started || m_stopping) return;
		if (bytes.size() > m_limits.maximumPendingBytes - (std::min)(m_pendingBytes, m_limits.maximumPendingBytes)) {
			if (m_diagnostics.droppedBytes < std::numeric_limits<std::uint64_t>::max() - bytes.size()) {
				m_diagnostics.droppedBytes += bytes.size();
			} else {
				m_diagnostics.droppedBytes = std::numeric_limits<std::uint64_t>::max();
			}
			return;
		}
		m_pendingBytes += bytes.size();
		m_pending.push_back({ relativePath, std::move(bytes) });
	} catch (...) {
		return;
	}
	m_wake.notify_one();
}

void COutputLogFileWriter::Stop() noexcept
{
	m_stopRequested = true;
	{
		std::lock_guard lock(m_mutex);
		m_stopping = true;
	}
	m_wake.notify_all();
	if (m_worker.joinable() && m_worker.get_id() != std::this_thread::get_id()) m_worker.join();
}

OutputLogFileWriterDiagnostics COutputLogFileWriter::Diagnostics() const noexcept
{
	std::lock_guard lock(m_mutex);
	return m_diagnostics;
}

void COutputLogFileWriter::WorkerMain() noexcept
{
	std::unique_lock lock(m_mutex);
	for (;;) {
		m_wake.wait(lock, [this] { return m_stopping || !m_pending.empty(); });
		if (m_pending.empty()) return;
		auto batch = std::move(m_pending);
		m_pending.clear();
		m_pendingBytes = 0;
		lock.unlock();
		// One append per file keeps the order of every file's lines.
		std::vector<std::pair<std::filesystem::path, std::string>> files;
		bool grouped = true;
		try {
			for (auto& write : batch) {
				auto found = std::ranges::find(files, write.relativePath,
					&std::pair<std::filesystem::path, std::string>::first);
				if (found == files.end()) {
					files.emplace_back(std::move(write.relativePath), std::move(write.bytes));
				} else {
					found->second += write.bytes;
				}
			}
		} catch (...) {
			files.clear();
			grouped = false;
		}
		std::uint64_t writtenBytes = 0;
		std::uint64_t failedWrites = grouped ? 0 : 1;
		std::uint64_t rotations = 0;
		for (const auto& [relativePath, bytes] : files) {
			const auto outcome = WriteFile(relativePath, bytes);
			if (outcome.written) {
				writtenBytes += bytes.size();
			} else {
				++failedWrites;
			}
			if (outcome.rotated) ++rotations;
		}
		RecordBatch(failedWrites == 0, writtenBytes, failedWrites, rotations);
		lock.lock();
	}
}

COutputLogFileWriter::WriteOutcome COutputLogFileWriter::WriteFile(
	const std::filesystem::path& relativePath, const std::string& bytes) noexcept
{
	try {
		const auto file = m_windowFolder / relativePath;
		const auto directory = file.parent_path();
		const auto cached = std::ranges::find(m_createdDirectories, directory);
		if (cached == m_createdDirectories.end()) {
			if (!m_files->CreateDirectories(directory)) return {};
			m_createdDirectories.push_back(directory);
		}
		WriteOutcome outcome;
		if (const auto size = m_files->FileSize(file);
			size && *size > 0 && *size + bytes.size() > m_limits.maximumFileBytes) {
			// A reader without delete sharing (tail -f, an editor) blocks the
			// rename; the file then grows past the bound instead of dropping
			// every later line while that reader stays open.
			outcome.rotated = m_files->MoveReplace(file, BackupPathFor(file));
		}
		outcome.written = m_files->Append(file, bytes);
		if (!outcome.written) {
			// The folder may be gone (removed by hand or by another session's
			// retention); create it again before the next write.
			std::erase(m_createdDirectories, directory);
		}
		return outcome;
	} catch (...) {
		return {};
	}
}

void COutputLogFileWriter::RecordBatch(bool succeeded, std::uint64_t writtenBytes, std::uint64_t failedWrites,
	std::uint64_t rotations) noexcept
{
	constexpr auto kMaximum = std::numeric_limits<std::uint64_t>::max();
	const auto add = [](std::uint64_t& counter, std::uint64_t value) {
		counter = value > kMaximum - counter ? kMaximum : counter + value;
	};
	bool changed = false;
	{
		std::lock_guard lock(m_mutex);
		add(m_diagnostics.writtenBytes, writtenBytes);
		add(m_diagnostics.failedWrites, failedWrites);
		add(m_diagnostics.rotations, rotations);
		changed = m_diagnostics.failing == succeeded;
		m_diagnostics.failing = !succeeded;
	}
	if (changed && m_onFailingChanged) {
		try {
			m_onFailingChanged(!succeeded);
		} catch (...) {
		}
	}
}

CFileBackedOutputService::CFileBackedOutputService(output::IOutputService& provider, COutputLogFileWriter& writer,
	Clock clock)
	: m_provider(provider)
	, m_writer(writer)
	, m_clock(clock ? std::move(clock) : Clock(CurrentLocalLogTimestamp))
{
}

output::OutputOperationResult CFileBackedOutputService::CreateChannel(const output::OutputCreateChannelRequest& request)
{
	std::lock_guard lock(m_mutex);
	const auto result = m_provider.CreateChannel(request);
	if (result.status == output::EOutputOperationStatus::Succeeded && request.kind == output::EOutputChannelKind::Log) {
		m_logChannels.insert_or_assign(ChannelKey{ request.owner.ownerId, request.channelId },
			OutputLogRelativePath(request.owner.ownerId, request.label));
	}
	return result;
}

output::OutputOperationResult CFileBackedOutputService::AppendOutput(const output::OutputTextMutationRequest& request)
{
	return m_provider.AppendOutput(request);
}

output::OutputOperationResult CFileBackedOutputService::ReplaceOutput(const output::OutputTextMutationRequest& request)
{
	return m_provider.ReplaceOutput(request);
}

output::OutputOperationResult CFileBackedOutputService::AppendLog(const output::OutputLogMutationRequest& request)
{
	std::lock_guard lock(m_mutex);
	const auto timestamp = m_clock();
	const auto result = m_provider.AppendLog(request);
	if (result.status != output::EOutputOperationStatus::Succeeded) return result;
	if (m_quiesced) return result;
	const auto found = m_logChannels.find(ChannelKey{ request.owner.ownerId, request.channelId });
	if (found == m_logChannels.end()) return result;
	try {
		std::string bytes;
		for (const auto& entry : request.entries) bytes += FormatOutputLogLine(timestamp, entry.level, entry.message);
		m_writer.Append(found->second, std::move(bytes));
	} catch (...) {
		// The channel already holds the entries; a lost file copy is the
		// writer's dropped-bytes case, not a failed append.
	}
	return result;
}

output::OutputOperationResult CFileBackedOutputService::Clear(const output::OutputChannelMutationRequest& request)
{
	// VS Code's Clear Output empties the view, not the log file behind it.
	return m_provider.Clear(request);
}

output::OutputOperationResult CFileBackedOutputService::Show(const output::OutputShowChannelRequest& request)
{
	return m_provider.Show(request);
}

output::OutputOperationResult CFileBackedOutputService::Hide(const output::OutputChannelMutationRequest& request)
{
	return m_provider.Hide(request);
}

output::OutputOperationResult CFileBackedOutputService::Dispose(const output::OutputChannelMutationRequest& request)
{
	std::lock_guard lock(m_mutex);
	const auto result = m_provider.Dispose(request);
	if (result.status == output::EOutputOperationStatus::Succeeded) {
		m_logChannels.erase(ChannelKey{ request.owner.ownerId, request.channelId });
	}
	return result;
}

output::OutputOperationResult CFileBackedOutputService::DisposeOwner(const output::OutputDisposeOwnerRequest& request)
{
	std::lock_guard lock(m_mutex);
	const auto result = m_provider.DisposeOwner(request);
	if (result.status == output::EOutputOperationStatus::Succeeded) {
		std::erase_if(m_logChannels, [&](const auto& channel) { return channel.first.first == request.owner.ownerId; });
	}
	return result;
}

output::OutputOperationResult CFileBackedOutputService::Stop() noexcept
{
	return m_provider.Stop();
}

output::OutputProviderHealthSnapshot CFileBackedOutputService::Health() const noexcept
{
	return m_provider.Health();
}

output::OutputServiceSnapshot CFileBackedOutputService::Snapshot() const
{
	return m_provider.Snapshot();
}

std::optional<output::OutputServiceSubscriptionId> CFileBackedOutputService::Subscribe(
	output::OutputServiceListener listener)
{
	return m_provider.Subscribe(std::move(listener));
}

void CFileBackedOutputService::Unsubscribe(output::OutputServiceSubscriptionId subscriptionId) noexcept
{
	m_provider.Unsubscribe(subscriptionId);
}

void CFileBackedOutputService::Quiesce() noexcept
{
	std::lock_guard lock(m_mutex);
	m_quiesced = true;
}

std::optional<std::filesystem::path> CFileBackedOutputService::LogFileFor(
	std::string_view ownerId, std::string_view channelId) const
{
	std::lock_guard lock(m_mutex);
	const auto found = m_logChannels.find(ChannelKey{ std::string(ownerId), std::string(channelId) });
	if (found == m_logChannels.end()) return std::nullopt;
	return m_writer.WindowFolder() / found->second;
}

} // namespace workbench::logs
