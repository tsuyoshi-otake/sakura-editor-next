/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "config/WorkspaceTrustMementoCodec.h"

#include <sakura/storage/StorageTypes.h>

#include <string>

namespace {

using config::CWorkspaceTrustMementoCodec;
using config::EWorkspaceTrustMementoCodecStatus;
using config::WorkspaceTrustMemento;

std::string Encoded(const WorkspaceTrustMemento& memento)
{
	const auto encoded = CWorkspaceTrustMementoCodec::Encode(memento);
	EXPECT_TRUE(encoded.Succeeded()) << encoded.diagnostic;
	return encoded.payload;
}

// ---------------------------------------------------------------------------
// Round trip
// ---------------------------------------------------------------------------

TEST(WorkspaceTrustMementoCodecTest, DefaultMementoRoundTrips)
{
	const WorkspaceTrustMemento memento;
	const auto decoded = CWorkspaceTrustMementoCodec::Decode(Encoded(memento));
	ASSERT_TRUE(decoded.Succeeded()) << decoded.diagnostic;
	EXPECT_EQ(memento, *decoded.memento);
}

TEST(WorkspaceTrustMementoCodecTest, EveryFlagCombinationRoundTrips)
{
	for (const bool startupPromptShown : { false, true }) {
		for (const bool untrustedFilesAccepted : { false, true }) {
			const WorkspaceTrustMemento memento{ startupPromptShown, untrustedFilesAccepted };
			const auto decoded = CWorkspaceTrustMementoCodec::Decode(Encoded(memento));
			ASSERT_TRUE(decoded.Succeeded()) << decoded.diagnostic;
			EXPECT_EQ(memento, *decoded.memento);
		}
	}
}

TEST(WorkspaceTrustMementoCodecTest, EncodingIsDeterministic)
{
	const WorkspaceTrustMemento memento{ true, false };
	EXPECT_EQ(Encoded(memento), Encoded(memento));
}

TEST(WorkspaceTrustMementoCodecTest, EncodedPayloadCarriesTheCurrentFormatVersion)
{
	const auto payload = Encoded(WorkspaceTrustMemento{});
	EXPECT_NE(std::string::npos, payload.find("\"formatVersion\""));
	EXPECT_NE(std::string::npos, payload.find("\"startupPromptShown\""));
	EXPECT_NE(std::string::npos, payload.find("\"untrustedFilesAccepted\""));
}

// ---------------------------------------------------------------------------
// Absent flags ask again rather than rejecting the record
// ---------------------------------------------------------------------------

TEST(WorkspaceTrustMementoCodecTest, AnAbsentFlagDecodesAsAskAgain)
{
	const auto decoded = CWorkspaceTrustMementoCodec::Decode(R"({"formatVersion":1})");
	ASSERT_TRUE(decoded.Succeeded()) << decoded.diagnostic;
	EXPECT_FALSE(decoded.memento->startupPromptShown);
	EXPECT_FALSE(decoded.memento->untrustedFilesAccepted);
}

TEST(WorkspaceTrustMementoCodecTest, OneKnownFlagSurvivesWhenTheOtherIsAbsent)
{
	const auto decoded = CWorkspaceTrustMementoCodec::Decode(
		R"({"formatVersion":1,"startupPromptShown":true})");
	ASSERT_TRUE(decoded.Succeeded()) << decoded.diagnostic;
	EXPECT_TRUE(decoded.memento->startupPromptShown);
	EXPECT_FALSE(decoded.memento->untrustedFilesAccepted);
}

TEST(WorkspaceTrustMementoCodecTest, UnknownMembersAreIgnoredRatherThanRejected)
{
	const auto decoded = CWorkspaceTrustMementoCodec::Decode(
		R"({"formatVersion":1,"startupPromptShown":true,"somethingLater":"value"})");
	ASSERT_TRUE(decoded.Succeeded()) << decoded.diagnostic;
	EXPECT_TRUE(decoded.memento->startupPromptShown);
}

TEST(WorkspaceTrustMementoCodecTest, LeadingAndTrailingWhitespaceIsAccepted)
{
	const auto decoded = CWorkspaceTrustMementoCodec::Decode(
		"  \r\n\t{\"formatVersion\":1,\"untrustedFilesAccepted\":true}\r\n  ");
	ASSERT_TRUE(decoded.Succeeded()) << decoded.diagnostic;
	EXPECT_TRUE(decoded.memento->untrustedFilesAccepted);
}

// ---------------------------------------------------------------------------
// A flag that is present but not a boolean is a corruption, never a default
// ---------------------------------------------------------------------------

TEST(WorkspaceTrustMementoCodecTest, AStringifiedFlagIsCorruptRatherThanTrue)
{
	const auto decoded = CWorkspaceTrustMementoCodec::Decode(
		R"({"formatVersion":1,"startupPromptShown":"true"})");
	EXPECT_EQ(EWorkspaceTrustMementoCodecStatus::CorruptPayload, decoded.status);
	EXPECT_FALSE(decoded.memento.has_value());
}

TEST(WorkspaceTrustMementoCodecTest, ANumericFlagIsCorruptRatherThanTruthy)
{
	const auto decoded = CWorkspaceTrustMementoCodec::Decode(
		R"({"formatVersion":1,"untrustedFilesAccepted":1})");
	EXPECT_EQ(EWorkspaceTrustMementoCodecStatus::CorruptPayload, decoded.status);
	EXPECT_FALSE(decoded.memento.has_value());
}

TEST(WorkspaceTrustMementoCodecTest, ANullFlagIsCorruptRatherThanAbsent)
{
	const auto decoded = CWorkspaceTrustMementoCodec::Decode(
		R"({"formatVersion":1,"startupPromptShown":null})");
	EXPECT_EQ(EWorkspaceTrustMementoCodecStatus::CorruptPayload, decoded.status);
}

// ---------------------------------------------------------------------------
// Malformed documents
// ---------------------------------------------------------------------------

TEST(WorkspaceTrustMementoCodecTest, EmptyPayloadIsCorrupt)
{
	EXPECT_EQ(EWorkspaceTrustMementoCodecStatus::CorruptPayload,
		CWorkspaceTrustMementoCodec::Decode("").status);
}

TEST(WorkspaceTrustMementoCodecTest, TruncatedJsonIsCorrupt)
{
	EXPECT_EQ(EWorkspaceTrustMementoCodecStatus::CorruptPayload,
		CWorkspaceTrustMementoCodec::Decode(R"({"formatVersion":1)").status);
}

TEST(WorkspaceTrustMementoCodecTest, ANonObjectRootIsCorrupt)
{
	EXPECT_EQ(EWorkspaceTrustMementoCodecStatus::CorruptPayload,
		CWorkspaceTrustMementoCodec::Decode("[]").status);
}

TEST(WorkspaceTrustMementoCodecTest, TrailingContentAfterTheDocumentIsCorrupt)
{
	EXPECT_EQ(EWorkspaceTrustMementoCodecStatus::CorruptPayload,
		CWorkspaceTrustMementoCodec::Decode(R"({"formatVersion":1} {"formatVersion":1})").status);
}

TEST(WorkspaceTrustMementoCodecTest, InvalidUtf8IsCorrupt)
{
	std::string payload = R"({"formatVersion":1,"note":"x"})";
	payload[payload.size() - 3] = static_cast<char>(0xff);
	EXPECT_EQ(EWorkspaceTrustMementoCodecStatus::CorruptPayload,
		CWorkspaceTrustMementoCodec::Decode(payload).status);
}

TEST(WorkspaceTrustMementoCodecTest, NestingBeyondTheFlatSchemaIsCorrupt)
{
	// The bound is checked before parsing, so a deeply nested document is rejected
	// without the parser ever being asked to walk it.
	EXPECT_EQ(EWorkspaceTrustMementoCodecStatus::CorruptPayload,
		CWorkspaceTrustMementoCodec::Decode(
			R"({"formatVersion":1,"a":{"b":{"c":{"d":1}}}})").status);
}

TEST(WorkspaceTrustMementoCodecTest, ABraceInsideAStringDoesNotCountAsNesting)
{
	const auto decoded = CWorkspaceTrustMementoCodec::Decode(
		R"({"formatVersion":1,"note":"{{{{{{{{{{","startupPromptShown":true})");
	ASSERT_TRUE(decoded.Succeeded()) << decoded.diagnostic;
	EXPECT_TRUE(decoded.memento->startupPromptShown);
}

// ---------------------------------------------------------------------------
// Schema version
// ---------------------------------------------------------------------------

TEST(WorkspaceTrustMementoCodecTest, AMissingFormatVersionIsUnsupportedSchema)
{
	EXPECT_EQ(EWorkspaceTrustMementoCodecStatus::UnsupportedSchema,
		CWorkspaceTrustMementoCodec::Decode(R"({"startupPromptShown":true})").status);
}

TEST(WorkspaceTrustMementoCodecTest, AFutureFormatVersionIsUnsupportedSchema)
{
	EXPECT_EQ(EWorkspaceTrustMementoCodecStatus::UnsupportedSchema,
		CWorkspaceTrustMementoCodec::Decode(R"({"formatVersion":2})").status);
}

TEST(WorkspaceTrustMementoCodecTest, ANonNumericFormatVersionIsUnsupportedSchema)
{
	EXPECT_EQ(EWorkspaceTrustMementoCodecStatus::UnsupportedSchema,
		CWorkspaceTrustMementoCodec::Decode(R"({"formatVersion":"1"})").status);
}

TEST(WorkspaceTrustMementoCodecTest, ANegativeOrFractionalFormatVersionIsUnsupportedSchema)
{
	EXPECT_EQ(EWorkspaceTrustMementoCodecStatus::UnsupportedSchema,
		CWorkspaceTrustMementoCodec::Decode(R"({"formatVersion":-1})").status);
	EXPECT_EQ(EWorkspaceTrustMementoCodecStatus::UnsupportedSchema,
		CWorkspaceTrustMementoCodec::Decode(R"({"formatVersion":1.5})").status);
}

// ---------------------------------------------------------------------------
// Bounds
// ---------------------------------------------------------------------------

TEST(WorkspaceTrustMementoCodecTest, AnOversizedPayloadIsRejectedBeforeParsing)
{
	std::string payload(platform::storage::kMaximumStorageMutationPayloadBytes + 1, 'x');
	EXPECT_EQ(EWorkspaceTrustMementoCodecStatus::PayloadTooLarge,
		CWorkspaceTrustMementoCodec::Decode(payload).status);
}

TEST(WorkspaceTrustMementoCodecTest, TheEncodedRecordStaysFarInsideTheMutationBound)
{
	EXPECT_LT(Encoded(WorkspaceTrustMemento{ true, true }).size(),
		platform::storage::kMaximumStorageMutationPayloadBytes);
}

} // namespace
