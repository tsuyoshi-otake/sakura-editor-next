/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "workbench/logs/ProblemsSnapshotFile.h"

#include "workbench/logs/LogFileSystem.h"
#include "workbench/win32/ProblemsOutputPanelProjection.h"

#include <Windows.h>

namespace workbench::logs {
namespace {

[[nodiscard]] std::string WideToUtf8(std::wstring_view value)
{
	if (value.empty()) return {};
	const int length = ::WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
		nullptr, 0, nullptr, nullptr);
	if (length <= 0) return {};
	std::string utf8(static_cast<std::size_t>(length), '\0');
	(void)::WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), utf8.data(), length,
		nullptr, nullptr);
	return utf8;
}

} // namespace

std::string EncodeProblemsSnapshotFile(const problems::ProblemsSnapshot& snapshot, const ProblemsSnapshotFileLimits& limits)
{
	const auto panel = win32::ProjectProblemsPanel(snapshot);
	std::string markers;
	std::size_t written = 0;
	bool truncated = false;
	for (const auto& entry : panel.entries) {
		if (written >= limits.maximumMarkers) {
			truncated = true;
			break;
		}
		const auto encoded = WideToUtf8(win32::SerializeProblemsViewMarker(entry));
		// Leave room for the envelope (well under 256 bytes).
		if (markers.size() + encoded.size() + 2 + 256 > limits.maximumBytes) {
			truncated = true;
			break;
		}
		markers += written == 0 ? "\n" : ",\n";
		markers += encoded;
		++written;
	}
	std::string file = "{\n\"version\": 1,\n\"revision\": " + std::to_string(snapshot.revision)
		+ ",\n\"markerCount\": " + std::to_string(panel.entries.size())
		+ ",\n\"truncated\": " + (truncated ? "true" : "false")
		+ ",\n\"markers\": [";
	file += markers;
	file += written == 0 ? "]\n}\n" : "\n]\n}\n";
	return file;
}

CProblemsSnapshotFileWriter::CProblemsSnapshotFileWriter(problems::MarkerService& markers, std::filesystem::path file,
	std::shared_ptr<ILogFileSystem> files, std::chrono::milliseconds coalesce, ProblemsSnapshotFileLimits limits,
	FailingChanged onFailingChanged)
	: m_markers(markers)
	, m_file(std::move(file))
	, m_files(std::move(files))
	, m_coalesce(coalesce)
	, m_limits(limits)
	, m_onFailingChanged(std::move(onFailingChanged))
	, m_state(std::make_shared<State>())
{
}

CProblemsSnapshotFileWriter::~CProblemsSnapshotFileWriter()
{
	Stop();
}

bool CProblemsSnapshotFileWriter::Start() noexcept
{
	if (!m_files) return false;
	{
		std::lock_guard lock(m_state->mutex);
		if (m_worker.joinable() || m_state->stopping) return m_worker.joinable();
		m_state->dirty = true;
	}
	try {
		const auto subscribed = m_markers.Subscribe([state = m_state](const problems::MarkerChange&) {
			{
				std::lock_guard lock(state->mutex);
				state->dirty = true;
			}
			state->wake.notify_one();
		});
		if (subscribed.status != problems::EMarkerSubscriptionStatus::Subscribed || !subscribed.subscriptionId) {
			return false;
		}
		m_subscription = subscribed.subscriptionId;
		m_worker = std::thread([this] { WorkerMain(); });
		return true;
	} catch (...) {
		if (m_subscription) {
			m_markers.Unsubscribe(*m_subscription);
			m_subscription.reset();
		}
		return false;
	}
}

void CProblemsSnapshotFileWriter::Stop() noexcept
{
	if (m_subscription) {
		m_markers.Unsubscribe(*m_subscription);
		m_subscription.reset();
	}
	{
		std::lock_guard lock(m_state->mutex);
		m_state->stopping = true;
	}
	m_state->wake.notify_all();
	if (m_worker.joinable() && m_worker.get_id() != std::this_thread::get_id()) m_worker.join();
}

std::uint64_t CProblemsSnapshotFileWriter::WrittenRevision() const noexcept
{
	std::lock_guard lock(m_state->mutex);
	return m_writtenRevision;
}

void CProblemsSnapshotFileWriter::WorkerMain() noexcept
{
	auto& state = *m_state;
	std::chrono::milliseconds retryDelay{};
	std::unique_lock lock(state.mutex);
	for (;;) {
		state.wake.wait(lock, [&] { return state.stopping || state.dirty; });
		if (state.stopping) return;
		// Let a burst of changes settle into one write; Stop cuts the wait short.
		if (state.wake.wait_for(lock, m_coalesce, [&] { return state.stopping; })) return;
		state.dirty = false;
		lock.unlock();
		bool written = false;
		bool serviceStopped = false;
		std::uint64_t revision = 0;
		try {
			const auto snapshot = m_markers.Snapshot();
			revision = snapshot.revision;
			serviceStopped = snapshot.stopped;
			if (!serviceStopped) {
				// The folder may have been removed while the window runs.
				(void)m_files->CreateDirectories(m_file.parent_path());
				written = m_files->ReplaceContent(m_file, EncodeProblemsSnapshotFile(snapshot, m_limits));
			}
		} catch (...) {
			written = false;
		}
		lock.lock();
		// A stopped service has nothing current to write; the last file stays.
		if (serviceStopped) continue;
		if (written) m_writtenRevision = revision;
		const bool changed = m_failing == written;
		m_failing = !written;
		if (changed && m_onFailingChanged) {
			lock.unlock();
			try {
				m_onFailingChanged(!written);
			} catch (...) {
			}
			lock.lock();
		}
		if (written) {
			retryDelay = {};
			continue;
		}
		// Retry the same content on its own; a marker change meanwhile is
		// folded into the retry, and Stop cuts the wait short.
		retryDelay = retryDelay.count() == 0 ? std::chrono::milliseconds(500)
											  : (std::min)(retryDelay * 2, kMaximumRetryDelay);
		if (state.wake.wait_for(lock, retryDelay, [&] { return state.stopping; })) return;
		state.dirty = true;
	}
}

} // namespace workbench::logs
