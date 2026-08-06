/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "extension/CExtensionManager.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ZipArchiveFixture.h"

#include "util/file.h"

/*!
	@brief FetchVsixStreamed を実装する registry client を使って、streaming 経路そのものを検証する

	src/test/cpp/tests1/test-cextensionmanager.cpp の FakeOpenVsxRegistryClient は
	FetchVsixStreamed を override しないため（既定の Unsupported を継承する）、そちらの
	既存テストは常に buffered fallback（FetchVsix + WriteDownloadedVsix）経路だけを通る。
	このファイルは FetchVsixStreamed を実際に override して chunk 単位で sink を呼ぶ fake を
	使い、CExtensionManager::Install が streaming 経路（chunk ごとの逐次書き込み + 増分 sha256 +
	transport 相当の途中失敗 + キャンセル）を正しく扱うことを検証する。通信・Shell 展開は
	CZipFile::ExtractVsixSafely 自体（miniz ベース、IShellDispatch 不要）に閉じているため、
	外部サービスにも live/disabled 指定にも依存しない。
 */

namespace {

//! 導入先を実プロファイルから隔離する。失敗時の staging cleanup を観測できる。
class TempDirectory {
public:
	TempDirectory()
		: m_path(GetTempFilePath(L"extsdir"))
	{
		std::error_code ec;
		std::filesystem::remove(m_path, ec);
		std::filesystem::create_directory(m_path, ec);
	}
	~TempDirectory()
	{
		std::error_code ec;
		std::filesystem::remove_all(m_path, ec);
	}
	TempDirectory(const TempDirectory&) = delete;
	TempDirectory& operator = (const TempDirectory&) = delete;

	const std::filesystem::path& GetPath() const noexcept { return m_path; }

private:
	std::filesystem::path m_path;
};

//! extension/package.json だけを持つ、最小の妥当な VSIX を組み立てて中身をバイト列で返す
std::vector<std::uint8_t> BuildMinimalVsixBytes()
{
	const auto zipPath = GetTempFilePath(L"vxsrc");
	tests1::WriteZipArchive(zipPath, {
		{ .name = "extension/package.json",
		  .content = "{\"name\":\"streamed\",\"displayName\":\"Streamed Extension\"}" },
	});
	std::ifstream input(zipPath, std::ios::binary);
	std::vector<std::uint8_t> bytes(
		(std::istreambuf_iterator<char>(input)),
		std::istreambuf_iterator<char>());
	std::error_code ec;
	std::filesystem::remove(zipPath, ec);
	return bytes;
}

//! 名前・バージョン・URL だけを持つダウンロード可能な拡張を作る
SOpenVsxExtension MakeDownloadableExtension()
{
	SOpenVsxExtension ext;
	ext.sNamespace = L"test";
	ext.sName = L"streamed-extension";
	ext.sVersion = L"1.0.0";
	ext.sDownloadUrl = L"https://example.invalid/download.vsix";
	ext.sSha256Url = L"https://example.invalid/download.vsix.sha256";
	return ext;
}

/*!
	@brief FetchVsixStreamed を実際に override し、指定バイト列を chunk 単位で sink へ流す fake

	streamedOutcome / cancelAfterChunks で「途中で失敗を返す transport」「途中で cancellation
	が立つ」の両方を、実装を変えずに切り替えられるようにしてある。
 */
class StreamingFakeOpenVsxRegistryClient final : public extension::openvsx::IOpenVsxRegistryClient {
public:
	mutable int fetchVsixCalls = 0;			//!< buffered fallback (FetchVsix) が呼ばれた回数
	mutable int fetchVsixStreamedCalls = 0;	//!< FetchVsixStreamed が呼ばれた回数
	mutable int fetchSha256Calls = 0;
	mutable int sinkInvocations = 0;			//!< sink が実際に呼ばれた回数（chunk 分割の証拠）

	std::vector<std::uint8_t> vsixBytes;
	std::size_t chunkSize = 7;					//!< 小さく割って複数回 sink を呼ばせる
	extension::openvsx::EOpenVsxRequestOutcome streamedOutcome = extension::openvsx::EOpenVsxRequestOutcome::Success;
	std::optional<std::size_t> cancelAfterChunks;	//!< 指定回数だけ流したら cancellation を立てる想定にする
	extension::openvsx::OpenVsxBinaryOperation sha256Response{
		{ extension::openvsx::EOpenVsxRequestOutcome::NotRequested, platform::request::ERequestOutcome::Success, std::nullopt, {} }, {} };

	extension::openvsx::OpenVsxSearchOperation Search(
		std::wstring_view,
		int,
		int,
		const platform::request::IRequestCancellation* = nullptr) const override
	{
		return { { extension::openvsx::EOpenVsxRequestOutcome::InvalidRequest,
			platform::request::ERequestOutcome::InvalidRequest, std::nullopt, L"not used by this fixture" }, {} };
	}

	//! streaming が優先されるべきことの反証にする: 呼ばれたら常に失敗させ、テストで検出する
	extension::openvsx::OpenVsxBinaryOperation FetchVsix(
		std::wstring_view,
		const platform::request::IRequestCancellation* = nullptr) const override
	{
		++fetchVsixCalls;
		return { { extension::openvsx::EOpenVsxRequestOutcome::TransportFailure,
			platform::request::ERequestOutcome::TransportFailure, std::nullopt,
			L"buffered fallback must not be used when streaming is supported" }, {} };
	}

	extension::openvsx::OpenVsxBinaryOperation FetchOptionalSha256(
		const std::optional<std::wstring>&,
		const platform::request::IRequestCancellation* = nullptr) const override
	{
		++fetchSha256Calls;
		return sha256Response;
	}

	extension::openvsx::OpenVsxOperationStatus FetchVsixStreamed(
		std::wstring_view,
		const extension::openvsx::OpenVsxBodyChunkSink& sink,
		const platform::request::IRequestCancellation* cancellation = nullptr) const override
	{
		++fetchVsixStreamedCalls;
		std::size_t delivered = 0;
		std::size_t chunkCount = 0;
		while (delivered < vsixBytes.size()) {
			if (cancelAfterChunks && chunkCount >= *cancelAfterChunks) {
				return { extension::openvsx::EOpenVsxRequestOutcome::Cancelled,
					platform::request::ERequestOutcome::Cancelled, std::nullopt, L"cancelled mid-stream" };
			}
			if (cancellation && cancellation->IsCancellationRequested()) {
				return { extension::openvsx::EOpenVsxRequestOutcome::Cancelled,
					platform::request::ERequestOutcome::Cancelled, std::nullopt, L"cancelled mid-stream" };
			}
			const std::size_t remaining = vsixBytes.size() - delivered;
			const std::size_t thisChunk = (remaining < chunkSize) ? remaining : chunkSize;
			++sinkInvocations;
			if (!sink(vsixBytes.data() + delivered, thisChunk)) {
				return { extension::openvsx::EOpenVsxRequestOutcome::TransportFailure,
					platform::request::ERequestOutcome::TransportFailure, std::nullopt, L"sink rejected a chunk" };
			}
			delivered += thisChunk;
			++chunkCount;

			// 全量を流し終える前に、あえて途中失敗として打ち切る指定になっていれば従う。
			// 512 MiB 上限超過のような transport 側の逐次判定を模擬する。
			if (streamedOutcome != extension::openvsx::EOpenVsxRequestOutcome::Success && delivered >= vsixBytes.size() / 2) {
				return { streamedOutcome, platform::request::ERequestOutcome::ResponseBodyLimitExceeded,
					std::nullopt, L"simulated mid-stream transport failure" };
			}
		}
		return { extension::openvsx::EOpenVsxRequestOutcome::Success, platform::request::ERequestOutcome::Success, std::nullopt, {} };
	}
};

} // namespace

//! streaming 経路が成功すると、buffered FetchVsix には一切触れずに導入が完了する
TEST(CExtensionManagerStreamedInstall, StreamedFetchWritesIncrementallyAndInstallsSuccessfully)
{
	const TempDirectory directory;
	CExtensionManager manager(directory.GetPath());

	StreamingFakeOpenVsxRegistryClient client;
	client.vsixBytes = BuildMinimalVsixBytes();
	ASSERT_FALSE(client.vsixBytes.empty());
	client.chunkSize = 11;	// アーカイブ全体よりずっと小さく割って、複数回の chunk 配送を保証する
	client.sha256Response = { { extension::openvsx::EOpenVsxRequestOutcome::Success,
		platform::request::ERequestOutcome::Success, std::nullopt, {} }, {} };
	{
		const TempDirectory zipScratch;
		const auto scratchFile = zipScratch.GetPath() / L"src.vsix";
		{
			std::ofstream out(scratchFile, std::ios::binary | std::ios::trunc);
			out.write(reinterpret_cast<const char*>(client.vsixBytes.data()), static_cast<std::streamsize>(client.vsixBytes.size()));
		}
		const std::wstring sExpectedHex = CExtensionManager::ComputeSha256Hex(scratchFile);
		ASSERT_FALSE(sExpectedHex.empty());
		const std::string sExpectedHexUtf8(sExpectedHex.begin(), sExpectedHex.end());
		client.sha256Response.value.assign(sExpectedHexUtf8.begin(), sExpectedHexUtf8.end());
	}

	std::wstring errorMsg;
	const bool installed = manager.Install(MakeDownloadableExtension(), client, errorMsg);
	EXPECT_TRUE(installed) << errorMsg.c_str();

	EXPECT_EQ(0, client.fetchVsixCalls) << L"streaming が成功したので buffered fallback を使ってはならない";
	EXPECT_EQ(1, client.fetchVsixStreamedCalls);
	EXPECT_EQ(1, client.fetchSha256Calls);
	EXPECT_GT(client.sinkInvocations, 1) << L"chunk 単位で複数回 sink が呼ばれていること";

	SInstalledExtension found;
	EXPECT_TRUE(manager.FindInstalled(L"test.streamed-extension", found));
	EXPECT_STREQ(L"1.0.0", found.sVersion.c_str());
	EXPECT_TRUE(std::filesystem::exists(
		found.dir / CExtensionManager::kVsixContentDir / CExtensionManager::kManifestFileName));

	std::wstring uninstallError;
	EXPECT_TRUE(manager.Uninstall(L"test.streamed-extension", uninstallError)) << uninstallError.c_str();
}

//! transport 相当が転送途中で上限超過を報告した場合、staging を残さず失敗し、fallback もしない
TEST(CExtensionManagerStreamedInstall, MidStreamTransportFailureAbortsWithoutFallbackOrStaging)
{
	const TempDirectory directory;
	CExtensionManager manager(directory.GetPath());

	StreamingFakeOpenVsxRegistryClient client;
	client.vsixBytes = std::vector<std::uint8_t>(4096, 0x5A);
	client.chunkSize = 64;
	client.streamedOutcome = extension::openvsx::EOpenVsxRequestOutcome::ResponseBodyLimitExceeded;

	std::wstring errorMsg;
	EXPECT_FALSE(manager.Install(MakeDownloadableExtension(), client, errorMsg));
	EXPECT_EQ(L"cannot fetch extension package", errorMsg);
	EXPECT_EQ(0, client.fetchVsixCalls) << L"streaming の途中失敗は Unsupported ではないので fallback してはならない";
	EXPECT_EQ(1, client.fetchVsixStreamedCalls);
	EXPECT_EQ(0, client.fetchSha256Calls);
	EXPECT_TRUE(std::filesystem::is_empty(directory.GetPath()));
}

//! ストリーミング中に検出された cancellation は terminal cancelled になり、staging を残さない
TEST(CExtensionManagerStreamedInstall, MidStreamCancellationAbortsWithoutFallbackOrStaging)
{
	const TempDirectory directory;
	CExtensionManager manager(directory.GetPath());

	StreamingFakeOpenVsxRegistryClient client;
	client.vsixBytes = std::vector<std::uint8_t>(4096, 0x11);
	client.chunkSize = 64;
	client.cancelAfterChunks = 2;

	std::wstring errorMsg;
	EXPECT_FALSE(manager.Install(MakeDownloadableExtension(), client, errorMsg));
	EXPECT_EQ(L"extension installation cancelled", errorMsg);
	EXPECT_EQ(0, client.fetchVsixCalls);
	EXPECT_EQ(1, client.fetchVsixStreamedCalls);
	EXPECT_EQ(0, client.fetchSha256Calls);
	EXPECT_TRUE(std::filesystem::is_empty(directory.GetPath()));
}

//! streaming 中に増分計算した digest が registry の公開値と食い違えば、展開前に拒否する
TEST(CExtensionManagerStreamedInstall, StreamedDigestMismatchIsRejectedBeforeExtraction)
{
	const TempDirectory directory;
	CExtensionManager manager(directory.GetPath());

	StreamingFakeOpenVsxRegistryClient client;
	client.vsixBytes = BuildMinimalVsixBytes();
	ASSERT_FALSE(client.vsixBytes.empty());
	client.chunkSize = 13;
	const std::string wrongHex(64, '0');	// 妥当な形の 16 進 64 桁だが、実際の digest とは一致しない
	client.sha256Response = { { extension::openvsx::EOpenVsxRequestOutcome::Success,
		platform::request::ERequestOutcome::Success, std::nullopt, {} },
		std::vector<std::uint8_t>(wrongHex.begin(), wrongHex.end()) };

	std::wstring errorMsg;
	EXPECT_FALSE(manager.Install(MakeDownloadableExtension(), client, errorMsg));
	EXPECT_EQ(L"extension package sha256 did not match registry metadata", errorMsg);
	EXPECT_EQ(0, client.fetchVsixCalls);
	EXPECT_EQ(1, client.fetchVsixStreamedCalls);
	EXPECT_EQ(1, client.fetchSha256Calls);
	EXPECT_TRUE(std::filesystem::is_empty(directory.GetPath()));
}

//! FetchVsixStreamed が既定の Unsupported を返す（override しない）場合は、この fake でも
//! streaming ではなく buffered fallback（FetchVsix）を使うこと。
//! test-cextensionmanager.cpp の既存 fake と同じ既定挙動をこのファイルの fake でも確認する。
TEST(CExtensionManagerStreamedInstall, FallsBackToBufferedFetchWhenStreamingReportsUnsupported)
{
	class UnsupportedStreamingClient final : public extension::openvsx::IOpenVsxRegistryClient {
	public:
		mutable int fetchVsixCalls = 0;
		mutable int fetchSha256Calls = 0;

		extension::openvsx::OpenVsxSearchOperation Search(
			std::wstring_view, int, int, const platform::request::IRequestCancellation* = nullptr) const override
		{
			return { { extension::openvsx::EOpenVsxRequestOutcome::InvalidRequest,
				platform::request::ERequestOutcome::InvalidRequest, std::nullopt, {} }, {} };
		}

		extension::openvsx::OpenVsxBinaryOperation FetchVsix(
			std::wstring_view, const platform::request::IRequestCancellation* = nullptr) const override
		{
			++fetchVsixCalls;
			return { { extension::openvsx::EOpenVsxRequestOutcome::TransportFailure,
				platform::request::ERequestOutcome::TransportFailure, std::nullopt, L"expected fallback failure" }, {} };
		}

		extension::openvsx::OpenVsxBinaryOperation FetchOptionalSha256(
			const std::optional<std::wstring>&, const platform::request::IRequestCancellation* = nullptr) const override
		{
			++fetchSha256Calls;
			return { { extension::openvsx::EOpenVsxRequestOutcome::NotRequested,
				platform::request::ERequestOutcome::Success, std::nullopt, {} }, {} };
		}
		// FetchVsixStreamed は override しない: 基底の既定 Unsupported をそのまま使う。
	};

	const TempDirectory directory;
	CExtensionManager manager(directory.GetPath());
	UnsupportedStreamingClient client;

	std::wstring errorMsg;
	EXPECT_FALSE(manager.Install(MakeDownloadableExtension(), client, errorMsg));
	EXPECT_EQ(L"cannot fetch extension package", errorMsg);
	EXPECT_EQ(1, client.fetchVsixCalls) << L"Unsupported を返す streaming からは buffered fallback へ切り替わること";
	EXPECT_EQ(0, client.fetchSha256Calls);
	EXPECT_TRUE(std::filesystem::is_empty(directory.GetPath()));
}
