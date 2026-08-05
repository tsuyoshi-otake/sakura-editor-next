/*! @file */
/*
    Copyright (C) 2026, Sakura Editor Organization

    SPDX-License-Identifier: Zlib
*/

#include <sakura/controlipc/ControlIpcProtocol.h>

#if __has_include("platform/controlipc/ControlIpcProtocol.h") || __has_include("ControlIpcProtocol.h")
#error "sakura_controlipc_protocol_tests consumer can reach the provider private protocol header"
#endif

#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace platform::controlipc;

ControlIpcFrame HelloFrame()
{
	return { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::Hello,
		EControlIpcFlags::Request, 1, 0 }, {} };
}

ControlIpcFrame CancelAckFrame()
{
	return { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::CancelAck,
		EControlIpcFlags::Response | EControlIpcFlags::Terminal, 2, 7 }, {} };
}

std::vector<std::uint8_t> ExpectedHelloBytes()
{
	return {
		0x1c, 0x00, 0x00, 0x00, 0x53, 0x43, 0x49, 0x50,
		0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
		0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
}

std::vector<std::uint8_t> ExpectedCancelAckBytes()
{
	return {
		0x1c, 0x00, 0x00, 0x00, 0x53, 0x43, 0x49, 0x50,
		0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x06, 0x00,
		0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
}

std::vector<std::uint8_t> Encode(const ControlIpcFrame& frame)
{
	const auto result = EncodeControlIpcFrame(frame);
	if (result.outcome != EControlIpcEncodeOutcome::Encoded) return {};
	return result.bytes;
}

bool CanonicalFixtureFramesMatch()
{
	return Encode(HelloFrame()) == ExpectedHelloBytes()
		&& Encode(CancelAckFrame()) == ExpectedCancelAckBytes();
}

bool FragmentedAndCoalescedDecodePreservesFrames()
{
	const auto first = Encode(HelloFrame());
	const auto second = Encode(CancelAckFrame());
	std::vector<std::uint8_t> combined = first;
	combined.insert(combined.end(), second.begin(), second.end());

	CControlIpcFrameDecoder decoder;
	if (decoder.Feed(std::span<const std::uint8_t>(combined).first(2)).outcome != EControlIpcDecodeOutcome::NeedMoreData) return false;
	const auto decoded = decoder.Feed(std::span<const std::uint8_t>(combined).subspan(2));
	return decoded.outcome == EControlIpcDecodeOutcome::Decoded
		&& decoded.frames.size() == 2
		&& decoded.frames[0].header.kind == EControlIpcKind::Hello
		&& decoded.frames[1].header.kind == EControlIpcKind::CancelAck
		&& decoded.frames[1].header.generation == 7;
}

bool MinorVersionAndUtf8FieldsRemainCompatible()
{
	auto frame = CancelAckFrame();
	frame.header.minorVersion = kControlIpcMinorVersion + 3;
	const auto encoded = Encode(frame);
	CControlIpcFrameDecoder decoder;
	const auto decoded = decoder.Feed(encoded);
	if (decoded.outcome != EControlIpcDecodeOutcome::Decoded || decoded.frames.size() != 1
		|| decoded.frames[0].header.minorVersion != kControlIpcMinorVersion + 3) return false;

	ControlIpcFields fields;
	if (!AddUtf8Field(fields, EControlIpcFieldTag::Diagnostic, "protocol fixture")) return false;
	const auto fieldBytes = EncodeControlIpcFields(fields);
	if (!fieldBytes) return false;
	const auto restored = DecodeControlIpcFields(*fieldBytes);
	const auto diagnostic = GetUtf8Field(restored.fields, EControlIpcFieldTag::Diagnostic);
	return restored.outcome == EControlIpcFieldDecodeOutcome::Decoded && diagnostic && *diagnostic == "protocol fixture";
}

bool InvalidDirectionIsRejectedBeforeTransport()
{
	auto invalid = CancelAckFrame();
	invalid.header.flags = EControlIpcFlags::Request;
	return EncodeControlIpcFrame(invalid).outcome == EControlIpcEncodeOutcome::InvalidFlags;
}

bool StickyFailureHasExplicitResetTerminal()
{
	CControlIpcFrameDecoder decoder(64);
	const std::array<std::uint8_t, 4> oversize{ 65, 0, 0, 0 };
	if (decoder.Feed(oversize).outcome != EControlIpcDecodeOutcome::OversizeFrame || !decoder.IsFailed()) return false;
	if (decoder.Feed(Encode(HelloFrame())).outcome != EControlIpcDecodeOutcome::OversizeFrame) return false;
	decoder.Reset();
	const auto decoded = decoder.Feed(Encode(HelloFrame()));
	return decoded.outcome == EControlIpcDecodeOutcome::Decoded && decoded.frames.size() == 1 && !decoder.IsFailed();
}

struct TestCase {
	std::string_view name;
	bool (*run)();
};

constexpr std::array kTests{
	TestCase{ "CanonicalFixtureFramesMatch", CanonicalFixtureFramesMatch },
	TestCase{ "FragmentedAndCoalescedDecodePreservesFrames", FragmentedAndCoalescedDecodePreservesFrames },
	TestCase{ "MinorVersionAndUtf8FieldsRemainCompatible", MinorVersionAndUtf8FieldsRemainCompatible },
	TestCase{ "InvalidDirectionIsRejectedBeforeTransport", InvalidDirectionIsRejectedBeforeTransport },
	TestCase{ "StickyFailureHasExplicitResetTerminal", StickyFailureHasExplicitResetTerminal },
};

bool Matches(std::string_view fullName, std::string_view filter)
{
	if (filter.empty() || filter == "*") return true;
	const auto star = filter.find('*');
	if (star == std::string_view::npos) return fullName == filter;
	const auto prefix = filter.substr(0, star);
	const auto suffix = filter.substr(star + 1);
	return fullName.starts_with(prefix) && fullName.ends_with(suffix)
		&& fullName.size() >= prefix.size() + suffix.size();
}

} // namespace

int main(int argc, char** argv)
{
	std::string_view filter = "ControlIpcProtocol.*";
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument = argv[index];
		if (argument == "--gtest_list_tests") {
			std::cout << "ControlIpcProtocol.\n";
			for (const auto& test : kTests) std::cout << "  " << test.name << '\n';
			return 0;
		}
		constexpr std::string_view prefix = "--gtest_filter=";
		if (argument.starts_with(prefix)) filter = argument.substr(prefix.size());
	}

	int selected = 0;
	int failed = 0;
	for (const auto& test : kTests) {
		const std::string fullName = "ControlIpcProtocol." + std::string(test.name);
		if (!Matches(fullName, filter)) continue;
		++selected;
		const bool passed = test.run();
		std::cout << (passed ? "[       OK ] " : "[  FAILED  ] ") << fullName << '\n';
		if (!passed) ++failed;
	}
	std::cout << "[==========] " << selected << " tests ran; " << failed << " failed.\n";
	return failed == 0 && selected > 0 ? 0 : 1;
}
