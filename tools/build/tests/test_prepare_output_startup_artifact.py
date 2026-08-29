"""Contract checks for the explicit Output startup-artifact producer.

The producer owns a potentially expensive MSVC build.  These tests therefore
exercise its static contract and the opt-in PowerShell self-test only; they do
not invoke a native build, Cargo, or the runtime-stage command.
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
PRODUCER = ROOT / "tools" / "prepare-output-startup-artifact.ps1"
SAKURA_BUILD = ROOT / "tools" / "build" / "sakura_build.py"
FINAL_IMAGE_EVIDENCE = ROOT / "tools" / "build" / "sakura_build_lib" / "output_final_image_evidence.py"


def read_utf16le_script(path: Path) -> str:
    raw = path.read_bytes()
    if not raw.startswith(b"\xff\xfe"):
        raise AssertionError(f"{path} is missing a UTF-16LE BOM")
    text = raw.decode("utf-16")
    if "\n" in text.replace("\r\n", ""):
        raise AssertionError(f"{path} contains a bare LF")
    if "\r\r\n" in text:
        raise AssertionError(f"{path} contains CRCRLF")
    return text


def powershell_hosts() -> list[str]:
    return [name for name in ("powershell.exe", "pwsh") if shutil.which(name)]


def _matching_powershell_brace(text: str, opening: int) -> int:
    """Return the close brace for a PowerShell function body.

    PowerShell command strings contain format placeholders such as ``{0}``,
    so a regular expression that stops at the first close brace is not a
    useful structural check.  This small lexer skips strings and comments
    while balancing braces; it intentionally does not execute the script.
    """

    depth = 0
    index = opening
    quote: str | None = None
    block_comment = False
    while index < len(text):
        character = text[index]
        following = text[index + 1] if index + 1 < len(text) else ""
        if block_comment:
            if character == "#" and following == ">":
                block_comment = False
                index += 2
                continue
            index += 1
            continue
        if quote == "'":
            if character == "'":
                if following == "'":
                    index += 2
                    continue
                quote = None
            index += 1
            continue
        if quote == '"':
            if character == "`":
                index += 2
                continue
            if character == '"':
                quote = None
            index += 1
            continue
        if character == "<" and following == "#":
            block_comment = True
            index += 2
            continue
        if character == "#":
            newline = text.find("\n", index)
            index = len(text) if newline < 0 else newline + 1
            continue
        if character in ("'", '"'):
            quote = character
            index += 1
            continue
        if character == "`":
            index += 2
            continue
        if character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                return index
        index += 1
    raise AssertionError("PowerShell function has an unterminated brace body")


def powershell_function_body(text: str, name: str) -> str:
    """Extract one function body without relying on line numbers."""

    declaration = re.search(
        rf"(?im)^\s*function\s+{re.escape(name)}\s*\{{", text
    )
    if declaration is None:
        raise AssertionError(f"PowerShell function {name!r} is missing")
    opening = text.find("{", declaration.start(), declaration.end())
    if opening < 0:
        raise AssertionError(f"PowerShell function {name!r} has no body")
    closing = _matching_powershell_brace(text, opening)
    return text[opening + 1 : closing]


def powershell_functions(text: str) -> list[tuple[str, str]]:
    """Extract top-level function names and bodies for semantic checks."""

    declarations = list(
        re.finditer(r"(?im)^\s*function\s+([A-Za-z_][\w-]*)\s*\{", text)
    )
    functions: list[tuple[str, str]] = []
    for declaration in declarations:
        name = declaration.group(1)
        opening = text.find("{", declaration.start(), declaration.end())
        closing = _matching_powershell_brace(text, opening)
        functions.append((name, text[opening + 1 : closing]))
    return functions


class PrepareOutputStartupArtifactContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = read_utf16le_script(PRODUCER)
        cls.cli_text = SAKURA_BUILD.read_text(encoding="utf-8")
        cls.final_image_evidence_text = FINAL_IMAGE_EVIDENCE.read_text(encoding="utf-8")

    def test_script_is_utf16le_with_crlf(self) -> None:
        raw = PRODUCER.read_bytes()
        self.assertTrue(raw.startswith(b"\xff\xfe"))
        text = raw.decode("utf-16")
        self.assertNotIn("\n", text.replace("\r\n", ""))
        self.assertNotIn("\r\r\n", text)

    def test_selector_and_reproducibility_contract_is_explicit(self) -> None:
        for marker in (
            "Assert-BackendSelector",
            "SAKURA_OUTPUT_BACKEND = $Backend",
            "SAKURA_UTF16_BACKEND = 'cpp'",
            "SAKURA_OUTPUT_PRODUCTION_PACKAGE = 'false'",
            "SAKURA_UTF16_PRODUCTION_PACKAGE = 'false'",
            "SAKURA_UTF16_BENCHMARK_TELEMETRY = 'false'",
            "SAKURA_GENERATE_ASSEMBLY_LISTINGS = 'false'",
            "SKIP_CREATE_GITHASH = '1'",
            "MSBUILDDISABLENODEREUSE = '1'",
            "SAKURA_BUILD_JOBS = [string]$BuildParallelism",
            "sourceHead",
            "sourceDirty",
            "sourceStatusSha256",
            "$statusLines = @()",
            "Get-MsvcIdentity",
            "Resolve-RustcExecutable",
            "Invoke-NativeOutputCapture $rustup @('which', 'rustc')",
            "Get-RustToolchainIdentity",
            "rust/native/Cargo.lock",
            "packagePlanSha256",
            "buildCommandSha256",
            "windowsImageSha256",
            "powerModeSha256",
            "selectorProof",
            "providerObjectSha256After",
            "Resolve-Dumpbin",
            "Microsoft Visual Studio/Installer/vswhere.exe",
            "-requires Microsoft.Component.MSBuild -find",
            "VC\\Tools\\MSVC\\**\\bin\\Hostx64\\x64\\dumpbin.exe",
            "$dumpbin = Resolve-Dumpbin $comspec",
            "Get-SelectorProof $providerObjectBefore $providerObjectAfter",
            "Get-NormalizedProviderSymbols",
            "Get-ProviderCompileSelector",
            "Assert-ProviderCompileSelector",
            "Assert-ProviderObjectFormat",
            "providerObjectFormat",
            "compileLogProof",
            "compileCommandHasGl",
            "compileLogExistsBefore",
            "compileLogExistsAfter",
            "$compileLogAfter = Get-OptionalFileIdentity $compileLogSource",
            "$Configuration -eq 'Release'",
            "Assert-ProviderCompileSelector $compileLogText",
            "ANONYMOUS OBJECT",
            "msvc-ltcg-compile-selector-verified",
            "compileLogSha256Before",
            "compileLogSizeBytesBefore",
            "compileLogSha256After",
            "compileLogSizeBytesAfter",
            "compileCommandRustSelectorDefineCount",
            "compile-log-before",
            "compile-log-after",
            "dumpbin-unresolved-refs-verified",
            "Get-ProviderArchiveProof",
            "Add-ProviderArchiveProof",
            "dumpbin-defined-exports-verified",
            "definedProviderSymbols",
            "archiveExportsVerified",
            "$expected.Sort([StringComparer]::Ordinal)",
            "$unorderedRustSymbols",
            "rustSelectorProofVerified",
            "cleanup-unverified",
            "finalRootExists",
            "remainingCount",
            "cleanupEnvelopeVerified",
            "Acquire-ExclusiveLock",
            "FileStream]::new",
            "FileMode]::CreateNew",
            "FileShare]::None",
            "FileOptions]::DeleteOnClose",
            "Release-ExclusiveLock",
            "exclusiveLockVerified",
        ):
            self.assertIn(marker, self.text)
        self.assertNotIn("CreateDirectory($script:LockPath)", self.text)
        self.assertNotIn("$compileLogAfter = Get-FileIdentity $compileLogSource", self.text)
        self.assertRegex(self.text, r"\$script:SchemaVersion\s*=\s*1")
        self.assertIn("[ValidateSet('x64')]", self.text)
        self.assertIn("[ValidateSet('Debug', 'Release')]", self.text)
        self.assertNotRegex(self.text, r"Get-ChildItem[^\r\n]*-Recurse")
        self.assertNotRegex(self.text, r"Remove-Item[^\r\n]*-Recurse")

    def test_transaction_and_fail_closed_identity_contract_is_explicit(self) -> None:
        for marker in (
            "Get-OptionalFileIdentity $artifactSource",
            "Get-OptionalFileIdentity $compileLogSource",
            "$artifactAfter = Get-FileIdentity $artifactSource",
            "$compileLogAfter = Get-OptionalFileIdentity $compileLogSource",
            "CL.command.1.tlog",
            "Release provider object and compile log were not produced by this build.",
            "artifactHashBefore",
            "artifactHashAfter",
            "artifactSha256Before",
            "artifactSha256After",
            "Assert-SourceStateEqual $sourceBefore $sourceAfterBuild",
            "Assert-SourceStateEqual $sourceBefore $sourceAfterStage",
            "Get-RuntimeStageSnapshot $canonicalStage",
            "Copy-RuntimeStage $canonicalStage $transactionStage",
            "Convert-RuntimeReceiptPath",
            "Assert-RuntimeReceiptArtifactIdentity",
            "nested\\sakura_lang_en_US.dll",
            "COM[1-9]|LPT[1-9]",
            "dependencyClosureSha256",
            "runtimeStageReceiptSha256",
            "Write-JsonAtomic $manifestPath $manifest",
            "[IO.Directory]::Move($transactionRoot, $finalRoot)",
            "manifestGeneratedByProducer = $true",
            "publication = 'atomic-directory-rename'",
        ):
            self.assertIn(marker, self.text)
        self.assertIn("if (Test-Path -LiteralPath $finalRoot) { throw", self.text)
        self.assertIn("refusing overwrite", self.text)
        self.assertIn("Assert-PayloadFreeManifest", self.text)

    def test_canonical_final_image_verify_cli_is_no_build_and_payload_free(self) -> None:
        cli = self.cli_text
        parser_marker = 'evidence_final_image_verify = evidence_commands.add_parser('
        dispatch_marker = 'if args.command == "evidence" and args.evidence_command == "output-final-image-verify":'
        self.assertIn(parser_marker, cli)
        dispatch = cli.index(dispatch_marker)
        graph_load = cli.index("graph = load_semantic_graph", dispatch)
        manifest_resolution = cli.index("manifest = args.manifest.resolve()", dispatch)
        verify_function = re.search(
            r"^def _run_output_final_image_verify\b(?P<body>.*?)(?=^def |^class |\Z)",
            cli,
            re.MULTILINE | re.DOTALL,
        )
        self.assertIsNotNone(verify_function, "canonical final-image verify helper is missing")
        assert verify_function is not None
        verify_body = verify_function.group("body")
        self.assertLess(dispatch, manifest_resolution)
        self.assertLess(dispatch, graph_load)
        self.assertNotIn("load_semantic_graph", cli[dispatch:graph_load])
        self.assertNotIn("args.manifest.resolve", cli[dispatch:manifest_resolution])

        for option in (
            "--native-evidence",
            "--stage-root",
            "--backend",
            "--platform",
            "--configuration",
            "--artifact-sha256",
            "--artifact-size-bytes",
        ):
            self.assertIn(option, cli[cli.index(parser_marker) : dispatch])
        for marker in (
            "load_bounded_json(",
            "validate_bound_native_evidence_for_final_image(",
            '"payloadFree": True',
            '"record": "output-final-image-binding-validation"',
            '"failureCode": code',
            "return EXIT_USAGE",
            "return 5",
            'output(result, "json")',
        ):
            self.assertIn(marker, verify_body)

        # The result keys below are owned by the Python evidence module.  This
        # makes the producer contract follow the canonical serializer rather
        # than duplicating a PowerShell-specific receipt schema.
        for field in (
            "boundNativeEvidenceSha256",
            "sourceNativeEvidenceSha256",
            "stageId",
            "receiptPath",
            "receiptSha256",
            "files",
            "provider",
        ):
            self.assertIn(f'"{field}"', self.final_image_evidence_text)

    def _final_image_verify_helper(self) -> tuple[str, str]:
        helpers = [
            (name, body)
            for name, body in powershell_functions(self.text)
            if "evidence output-final-image-verify" in body
        ]
        self.assertEqual(
            1,
            len(helpers),
            "exactly one PowerShell helper may assemble the canonical barrier command",
        )
        return helpers[0]

    def _summary_body(self, producer: str) -> str:
        match = re.search(
            r"\$summary\s*=\s*\[ordered\]@\{(?P<body>.*?)\r?\n\s*\}\r?\n\s*\$successSummary\s*=\s*\$summary",
            producer,
            re.DOTALL,
        )
        self.assertIsNotNone(match, "producer success summary is missing")
        assert match is not None
        return match.group("body")

    def test_qualified_final_image_has_one_rebuild_and_three_ordered_barriers(self) -> None:
        producer = powershell_function_body(self.text, "Invoke-Producer")
        helper_name, helper = self._final_image_verify_helper()
        observer_helper = powershell_function_body(self.text, "New-QualifiedObserverCommand")

        for marker in (
            "[switch]$QualifiedFinalImage",
            "$FinalImageStageRoot",
            "New-QualifiedObserverCommand",
            "--rebuild",
            "--final-image-backend",
            "--final-image-stage-root",
            "--package-timeout-seconds",
            "Assert-QualifiedSourceState",
            "finalImageStageRootExists",
            "finalImageStageRootExpected",
            "Qualified final-image production requires a clean exact source state.",
        ):
            self.assertIn(marker, self.text)
        self.assertEqual(1, observer_helper.count("inventory observe-product"))
        self.assertEqual(1, observer_helper.count("--rebuild"))
        self.assertEqual(1, len(re.findall(r"\bNew-QualifiedObserverCommand\b", producer)))

        qualified_build = re.search(
            r"\$buildCommandText\s*=\s*if\s*\(\s*\$QualifiedFinalImage\s*\)\s*\{(?P<qualified>.*?)\r?\n\s*\}\s*else\s*\{(?P<legacy>.*?)\r?\n\s*\}",
            producer,
            re.DOTALL,
        )
        self.assertIsNotNone(qualified_build, "qualified/legacy build gate is missing")
        assert qualified_build is not None
        self.assertIn("New-QualifiedObserverCommand", qualified_build.group("qualified"))
        self.assertNotIn("build-dev.bat", qualified_build.group("qualified"))
        self.assertIn("buildBatch", qualified_build.group("legacy"))
        self.assertIn("build-dev.bat", self.text)
        self.assertIn(
            "$observerWorkTimeoutSeconds = [Int64]$TimeoutSeconds + [Int64]$PackageTimeoutSeconds",
            producer,
        )

        # Count call sites in the producer body, not copies of the CLI text in
        # the helper.  This remains stable if the helper is renamed or its
        # command line is reformatted.
        call_matches = [
            match
            for match in re.finditer(rf"(?im)^[^\r\n]*\b{re.escape(helper_name)}\b", producer)
            if not match.group(0).lstrip().startswith("#")
        ]
        self.assertEqual(3, len(call_matches), "qualified producer must use exactly three barriers")
        call_positions = [match.start() for match in call_matches]
        self.assertLess(call_positions[0], call_positions[1])
        self.assertLess(call_positions[1], call_positions[2])

        build_block_end = qualified_build.end()
        observer_completion = producer.find("Invoke-OwnedCommand", build_block_end)
        self.assertGreaterEqual(observer_completion, 0, "observer command has no owned execution")
        manifest_call = producer.find("New-BuildManifest ", call_positions[0])
        self.assertGreaterEqual(manifest_call, 0, "manifest construction is missing")
        move_match = re.search(
            r"\[IO\.Directory\]::Move\(\s*\$transactionRoot\s*,\s*\$finalRoot\s*\)",
            producer,
        )
        self.assertIsNotNone(move_match, "atomic publication move is missing")
        assert move_match is not None
        self.assertGreater(call_positions[0], observer_completion)
        self.assertLess(call_positions[1], manifest_call)
        self.assertGreater(call_positions[2], move_match.start())

        # The last barrier must consume the native record after publication,
        # not the transaction-root path that was used before the move.
        post_move = producer[move_match.start() : call_positions[2] + 1400]
        self.assertRegex(
            post_move,
            r"(?is)(?:final(?:[-_]image)?native|movednative|native[-_]product|Join-Path\s+\$finalRoot|\$finalRoot)",
        )
        for option in (
            "--native-evidence",
            "--stage-root",
            "--backend",
            "--platform",
            "--configuration",
            "--artifact-sha256",
            "--artifact-size-bytes",
        ):
            self.assertIn(option, helper)
        for marker in (
            "BarrierSubstage",
            "failureCode",
            "Set-ProducerFailureContext ([string]$failureResult.failureCode) $BarrierSubstage",
            "Set-ProducerFailureContext 'OUTPUT_FINAL_IMAGE_VERIFY_OUTPUT_INVALID' $BarrierSubstage",
            "Read-OwnedBoundedUtf8Text",
            "Get-OwnedFileIdentity",
            "Open-VerifiedOwnedOutput",
            "Remove-VerifiedOwnedOutput",
            "$nativeEvidenceParent = Split-Path -Parent",
            "$temporaryOwned = $false",
            "$temporaryOwned = $true",
            "$temporaryIdentity = $null",
            "$temporaryIdentity = Get-OwnedFileIdentity $temporaryStream",
            "[IO.FileMode]::CreateNew",
            "[IO.FileShare]::ReadWrite",
            "Open-VerifiedOwnedOutput $temporary $temporaryIdentity $BarrierSubstage",
            "Read-OwnedBoundedUtf8Text $temporaryStream (16 * 1024)",
            "Read-OwnedBoundedUtf8Text $temporaryStream (64 * 1024)",
            "$temporaryOwned -and $null -ne $temporaryIdentity",
            "Remove-VerifiedOwnedOutput $temporary $temporaryIdentity $BarrierSubstage",
            "finally",
        ):
            self.assertIn(marker, helper)
        for marker in (
            "GetFileInformationByHandle",
            "BY_HANDLE_FILE_INFORMATION",
            "VolumeSerialNumber",
            "FileIndexHigh",
            "DeleteOwnedFileIfIdentity",
            "SetFileInformationByHandle",
            "FILE_DISPOSITION_INFO",
            "FILE_FLAG_OPEN_REPARSE_POINT",
            "IdentityAvailable",
            "IdentityMatched",
            "OUTPUT_FINAL_IMAGE_VERIFY_FILE_IDENTITY_CHANGED",
            "OUTPUT_FINAL_IMAGE_VERIFY_FILE_IDENTITY_UNAVAILABLE",
            "expectedNativeKeys = @('ErrorCode', 'FileIndex', 'Succeeded', 'VolumeSerialNumber')",
            "expectedDeleteKeys = @('ErrorCode', 'IdentityAvailable', 'IdentityMatched', 'Succeeded')",
            "Test-OwnedFileIdentityEqual",
            "Assert-OwnedFileIdentity",
        ):
            self.assertIn(marker, self.text)
        self.assertNotIn("[IO.File]::ReadAllBytes($temporary)", helper)
        self.assertNotIn("[IO.File]::Delete($temporary)", helper)
        self.assertNotRegex(helper, r"if\s*\(\s*\$temporaryOwned[^\r\n]*Test-Path")
        self.assertLess(
            helper.index("Set-ProducerFailureContext 'OUTPUT_FINAL_IMAGE_VERIFY_OUTPUT_INVALID' $BarrierSubstage"),
            helper.index("Invoke-OwnedCommand"),
        )
        cleanup = helper[helper.rfind("finally") :]
        self.assertIn("Remove-VerifiedOwnedOutput $temporary $temporaryIdentity $BarrierSubstage", cleanup)
        open_body = powershell_function_body(self.text, "Open-VerifiedOwnedOutput")
        self.assertLess(open_body.index("Assert-OwnedFileIdentity"), open_body.index("return $stream"))
        delete_body = powershell_function_body(self.text, "Remove-VerifiedOwnedOutput")
        self.assertLess(delete_body.index("Assert-RegularFile $Path"), delete_body.index("DeleteOwnedFileIfIdentity"))
        self.assertLess(delete_body.index("Assert-NoReparseAncestors $Path"), delete_body.index("DeleteOwnedFileIfIdentity"))
        self.assertLess(delete_body.index("IdentityMatched"), delete_body.index("return $true"))

        # Publication is not committed until the summary has passed the same
        # payload-free boundary used for manifests and has been serialized.
        summary_position = producer.index("$summary = [ordered]@{")
        payload_free_position = producer.index("Assert-PayloadFreeManifest $summary", summary_position)
        success_json_position = producer.index("$successJson = $summary | ConvertTo-Json", payload_free_position)
        published_position = producer.index("$published = $true", success_json_position)
        script_published_position = producer.index("$script:Published = $true", published_position)
        self.assertLess(summary_position, payload_free_position)
        self.assertLess(payload_free_position, success_json_position)
        self.assertLess(success_json_position, published_position)
        self.assertLess(published_position, script_published_position)
        self.assertIn("Write-Output $successJson", self.text)
        self.assertNotIn("Write-Output ($successSummary | ConvertTo-Json", self.text)
        self.assertEqual(
            3,
            len(re.findall(r"\bAssert-QualifiedFinalImageVerificationEqual\b", producer)),
            "each qualified barrier must have an explicit identity equality",
        )

    def test_final_image_binding_is_python_owned_not_reimplemented_in_powershell(self) -> None:
        forbidden_functions = (
            "Normalize-Sha256",
            "ConvertTo-CanonicalJsonValue",
            "Resolve-RepositoryRelativePath",
            "Get-FinalImageStageSnapshot",
            "Get-QualifiedNativeFinalImageBinding",
        )
        for name in forbidden_functions:
            self.assertNotRegex(
                self.text,
                rf"(?im)^\s*function\s+{re.escape(name)}\b",
            )
            self.assertNotIn(name, self.text)
        _helper_name, helper = self._final_image_verify_helper()
        self.assertNotRegex(helper, r"\b(?:Get-Sha256|Get-TextSha256)\b")
        self.assertNotRegex(helper, r"(?i)Get-Content[^\r\n]*(?:receipt|stage)")
        self.assertIn("validate_bound_native_evidence_for_final_image", self.cli_text)

    def test_qualified_manifest_and_summary_expose_canonical_identities(self) -> None:
        producer = powershell_function_body(self.text, "Invoke-Producer")
        manifest = powershell_function_body(self.text, "New-BuildManifest")

        for alternatives in (
            ("qualifiedFinalImage",),
            ("buildTarget",),
            ("boundNativeEvidenceSha256", "nativeEvidenceSha256"),
            ("sourceNativeEvidenceSha256", "nativeEvidenceSourceSha256"),
            ("finalImageStage",),
            ("stageId", "finalImageStageId"),
            ("receiptPath", "receipt", "finalImageReceiptPath"),
            ("receiptSha256", "finalImageReceiptSha256"),
            ("exeSha256", "finalImageExeSha256"),
            ("exeSizeBytes", "finalImageExeSizeBytes"),
            ("mapSha256", "finalImageMapSha256"),
            ("mapSizeBytes", "finalImageMapSizeBytes"),
        ):
            self.assertTrue(
                any(field in manifest for field in alternatives),
                f"manifest is missing one of {alternatives}",
            )
        self.assertRegex(manifest, r"(?i)\bprovider(?:Summary)?\b")

        summary = self._summary_body(producer)
        for alternatives in (
            ("qualifiedFinalImage",),
            ("buildTarget",),
            ("boundNativeEvidenceSha256", "nativeEvidenceSha256"),
            ("sourceNativeEvidenceSha256", "nativeEvidenceSourceSha256"),
            ("stageId", "finalImageStageId"),
            ("receiptSha256", "finalImageReceiptSha256"),
            ("exeSha256", "finalImageExeSha256", "artifactSha256"),
            ("mapSha256", "finalImageMapSha256"),
            ("mapSizeBytes", "finalImageMapSizeBytes"),
        ):
            self.assertTrue(
                any(field in summary for field in alternatives),
                f"success summary is missing one of {alternatives}",
            )
        self.assertRegex(summary, r"(?i)\b(?:receiptPath|receipt|finalImageReceiptPath)\b")
        self.assertRegex(summary, r"(?i)\b(?:provider|providerSummary|finalImageProvider)\b")

    def test_nonqualified_build_path_remains_compatible_and_explicit(self) -> None:
        producer = powershell_function_body(self.text, "Invoke-Producer")
        manifest = powershell_function_body(self.text, "New-BuildManifest")
        qualified_build = re.search(
            r"\$buildCommandText\s*=\s*if\s*\(\s*\$QualifiedFinalImage\s*\)\s*\{(?P<qualified>.*?)\r?\n\s*\}\s*else\s*\{(?P<legacy>.*?)\r?\n\s*\}",
            producer,
            re.DOTALL,
        )
        self.assertIsNotNone(qualified_build)
        assert qualified_build is not None
        self.assertIn("buildBatch", qualified_build.group("legacy"))
        self.assertIn("build-dev.bat", self.text)
        self.assertIn("$manifest.buildTarget = 'Build'", manifest)
        self.assertIn("$manifest.qualifiedFinalImage = $false", manifest)
        self.assertIn("non-qualified", manifest)
        self.assertIn("if (-not $QualifiedFinalImage) { Assert-RegularFile $buildBatch }", producer)
        self.assertRegex(
            producer,
            r"(?i)qualifiedFinalImage\s*=\s*(?:\[[^\r\n\]]+\]\s*)?\$(?:QualifiedFinalImage|false)",
        )

    def test_qualified_stage_root_parameter_is_not_shadowed_by_cleanup_state(self) -> None:
        self.assertRegex(self.text, r"(?m)^\s*\[string\]\$FinalImageStageRoot,")
        self.assertNotRegex(self.text, r"\$script:FinalImageStageRoot(?:Owned)?\s*=")

    def test_qualified_cli_binding_reaches_bounded_preflight_without_build(self) -> None:
        hosts = powershell_hosts()
        if not hosts:
            self.skipTest("Neither powershell.exe nor pwsh is available")
        for shell in hosts:
            with self.subTest(shell=shell):
                with tempfile.TemporaryDirectory(prefix="sakura-output-startup-binding-") as directory:
                    token = Path(directory).name
                    owned_root = ROOT / "build" / "tmp" / f"producer-qualified-binding-{token}"
                    self.assertFalse(owned_root.exists())
                    relative_root = Path("build") / "tmp" / owned_root.name
                    relative_output = relative_root / "producer"
                    configuration_root = owned_root / "producer" / "Debug"
                    try:
                        completed = subprocess.run(
                            [
                                shell,
                                "-NoProfile",
                                "-NonInteractive",
                                "-ExecutionPolicy",
                                "Bypass",
                                "-File",
                                str(PRODUCER),
                                "-Backend",
                                "cpp",
                                "-OutputDirectory",
                                relative_output.as_posix(),
                                "-QualifiedFinalImage",
                                "-FinalImageStageRoot",
                                relative_root.as_posix(),
                            ],
                            cwd=ROOT,
                            capture_output=True,
                            text=True,
                            encoding="utf-8",
                            errors="replace",
                            timeout=30,
                            check=False,
                        )
                        self.assertEqual(1, completed.returncode, completed.stderr)
                        lines = [line for line in completed.stdout.splitlines() if line.strip()]
                        self.assertEqual(1, len(lines), completed.stdout)
                        failure = json.loads(lines[0])
                        self.assertEqual(1, failure["schemaVersion"])
                        self.assertEqual("failed", failure["status"])
                        self.assertEqual("preflight", failure["failure"]["stage"])
                        self.assertEqual("PRODUCER_PREFLIGHT", failure["failure"]["code"])
                        self.assertTrue(failure["payloadFree"])
                        # The stage-root argument is an ancestor of the producer root,
                        # so the retained parameter reaches the bounded relationship
                        # check after OutputDirectory creates this test-only path.  A
                        # shadowed parameter fails earlier and never creates it.
                        self.assertTrue(configuration_root.is_dir())
                        self.assertFalse((configuration_root / "cpp").exists())
                        self.assertFalse((configuration_root / ".cpp-transaction").exists())
                    finally:
                        if owned_root.exists():
                            self.assertTrue(owned_root.is_dir())
                            self.assertFalse(owned_root.is_symlink())
                            shutil.rmtree(owned_root)
                    self.assertFalse(owned_root.exists())

    def test_failure_cleanup_is_payload_free_and_holds_adoption(self) -> None:
        failure = powershell_function_body(self.text, "New-FailureEnvelope")
        producer = powershell_function_body(self.text, "Invoke-Producer")
        for marker in (
            "payloadFree = $true",
            "status = 'failed'",
            "status = 'not-published'",
            "cleanup-unverified",
            "remainingCount",
            "$failure.code",
            "$failure.substage",
            "Remove-OwnedDirectory",
            "if ($null -ne $failureType -or -not $cleanupVerified)",
        ):
            self.assertIn(marker, self.text)
        self.assertRegex(failure, r"(?i)\b(?:adoption|decision)\b")
        self.assertRegex(failure, r"(?i)\bHOLD\b")
        self.assertRegex(failure, r"(?i)adoptionEligible\s*=\s*\$false")
        self.assertRegex(producer, r"(?i)\$published\s*=\s*\$false")

    def test_native_exit_code_capture_and_typed_failure_contract_is_explicit(self) -> None:
        for marker in (
            "function Invoke-NativeOutputCapture",
            "$rawOutput = & $Executable @Arguments 2>$null",
            "$exitCode = $LASTEXITCODE",
            "function Convert-RustToolchainCaptureToIdentity",
            "RUST_TOOLCHAIN_IDENTITY_COMMAND_FAILED",
            "RUST_TOOLCHAIN_IDENTITY_MALFORMED",
            "rustToolchainNonzeroRejected",
            "rustToolchainMalformedRejected",
            "rustToolchainFailureCodeVerified",
            "rustToolchainExitZeroVerified",
            "primaryCode",
            "FailureSubstage",
        ):
            self.assertIn(marker, self.text)
        self.assertRegex(
            self.text,
            r"\$rawOutput = & \$Executable @Arguments 2>\$null\r?\n\s+\$exitCode = \$LASTEXITCODE",
        )
        self.assertNotRegex(
            self.text,
            r"\$rawOutput\s*=\s*& \$Executable @Arguments[^\r\n]*\|",
        )

    def test_self_test_is_bounded_and_does_not_launch_build(self) -> None:
        self.assertIn("$dirtySourceState", self.text)
        self.assertRegex(self.text, r"Assert-QualifiedSourceState\s+\$dirtySourceState")
        self.assertRegex(self.text, r"qualifiedDirtySourceRejected\s*=\s*\$qualifiedDirtySourceRejected")
        hosts = powershell_hosts()
        if not hosts:
            self.skipTest("Neither powershell.exe nor pwsh is available")
        for shell in hosts:
            completed = subprocess.run(
                [
                    shell,
                    "-NoProfile",
                    "-NonInteractive",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(PRODUCER),
                    "-SelfTest",
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=30,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            lines = [line for line in completed.stdout.splitlines() if line.strip()]
            self.assertEqual(1, len(lines), completed.stdout)
            self.assertTrue(lines[0].startswith("SELFTEST_JSON "), completed.stdout)
            payload = json.loads(lines[0][len("SELFTEST_JSON ") :])
            self.assertTrue(payload["passed"])
            self.assertTrue(payload["noBuildLaunched"])
            self.assertTrue(payload["selectorVerified"])
            self.assertTrue(payload["selectorProofVerified"])
            self.assertTrue(payload["qualifiedDirtySourceRejected"])
            self.assertTrue(payload["temporaryIdentityVerified"])
            self.assertTrue(payload["temporaryIdentityMismatchRejected"])
            self.assertTrue(payload["rustLtcgSelectorVerified"])
            self.assertTrue(payload["cppLtcgSelectorVerified"])
            self.assertTrue(payload["missingGlRejected"])
            self.assertTrue(payload["duplicateSelectorRejected"])
            self.assertTrue(payload["ambiguousSourceRejected"])
            self.assertTrue(payload["wrongConfigObjectFormatRejected"])
            self.assertTrue(payload["archiveExportsVerified"])
            self.assertTrue(payload["archiveExactSetRejected"])
            self.assertTrue(payload["runtimeStageVerified"])
            self.assertTrue(payload["manifestPayloadFreeVerified"])
            self.assertTrue(payload["boundedProcessOwnershipVerified"])
            self.assertTrue(payload["canonicalClosureVerified"])
            self.assertTrue(payload["nativeExitCodeCaptureVerified"])
            self.assertTrue(payload["rustToolchainExitZeroVerified"])
            self.assertTrue(payload["rustToolchainNonzeroRejected"])
            self.assertTrue(payload["rustToolchainMalformedRejected"])
            self.assertTrue(payload["rustToolchainFailureCodeVerified"])

    def test_invalid_selector_is_a_typed_payload_free_failure(self) -> None:
        hosts = powershell_hosts()
        if not hosts:
            self.skipTest("Neither powershell.exe nor pwsh is available")
        shell = hosts[0]
        with tempfile.TemporaryDirectory(prefix="sakura-output-startup-producer-") as directory:
            # The producer only permits evidence below the repository build
            # tree.  The unique path is used to prove that preflight fails
            # before it creates a transaction or starts a build.
            relative_output = f"build/tmp/producer-contract-{Path(directory).name}"
            completed = subprocess.run(
                [
                    shell,
                    "-NoProfile",
                    "-NonInteractive",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(PRODUCER),
                    "-Backend",
                    "auto",
                    "-OutputDirectory",
                    relative_output,
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=30,
                check=False,
            )
            self.assertEqual(1, completed.returncode, completed.stderr)
            lines = [line for line in completed.stdout.splitlines() if line.strip()]
            self.assertEqual(1, len(lines), completed.stdout)
            failure = json.loads(lines[0])
            self.assertEqual(1, failure["schemaVersion"])
            self.assertEqual("failed", failure["status"])
            self.assertEqual("preflight", failure["failure"]["stage"])
            self.assertEqual("preflight", failure["failure"]["type"])
            self.assertEqual("PRODUCER_PREFLIGHT", failure["failure"]["code"])
            self.assertEqual("preflight", failure["failure"]["substage"])
            self.assertTrue(failure["payloadFree"])
            self.assertNotIn(str(ROOT), completed.stdout)
            self.assertNotIn(str(ROOT), completed.stderr)
            self.assertFalse((ROOT / relative_output).exists())


if __name__ == "__main__":
    unittest.main()
