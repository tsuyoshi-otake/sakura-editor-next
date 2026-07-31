/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/output/OutputService.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace workbench::output {
namespace {

constexpr std::size_t kMaximumStableIdBytes = 160;
constexpr std::size_t kMaximumLabelBytes = 512;
constexpr std::size_t kMaximumMetadataBytes = 512;

bool IsValidUtf8(const std::string_view value, const bool permitControls) noexcept
{
	for (std::size_t index = 0; index < value.size();) {
		const auto first = static_cast<unsigned char>(value[index]);
		if (first < 0x80) {
			if ((!permitControls && (first < 0x20 || first == 0x7f)) || first == 0) return false;
			++index;
			continue;
		}
		std::size_t continuationCount{};
		std::uint32_t codePoint{};
		if (first >= 0xc2 && first <= 0xdf) { continuationCount = 1; codePoint = first & 0x1f; }
		else if (first >= 0xe0 && first <= 0xef) { continuationCount = 2; codePoint = first & 0x0f; }
		else if (first >= 0xf0 && first <= 0xf4) { continuationCount = 3; codePoint = first & 0x07; }
		else return false;
		if (index + continuationCount >= value.size()) return false;
		for (std::size_t continuation = 1; continuation <= continuationCount; ++continuation) {
			const auto next = static_cast<unsigned char>(value[index + continuation]);
			if ((next & 0xc0) != 0x80) return false;
			codePoint = (codePoint << 6) | (next & 0x3f);
		}
		const auto minimum = continuationCount == 1 ? 0x80U : continuationCount == 2 ? 0x800U : 0x10000U;
		if (codePoint < minimum || codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff)) return false;
		if (!permitControls && (codePoint >= 0x80 && codePoint <= 0x9f)) return false;
		index += continuationCount + 1;
	}
	return true;
}

bool IsValidBoundedText(const std::string_view value, const std::size_t maximumBytes) noexcept
{
	return value.size() <= maximumBytes && IsValidUtf8(value, true);
}

std::uint64_t CharacterCount(const std::string_view value) noexcept
{
	std::uint64_t count{};
	for (const unsigned char valueByte : value) {
		if ((valueByte & 0xc0) != 0x80) ++count;
	}
	return count;
}

std::size_t LeadingBoundary(const std::string_view value, std::size_t byteCount) noexcept
{
	byteCount = std::min(byteCount, value.size());
	while (byteCount < value.size() && (static_cast<unsigned char>(value[byteCount]) & 0xc0) == 0x80) ++byteCount;
	return byteCount;
}

void KeepSuffix(std::string& value, const std::size_t maximumBytes, std::uint64_t& droppedCharacters)
{
	if (value.size() <= maximumBytes) return;
	const auto removed = LeadingBoundary(value, value.size() - maximumBytes);
	droppedCharacters += CharacterCount(std::string_view(value).substr(0, removed));
	value.erase(0, removed);
}

std::string LogLevelText(const EOutputLogLevel level)
{
	switch (level) {
	case EOutputLogLevel::Trace: return "Trace";
	case EOutputLogLevel::Debug: return "Debug";
	case EOutputLogLevel::Info: return "Info";
	case EOutputLogLevel::Warning: return "Warning";
	case EOutputLogLevel::Error: return "Error";
	}
	return "Info";
}

std::string RenderLogEntry(const OutputLogEntry& entry)
{
	std::string rendered = "[" + LogLevelText(entry.level) + "] ";
	if (entry.source) {
		rendered += *entry.source;
		rendered += ": ";
	}
	rendered += entry.message;
	rendered += '\n';
	return rendered;
}

void AppendToken(std::string& target, const std::string_view value)
{
	target.append(std::to_string(value.size()));
	target.push_back(':');
	target.append(value);
	target.push_back(';');
}

void AppendOwner(std::string& target, const OutputOwner& owner)
{
	AppendToken(target, owner.ownerId);
	AppendToken(target, std::to_string(owner.generation));
}

void AppendOperation(std::string& target, const OutputOperation& operation)
{
	AppendToken(target, operation.operationId);
	AppendToken(target, operation.expectedRevision ? std::to_string(*operation.expectedRevision) : "-");
}

void AppendMetadata(std::string& target, const OutputChannelMetadata& metadata)
{
	AppendToken(target, metadata.languageId.value_or(""));
	AppendToken(target, metadata.source.value_or(""));
}

bool IsValidMetadataValue(const std::optional<std::string>& value) noexcept
{
	return !value || (!value->empty() && value->size() <= kMaximumMetadataBytes && IsValidUtf8(*value, false));
}

bool IsValidMetadata(const OutputChannelMetadata& metadata) noexcept
{
	return IsValidMetadataValue(metadata.languageId) && IsValidMetadataValue(metadata.source);
}

bool IsSameOwner(const OutputOwner& left, const OutputOwner& right) noexcept
{
	return left == right;
}

} // namespace

struct OutputService::Impl final {
	struct Channel final {
		std::string id;
		std::string label;
		OutputOwner owner;
		EOutputChannelKind kind{ EOutputChannelKind::Output };
		OutputChannelMetadata metadata;
		bool visible{};
		bool lastShowPreservedFocus{};
		std::uint64_t droppedCharacterCount{};
		std::string text;
		std::deque<OutputLogEntry> logEntries;
		std::string projectedText;
	};
	struct CompletedOperation final {
		std::string fingerprint;
		OutputOperationResult result;
	};
	struct OwnerGeneration final {
		std::uint64_t generation{};
		bool disposed{};
	};
	struct PendingNotification final {
		OutputServiceChange change;
		std::vector<OutputServiceSubscriptionId> subscriberIds;
	};

	mutable std::mutex mutex;
	OutputServiceLimits limits;
	std::map<std::string, Channel, std::less<>> channels;
	// Tombstones retain the most recently disposed generation, fencing late work from an old extension host.
	std::map<std::string, OwnerGeneration, std::less<>> activeOwnerGenerations;
	std::optional<std::string> activeChannelId;
	std::map<std::string, CompletedOperation, std::less<>> completedOperations;
	std::deque<std::string> completedOperationOrder;
	std::map<OutputServiceSubscriptionId, OutputServiceListener> subscriptions;
	std::deque<PendingNotification> pendingNotifications;
	std::uint64_t revision{ 1 };
	std::uint64_t droppedNotificationCount{};
	OutputServiceSubscriptionId nextSubscriptionId{ 1 };
	bool drainingNotifications{};
	std::thread::id notificationDispatchThreadId;
	std::condition_variable notificationDrained;
	bool stopped{};

	explicit Impl(OutputServiceLimits initialLimits)
		: limits(std::move(initialLimits))
	{
		// Invalid caller limits fail closed to safe nonzero defaults; operations still validate their own payloads.
		if (limits.maximumOwners == 0) limits.maximumOwners = 1;
		if (limits.maximumChannels == 0) limits.maximumChannels = 1;
		if (limits.maximumTextBytesPerChannel == 0) limits.maximumTextBytesPerChannel = 1;
		if (limits.maximumPayloadBytes == 0) limits.maximumPayloadBytes = 1;
		if (limits.maximumLogEntriesPerChannel == 0) limits.maximumLogEntriesPerChannel = 1;
		if (limits.maximumSubscriptions == 0) limits.maximumSubscriptions = 1;
		if (limits.maximumRememberedOperations == 0) limits.maximumRememberedOperations = 1;
		if (limits.maximumPendingNotifications == 0) limits.maximumPendingNotifications = 1;
	}

	[[nodiscard]] OutputOperationResult Current(const EOutputOperationStatus status, const EOutputOperationReason reason) const noexcept
	{
		return { status, reason, revision };
	}

	void RememberLocked(std::string operationId, std::string fingerprint, const OutputOperationResult& result)
	{
		if (completedOperations.size() == limits.maximumRememberedOperations) {
			completedOperations.erase(completedOperationOrder.front());
			completedOperationOrder.pop_front();
		}
		completedOperationOrder.push_back(operationId);
		completedOperations.emplace(std::move(operationId), CompletedOperation{ std::move(fingerprint), result });
	}

	[[nodiscard]] std::optional<OutputOperationResult> ReplayOrConflictLocked(const OutputOperation& operation,
		const std::string& fingerprint) const
	{
		const auto found = completedOperations.find(operation.operationId);
		if (found == completedOperations.end()) return std::nullopt;
		if (found->second.fingerprint != fingerprint) return Current(EOutputOperationStatus::Conflict, EOutputOperationReason::OperationIdConflict);
		auto result = found->second.result;
		result.status = EOutputOperationStatus::Replayed;
		return result;
	}

	static void SaturatingIncrement(std::uint64_t& value) noexcept
	{
		if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
	}

	static void SaturatingAdd(std::uint64_t& value, const std::uint64_t amount) noexcept
	{
		const auto maximum = std::numeric_limits<std::uint64_t>::max();
		value = amount > maximum - value ? maximum : value + amount;
	}

	[[nodiscard]] bool QueueNotificationLocked(const EOutputChangeKind kind, const std::optional<std::string>& channelId) noexcept
	{
		try {
			if (pendingNotifications.size() >= limits.maximumPendingNotifications) {
				// Delivery is advisory. A bounded queue prevents a reentrant listener
				// from turning committed Output mutations into unbounded memory growth.
				SaturatingIncrement(droppedNotificationCount);
				return false;
			}
			PendingNotification pending{ .change = { .revision = revision, .kind = kind, .channelId = channelId, .activeChannelId = activeChannelId } };
			pending.subscriberIds.reserve(subscriptions.size());
			for (const auto& [id, ignored] : subscriptions) {
				(void)ignored;
				pending.subscriberIds.push_back(id);
			}
			pendingNotifications.push_back(std::move(pending));
			if (drainingNotifications) return false;
			drainingNotifications = true;
			return true;
		} catch (...) {
			SaturatingIncrement(droppedNotificationCount);
			return false;
		}
	}

	[[nodiscard]] bool WaitForNotificationDrain() noexcept
	{
		std::unique_lock lock(mutex);
		if (drainingNotifications && notificationDispatchThreadId == std::this_thread::get_id()) return true;
		try { notificationDrained.wait(lock, [this] { return !drainingNotifications; }); }
		catch (...) { return drainingNotifications; }
		return false;
	}

	void DrainNotifications() noexcept
	{
		{
			std::lock_guard lock(mutex);
			if (!drainingNotifications || notificationDispatchThreadId != std::thread::id{}) return;
			notificationDispatchThreadId = std::this_thread::get_id();
		}
		for (;;) {
			PendingNotification pending;
			{
				std::lock_guard lock(mutex);
				if (pendingNotifications.empty()) {
					drainingNotifications = false;
					notificationDispatchThreadId = {};
					notificationDrained.notify_all();
					return;
				}
				pending = std::move(pendingNotifications.front());
				pendingNotifications.pop_front();
			}
			for (const auto id : pending.subscriberIds) {
				OutputServiceListener listener;
				{
					std::lock_guard lock(mutex);
					const auto found = subscriptions.find(id);
					if (found != subscriptions.end()) listener = found->second;
				}
				if (!listener) continue;
				try {
					listener(pending.change);
				} catch (...) {
				}
			}
		}
	}

	void RebuildLogProjection(Channel& channel)
	{
		std::size_t projectedBytes{};
		for (const auto& entry : channel.logEntries) projectedBytes += RenderLogEntry(entry).size();
		while (projectedBytes > limits.maximumTextBytesPerChannel && !channel.logEntries.empty()) {
			const auto rendered = RenderLogEntry(channel.logEntries.front());
			SaturatingAdd(channel.droppedCharacterCount, CharacterCount(rendered));
			projectedBytes -= rendered.size();
			channel.logEntries.pop_front();
		}
		channel.projectedText.clear();
		channel.projectedText.reserve(projectedBytes);
		for (const auto& entry : channel.logEntries) channel.projectedText += RenderLogEntry(entry);
	}

	void SelectFallbackLocked()
	{
		if (activeChannelId && channels.contains(*activeChannelId)) return;
		const auto visible = std::find_if(channels.begin(), channels.end(), [](const auto& value) { return value.second.visible; });
		if (visible != channels.end()) {
			activeChannelId = visible->first;
		} else if (!channels.empty()) {
			activeChannelId = channels.begin()->first;
		} else {
			activeChannelId.reset();
		}
	}
};

bool OutputOwner::IsValid() const noexcept
{
	return generation != 0 && OutputService::IsValidStableId(ownerId);
}

OutputService::OutputService(OutputServiceLimits limits)
	: m_impl(new Impl(std::move(limits)))
{
}

OutputService::~OutputService()
{
	(void)Stop();
	delete m_impl;
}

bool OutputService::IsValidStableId(const std::string_view value) noexcept
{
	if (value.empty() || value.size() > kMaximumStableIdBytes || !IsValidUtf8(value, false)) return false;
	return std::none_of(value.begin(), value.end(), [](const unsigned char character) {
		return character <= 0x20 || character == 0x7f;
	});
}

bool OutputService::IsValidOperationId(const std::string_view value) noexcept
{
	return IsValidStableId(value);
}

namespace {

template <typename ImplType, typename Request, typename Fingerprint, typename Mutate>
OutputOperationResult Apply(ImplType& impl, const Request& request, Fingerprint makeFingerprint, Mutate mutate)
{
	bool drain{};
	OutputOperationResult result;
	{
		std::lock_guard lock(impl.mutex);
		if (impl.stopped) return impl.Current(EOutputOperationStatus::Stopped, EOutputOperationReason::None);
		if (!OutputService::IsValidOperationId(request.operation.operationId)) {
			return impl.Current(EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidOperationId);
		}
		const auto fingerprint = makeFingerprint(request);
		if (const auto replay = impl.ReplayOrConflictLocked(request.operation, fingerprint)) return *replay;
		if (request.operation.expectedRevision && *request.operation.expectedRevision != impl.revision) {
			return impl.Current(EOutputOperationStatus::StaleRevision, EOutputOperationReason::ExpectedRevisionMismatch);
		}
		result = mutate(impl, fingerprint, drain);
	}
	if (drain) impl.DrainNotifications();
	return result;
}

template <typename Channel>
bool IsOwnedChannel(const Channel& channel, const OutputOwner& owner) noexcept
{
	return IsSameOwner(channel.owner, owner);
}

template <typename ImplType, typename Channel>
OutputOperationResult ValidateOwnedChannel(ImplType& impl, const OutputOwner& owner, const std::string& channelId,
	const EOutputChannelKind expectedKind, Channel*& channel)
{
	if (!owner.IsValid()) return impl.Current(EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidOwner);
	if (!OutputService::IsValidStableId(channelId)) return impl.Current(EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidChannelId);
	const auto found = impl.channels.find(channelId);
	if (found == impl.channels.end()) return impl.Current(EOutputOperationStatus::NotApplicable, EOutputOperationReason::ChannelNotFound);
	if (!IsOwnedChannel(found->second, owner)) return impl.Current(EOutputOperationStatus::Conflict, EOutputOperationReason::OwnerGenerationConflict);
	if (found->second.kind != expectedKind) return impl.Current(EOutputOperationStatus::Rejected, EOutputOperationReason::ChannelKindMismatch);
	channel = &found->second;
	return impl.Current(EOutputOperationStatus::Succeeded, EOutputOperationReason::None);
}

std::string CreateFingerprint(const OutputCreateChannelRequest& request)
{
	std::string fingerprint("create;");
	AppendOperation(fingerprint, request.operation); AppendOwner(fingerprint, request.owner); AppendToken(fingerprint, request.channelId);
	AppendToken(fingerprint, request.label); AppendToken(fingerprint, std::to_string(static_cast<unsigned>(request.kind))); AppendMetadata(fingerprint, request.metadata);
	return fingerprint;
}

std::string TextFingerprint(const char* verb, const OutputTextMutationRequest& request)
{
	std::string fingerprint(verb); fingerprint.push_back(';'); AppendOperation(fingerprint, request.operation); AppendOwner(fingerprint, request.owner);
	AppendToken(fingerprint, request.channelId); AppendToken(fingerprint, request.text); return fingerprint;
}

std::string ChannelFingerprint(const char* verb, const OutputChannelMutationRequest& request)
{
	std::string fingerprint(verb); fingerprint.push_back(';'); AppendOperation(fingerprint, request.operation); AppendOwner(fingerprint, request.owner);
	AppendToken(fingerprint, request.channelId); return fingerprint;
}

} // namespace

OutputOperationResult OutputService::CreateChannel(const OutputCreateChannelRequest& request)
{
	return Apply(*m_impl, request, CreateFingerprint, [&](Impl& impl, const std::string& fingerprint, bool& drain) {
		if (!request.owner.IsValid()) return impl.Current(EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidOwner);
		if (!IsValidStableId(request.channelId)) return impl.Current(EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidChannelId);
		if (request.label.empty() || request.label.size() > kMaximumLabelBytes || !IsValidUtf8(request.label, false)) return impl.Current(EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidLabel);
		if (!IsValidMetadata(request.metadata)) return impl.Current(EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidMetadata);
		const auto existingGeneration = impl.activeOwnerGenerations.find(request.owner.ownerId);
		if (existingGeneration == impl.activeOwnerGenerations.end()
			&& impl.activeOwnerGenerations.size() >= impl.limits.maximumOwners) {
			return impl.Current(EOutputOperationStatus::Rejected, EOutputOperationReason::OwnerLimitExceeded);
		}
		bool adoptsNewGeneration = false;
		std::size_t replacedOwnerChannels = 0;
		if (existingGeneration != impl.activeOwnerGenerations.end()) {
			if (request.owner.generation < existingGeneration->second.generation
				|| (request.owner.generation == existingGeneration->second.generation && existingGeneration->second.disposed)) {
				return impl.Current(EOutputOperationStatus::Conflict, EOutputOperationReason::OwnerGenerationConflict);
			}
			adoptsNewGeneration = request.owner.generation > existingGeneration->second.generation;
			if (adoptsNewGeneration) {
				replacedOwnerChannels = static_cast<std::size_t>(std::count_if(
					impl.channels.begin(), impl.channels.end(), [&request](const auto& entry) {
						return entry.second.owner.ownerId == request.owner.ownerId;
					}));
			}
		}
		const auto existingChannel = impl.channels.find(request.channelId);
		if (existingChannel != impl.channels.end()
			&& !(adoptsNewGeneration && existingChannel->second.owner.ownerId == request.owner.ownerId)) {
			return impl.Current(EOutputOperationStatus::Conflict, EOutputOperationReason::InvalidChannelId);
		}
		if (impl.channels.size() - replacedOwnerChannels >= impl.limits.maximumChannels) {
			return impl.Current(EOutputOperationStatus::Rejected, EOutputOperationReason::ChannelLimitExceeded);
		}
		if (impl.revision == std::numeric_limits<std::uint64_t>::max()) return impl.Current(EOutputOperationStatus::RevisionExhausted, EOutputOperationReason::None);
		if (adoptsNewGeneration) {
			for (auto channel = impl.channels.begin(); channel != impl.channels.end();) {
				if (channel->second.owner.ownerId == request.owner.ownerId) channel = impl.channels.erase(channel);
				else ++channel;
			}
			impl.activeChannelId.reset();
			impl.SelectFallbackLocked();
		}
		impl.activeOwnerGenerations.insert_or_assign(request.owner.ownerId, Impl::OwnerGeneration{ .generation = request.owner.generation });
		impl.channels.emplace(request.channelId, Impl::Channel{ .id = request.channelId, .label = request.label, .owner = request.owner, .kind = request.kind, .metadata = request.metadata });
		impl.SelectFallbackLocked();
		++impl.revision;
		const auto result = impl.Current(EOutputOperationStatus::Succeeded, EOutputOperationReason::None);
		impl.RememberLocked(request.operation.operationId, fingerprint, result);
		drain = impl.QueueNotificationLocked(EOutputChangeKind::ChannelCreated, request.channelId);
		return result;
	});
}

OutputOperationResult OutputService::AppendOutput(const OutputTextMutationRequest& request)
{
	return Apply(*m_impl, request, [](const auto& value) { return TextFingerprint("append-output", value); }, [&](Impl& impl, const std::string& fingerprint, bool& drain) {
		if (!IsValidBoundedText(request.text, impl.limits.maximumPayloadBytes)) return impl.Current(EOutputOperationStatus::Rejected, request.text.size() > impl.limits.maximumPayloadBytes ? EOutputOperationReason::PayloadLimitExceeded : EOutputOperationReason::InvalidPayload);
		Impl::Channel* channel{}; const auto valid = ValidateOwnedChannel(impl, request.owner, request.channelId, EOutputChannelKind::Output, channel); if (!valid.Succeeded()) return valid;
		if (request.text.empty()) return impl.Current(EOutputOperationStatus::NotApplicable, EOutputOperationReason::None);
		if (impl.revision == std::numeric_limits<std::uint64_t>::max()) return impl.Current(EOutputOperationStatus::RevisionExhausted, EOutputOperationReason::None);
		channel->text += request.text; KeepSuffix(channel->text, impl.limits.maximumTextBytesPerChannel, channel->droppedCharacterCount); channel->projectedText = channel->text;
		++impl.revision; const auto result = impl.Current(EOutputOperationStatus::Succeeded, EOutputOperationReason::None); impl.RememberLocked(request.operation.operationId, fingerprint, result); drain = impl.QueueNotificationLocked(EOutputChangeKind::ContentAppended, request.channelId); return result;
	});
}

OutputOperationResult OutputService::ReplaceOutput(const OutputTextMutationRequest& request)
{
	return Apply(*m_impl, request, [](const auto& value) { return TextFingerprint("replace-output", value); }, [&](Impl& impl, const std::string& fingerprint, bool& drain) {
		if (!IsValidBoundedText(request.text, impl.limits.maximumPayloadBytes)) return impl.Current(EOutputOperationStatus::Rejected, request.text.size() > impl.limits.maximumPayloadBytes ? EOutputOperationReason::PayloadLimitExceeded : EOutputOperationReason::InvalidPayload);
		Impl::Channel* channel{}; const auto valid = ValidateOwnedChannel(impl, request.owner, request.channelId, EOutputChannelKind::Output, channel); if (!valid.Succeeded()) return valid;
		if (channel->text == request.text && channel->droppedCharacterCount == 0) return impl.Current(EOutputOperationStatus::NotApplicable, EOutputOperationReason::None);
		if (impl.revision == std::numeric_limits<std::uint64_t>::max()) return impl.Current(EOutputOperationStatus::RevisionExhausted, EOutputOperationReason::None);
		channel->text = request.text; channel->droppedCharacterCount = 0; KeepSuffix(channel->text, impl.limits.maximumTextBytesPerChannel, channel->droppedCharacterCount); channel->projectedText = channel->text;
		++impl.revision; const auto result = impl.Current(EOutputOperationStatus::Succeeded, EOutputOperationReason::None); impl.RememberLocked(request.operation.operationId, fingerprint, result); drain = impl.QueueNotificationLocked(EOutputChangeKind::ContentReplaced, request.channelId); return result;
	});
}

OutputOperationResult OutputService::AppendLog(const OutputLogMutationRequest& request)
{
	return Apply(*m_impl, request, [](const auto& value) {
		std::string fingerprint("append-log;"); AppendOperation(fingerprint, value.operation); AppendOwner(fingerprint, value.owner); AppendToken(fingerprint, value.channelId); for (const auto& entry : value.entries) { AppendToken(fingerprint, std::to_string(static_cast<unsigned>(entry.level))); AppendToken(fingerprint, entry.message); AppendToken(fingerprint, entry.source.value_or("")); } return fingerprint;
	}, [&](Impl& impl, const std::string& fingerprint, bool& drain) {
		if (request.entries.empty()) return impl.Current(EOutputOperationStatus::NotApplicable, EOutputOperationReason::None);
		if (request.entries.size() > impl.limits.maximumLogEntriesPerChannel) return impl.Current(EOutputOperationStatus::Rejected, EOutputOperationReason::LogEntryLimitExceeded);
		std::size_t payloadBytes{};
		for (const auto& entry : request.entries) {
			payloadBytes += entry.message.size() + (entry.source ? entry.source->size() : 0);
			if (!IsValidBoundedText(entry.message, impl.limits.maximumPayloadBytes) || entry.message.empty() || !IsValidMetadataValue(entry.source)) return impl.Current(EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidPayload);
			if (payloadBytes > impl.limits.maximumPayloadBytes) return impl.Current(EOutputOperationStatus::Rejected, EOutputOperationReason::PayloadLimitExceeded);
		}
		Impl::Channel* channel{}; const auto valid = ValidateOwnedChannel(impl, request.owner, request.channelId, EOutputChannelKind::Log, channel); if (!valid.Succeeded()) return valid;
		if (impl.revision == std::numeric_limits<std::uint64_t>::max()) return impl.Current(EOutputOperationStatus::RevisionExhausted, EOutputOperationReason::None);
		channel->logEntries.insert(channel->logEntries.end(), request.entries.begin(), request.entries.end());
		while (channel->logEntries.size() > impl.limits.maximumLogEntriesPerChannel) { channel->droppedCharacterCount += CharacterCount(RenderLogEntry(channel->logEntries.front())); channel->logEntries.pop_front(); }
		impl.RebuildLogProjection(*channel);
		++impl.revision; const auto result = impl.Current(EOutputOperationStatus::Succeeded, EOutputOperationReason::None); impl.RememberLocked(request.operation.operationId, fingerprint, result); drain = impl.QueueNotificationLocked(EOutputChangeKind::ContentAppended, request.channelId); return result;
	});
}

OutputOperationResult OutputService::Clear(const OutputChannelMutationRequest& request)
{
	return Apply(*m_impl, request, [](const auto& value) { return ChannelFingerprint("clear", value); }, [&](Impl& impl, const std::string& fingerprint, bool& drain) {
		if (!request.owner.IsValid()) return impl.Current(EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidOwner);
		if (!IsValidStableId(request.channelId)) return impl.Current(EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidChannelId);
		const auto found = impl.channels.find(request.channelId); if (found == impl.channels.end()) return impl.Current(EOutputOperationStatus::NotApplicable, EOutputOperationReason::ChannelNotFound); if (!IsOwnedChannel(found->second, request.owner)) return impl.Current(EOutputOperationStatus::Conflict, EOutputOperationReason::OwnerGenerationConflict);
		auto& channel = found->second; if (channel.text.empty() && channel.logEntries.empty() && channel.droppedCharacterCount == 0) return impl.Current(EOutputOperationStatus::NotApplicable, EOutputOperationReason::None);
		if (impl.revision == std::numeric_limits<std::uint64_t>::max()) return impl.Current(EOutputOperationStatus::RevisionExhausted, EOutputOperationReason::None);
		channel.text.clear(); channel.logEntries.clear(); channel.projectedText.clear(); channel.droppedCharacterCount = 0;
		++impl.revision; const auto result = impl.Current(EOutputOperationStatus::Succeeded, EOutputOperationReason::None); impl.RememberLocked(request.operation.operationId, fingerprint, result); drain = impl.QueueNotificationLocked(EOutputChangeKind::ContentCleared, request.channelId); return result;
	});
}

OutputOperationResult OutputService::Show(const OutputShowChannelRequest& request)
{
	return Apply(*m_impl, request, [](const auto& value) { std::string fingerprint("show;"); AppendOperation(fingerprint, value.operation); AppendOwner(fingerprint, value.owner); AppendToken(fingerprint, value.channelId); AppendToken(fingerprint, value.preserveFocus ? "1" : "0"); return fingerprint; }, [&](Impl& impl, const std::string& fingerprint, bool& drain) {
		if (!request.owner.IsValid()) return impl.Current(EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidOwner); if (!IsValidStableId(request.channelId)) return impl.Current(EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidChannelId);
		const auto found = impl.channels.find(request.channelId); if (found == impl.channels.end()) return impl.Current(EOutputOperationStatus::NotApplicable, EOutputOperationReason::ChannelNotFound); if (!IsOwnedChannel(found->second, request.owner)) return impl.Current(EOutputOperationStatus::Conflict, EOutputOperationReason::OwnerGenerationConflict);
		if (impl.revision == std::numeric_limits<std::uint64_t>::max()) return impl.Current(EOutputOperationStatus::RevisionExhausted, EOutputOperationReason::None); found->second.visible = true; found->second.lastShowPreservedFocus = request.preserveFocus; impl.activeChannelId = request.channelId;
		++impl.revision; const auto result = impl.Current(EOutputOperationStatus::Succeeded, EOutputOperationReason::None); impl.RememberLocked(request.operation.operationId, fingerprint, result); drain = impl.QueueNotificationLocked(EOutputChangeKind::ChannelShown, request.channelId); return result;
	});
}

OutputOperationResult OutputService::Hide(const OutputChannelMutationRequest& request)
{
	return Apply(*m_impl, request, [](const auto& value) { return ChannelFingerprint("hide", value); }, [&](Impl& impl, const std::string& fingerprint, bool& drain) {
		if (!request.owner.IsValid()) return impl.Current(EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidOwner); if (!IsValidStableId(request.channelId)) return impl.Current(EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidChannelId);
		const auto found = impl.channels.find(request.channelId); if (found == impl.channels.end()) return impl.Current(EOutputOperationStatus::NotApplicable, EOutputOperationReason::ChannelNotFound); if (!IsOwnedChannel(found->second, request.owner)) return impl.Current(EOutputOperationStatus::Conflict, EOutputOperationReason::OwnerGenerationConflict);
		if (!found->second.visible) return impl.Current(EOutputOperationStatus::NotApplicable, EOutputOperationReason::None); if (impl.revision == std::numeric_limits<std::uint64_t>::max()) return impl.Current(EOutputOperationStatus::RevisionExhausted, EOutputOperationReason::None);
		found->second.visible = false; if (impl.activeChannelId && *impl.activeChannelId == request.channelId) { impl.activeChannelId.reset(); impl.SelectFallbackLocked(); }
		++impl.revision; const auto result = impl.Current(EOutputOperationStatus::Succeeded, EOutputOperationReason::None); impl.RememberLocked(request.operation.operationId, fingerprint, result); drain = impl.QueueNotificationLocked(EOutputChangeKind::ChannelHidden, request.channelId); return result;
	});
}

OutputOperationResult OutputService::Dispose(const OutputChannelMutationRequest& request)
{
	return Apply(*m_impl, request, [](const auto& value) { return ChannelFingerprint("dispose", value); }, [&](Impl& impl, const std::string& fingerprint, bool& drain) {
		if (!request.owner.IsValid()) return impl.Current(EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidOwner); if (!IsValidStableId(request.channelId)) return impl.Current(EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidChannelId);
		const auto found = impl.channels.find(request.channelId); if (found == impl.channels.end()) return impl.Current(EOutputOperationStatus::NotApplicable, EOutputOperationReason::ChannelNotFound); if (!IsOwnedChannel(found->second, request.owner)) return impl.Current(EOutputOperationStatus::Conflict, EOutputOperationReason::OwnerGenerationConflict); if (impl.revision == std::numeric_limits<std::uint64_t>::max()) return impl.Current(EOutputOperationStatus::RevisionExhausted, EOutputOperationReason::None);
		impl.channels.erase(found); if (impl.activeChannelId && *impl.activeChannelId == request.channelId) { impl.activeChannelId.reset(); impl.SelectFallbackLocked(); }
		++impl.revision; const auto result = impl.Current(EOutputOperationStatus::Succeeded, EOutputOperationReason::None); impl.RememberLocked(request.operation.operationId, fingerprint, result); drain = impl.QueueNotificationLocked(EOutputChangeKind::ChannelDisposed, request.channelId); return result;
	});
}

OutputOperationResult OutputService::DisposeOwner(const OutputDisposeOwnerRequest& request)
{
	return Apply(*m_impl, request, [](const auto& value) { std::string fingerprint("dispose-owner;"); AppendOperation(fingerprint, value.operation); AppendOwner(fingerprint, value.owner); return fingerprint; }, [&](Impl& impl, const std::string& fingerprint, bool& drain) {
		if (!request.owner.IsValid()) return impl.Current(EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidOwner); const auto active = impl.activeOwnerGenerations.find(request.owner.ownerId); if (active == impl.activeOwnerGenerations.end() || active->second.disposed) return impl.Current(EOutputOperationStatus::NotApplicable, EOutputOperationReason::ChannelNotFound); if (active->second.generation != request.owner.generation) return impl.Current(EOutputOperationStatus::Conflict, EOutputOperationReason::OwnerGenerationConflict); if (impl.revision == std::numeric_limits<std::uint64_t>::max()) return impl.Current(EOutputOperationStatus::RevisionExhausted, EOutputOperationReason::None);
		for (auto iterator = impl.channels.begin(); iterator != impl.channels.end();) { if (IsSameOwner(iterator->second.owner, request.owner)) iterator = impl.channels.erase(iterator); else ++iterator; }
		active->second.disposed = true; impl.activeChannelId.reset(); impl.SelectFallbackLocked();
		++impl.revision; const auto result = impl.Current(EOutputOperationStatus::Succeeded, EOutputOperationReason::None); impl.RememberLocked(request.operation.operationId, fingerprint, result); drain = impl.QueueNotificationLocked(EOutputChangeKind::OwnerDisposed, std::nullopt); return result;
	});
}

OutputOperationResult OutputService::Stop() noexcept
{
	OutputOperationResult result;
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->stopped) {
			result = m_impl->Current(EOutputOperationStatus::Succeeded, EOutputOperationReason::None);
		} else {
			m_impl->channels.clear(); m_impl->activeOwnerGenerations.clear(); m_impl->activeChannelId.reset(); m_impl->completedOperations.clear(); m_impl->completedOperationOrder.clear(); m_impl->subscriptions.clear(); m_impl->pendingNotifications.clear(); m_impl->stopped = true;
			// At the terminal counter value we cannot publish another revision, but Stop still must close the service.
			if (m_impl->revision != std::numeric_limits<std::uint64_t>::max()) ++m_impl->revision;
			result = m_impl->Current(EOutputOperationStatus::Succeeded, EOutputOperationReason::None);
		}
	}
	result.callbackDrainDeferred = m_impl->WaitForNotificationDrain();
	return result;
}

OutputServiceSnapshot OutputService::Snapshot() const
{
	std::lock_guard lock(m_impl->mutex);
	OutputServiceSnapshot snapshot{ .revision = m_impl->revision, .stopped = m_impl->stopped, .droppedNotificationCount = m_impl->droppedNotificationCount, .activeChannelId = m_impl->activeChannelId };
	snapshot.channels.reserve(m_impl->channels.size());
	for (const auto& [id, channel] : m_impl->channels) {
		(void)id;
		snapshot.channels.push_back({ .channelId = channel.id, .label = channel.label, .owner = channel.owner, .kind = channel.kind, .metadata = channel.metadata, .visible = channel.visible, .lastShowPreservedFocus = channel.lastShowPreservedFocus, .droppedCharacterCount = channel.droppedCharacterCount, .text = channel.text, .logEntries = std::vector<OutputLogEntry>(channel.logEntries.begin(), channel.logEntries.end()), .projectedText = channel.projectedText });
	}
	return snapshot;
}

std::optional<OutputServiceSubscriptionId> OutputService::Subscribe(OutputServiceListener listener)
{
	if (!listener) return std::nullopt;
	std::lock_guard lock(m_impl->mutex);
	if (m_impl->stopped || m_impl->subscriptions.size() == m_impl->limits.maximumSubscriptions || m_impl->nextSubscriptionId == 0) return std::nullopt;
	const auto id = m_impl->nextSubscriptionId++;
	m_impl->subscriptions.emplace(id, std::move(listener));
	return id;
}

void OutputService::Unsubscribe(const OutputServiceSubscriptionId subscriptionId) noexcept
{
	std::lock_guard lock(m_impl->mutex);
	m_impl->subscriptions.erase(subscriptionId);
}

} // namespace workbench::output
