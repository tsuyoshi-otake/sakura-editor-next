/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "extension/CExtensionHostSharedState.h"

#include <filesystem>

#include <windows.h>

namespace {

std::filesystem::path UniqueProfilePath()
{
	return std::filesystem::temp_directory_path() /
		(L"sakura-extension-shared-state-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
			std::to_wstring(::GetTickCount64()));
}
} // namespace

TEST(CExtensionHostSharedStateTest, PublishesConsistentBrokerSnapshotToEditors)
{
	const auto profile = UniqueProfilePath();
	std::wstring diagnostic;
	CExtensionHostSharedState brokerState;
	ASSERT_TRUE(brokerState.CreateForBroker(profile, diagnostic)) << diagnostic;
	EXPECT_FALSE(brokerState.Read().has_value());

	CExtensionHostSharedState duplicateWriter;
	EXPECT_FALSE(duplicateWriter.CreateForBroker(profile, diagnostic));

	SExtensionHostBrokerSnapshot expected;
	expected.state = EExtensionHostState::Ready;
	expected.generation = 42;
	expected.hostProcessId = 1234;
	expected.retryCount = 2;
	expected.leaseOwnerCount = 3;
	expected.leaseCount = 5;
	expected.profileHash = L"0123456789abcdef0123456789abcdef";
	expected.bootId = L"fedcba9876543210fedcba9876543210";
	expected.extensionHostSessionId = L"00112233445566778899aabbccddeeff";
	expected.pipeName = L"\\\\.\\pipe\\sakura-exthost-test";
	expected.lastDiagnostic = "ready";
	brokerState.Publish(expected);

	CExtensionHostSharedState editorState;
	ASSERT_TRUE(editorState.OpenForEditor(profile, diagnostic)) << diagnostic;
	const auto actual = editorState.Read();
	ASSERT_TRUE(actual.has_value());
	EXPECT_EQ(actual->state, expected.state);
	EXPECT_EQ(actual->generation, expected.generation);
	EXPECT_EQ(actual->hostProcessId, expected.hostProcessId);
	EXPECT_EQ(actual->retryCount, expected.retryCount);
	EXPECT_EQ(actual->leaseOwnerCount, expected.leaseOwnerCount);
	EXPECT_EQ(actual->leaseCount, expected.leaseCount);
	EXPECT_EQ(actual->profileHash, expected.profileHash);
	EXPECT_EQ(actual->bootId, expected.bootId);
	EXPECT_EQ(actual->extensionHostSessionId, expected.extensionHostSessionId);
	EXPECT_EQ(actual->pipeName, expected.pipeName);
	EXPECT_EQ(actual->lastDiagnostic, expected.lastDiagnostic);

	expected.state = EExtensionHostState::KeepAlive;
	expected.leaseOwnerCount = 0;
	expected.leaseCount = 0;
	brokerState.Publish(expected);
	const auto updated = editorState.Read();
	ASSERT_TRUE(updated.has_value());
	EXPECT_EQ(updated->state, EExtensionHostState::KeepAlive);
	EXPECT_EQ(updated->leaseCount, 0u);
}

TEST(CExtensionHostSharedStateTest, RejectsMissingMalformedOrOversizeNativeSessionId)
{
	const auto profile = UniqueProfilePath();
	std::wstring diagnostic;
	CExtensionHostSharedState writer;
	ASSERT_TRUE(writer.CreateForBroker(profile, diagnostic)) << diagnostic;
	CExtensionHostSharedState reader;
	ASSERT_TRUE(reader.OpenForEditor(profile, diagnostic)) << diagnostic;

	SExtensionHostBrokerSnapshot snapshot;
	snapshot.state = EExtensionHostState::Ready;
	snapshot.extensionHostSessionId = L"00112233445566778899aabbccddeeff";
	writer.Publish(snapshot);
	ASSERT_TRUE(reader.Read().has_value());

	snapshot.extensionHostSessionId.clear();
	writer.Publish(snapshot);
	EXPECT_FALSE(reader.Read().has_value());

	snapshot.extensionHostSessionId = L"00112233445566778899aabbccddeefg";
	writer.Publish(snapshot);
	EXPECT_FALSE(reader.Read().has_value());

	snapshot.extensionHostSessionId.assign(33, L'a');
	writer.Publish(snapshot);
	EXPECT_FALSE(reader.Read().has_value());
}

TEST(CExtensionHostSharedStateTest, RequiresNativeSessionIdToBeClearedWhenUnavailable)
{
	const auto profile = UniqueProfilePath();
	std::wstring diagnostic;
	CExtensionHostSharedState writer;
	ASSERT_TRUE(writer.CreateForBroker(profile, diagnostic)) << diagnostic;
	CExtensionHostSharedState reader;
	ASSERT_TRUE(reader.OpenForEditor(profile, diagnostic)) << diagnostic;

	SExtensionHostBrokerSnapshot unavailable;
	unavailable.state = EExtensionHostState::Stopped;
	writer.Publish(unavailable);
	ASSERT_TRUE(reader.Read().has_value());
	EXPECT_TRUE(reader.Read()->extensionHostSessionId.empty());

	unavailable.extensionHostSessionId = L"00112233445566778899aabbccddeeff";
	writer.Publish(unavailable);
	EXPECT_FALSE(reader.Read().has_value());
}
