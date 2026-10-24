"""Release promotion ships the tested main-push Release build (Issue #376)."""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
BUILD_WORKFLOW = REPO_ROOT / ".github/workflows/build-sakura.yml"
PROMOTION_WORKFLOW = REPO_ROOT / ".github/workflows/release-promotion.yml"
RECORD_SCRIPT = REPO_ROOT / ".github/scripts/Write-ReleaseBuildRecord.ps1"
SELECT_SCRIPT = REPO_ROOT / ".github/scripts/Select-ReleasePackageSource.ps1"
IMPORT_SCRIPT = REPO_ROOT / ".github/scripts/Import-ReleaseBuildRecord.ps1"
CONTRACT_ENVIRONMENT = (
    "        SAKURA_UTF16_BACKEND: cpp\n",
    "        SAKURA_OUTPUT_BACKEND: rust\n",
    "        SAKURA_UTF16_PRODUCTION_PACKAGE: true\n",
    "        SAKURA_OUTPUT_PRODUCTION_PACKAGE: true\n",
)
CONTRACT_ARGUMENTS = (
    '-Utf16Backend "$env:SAKURA_UTF16_BACKEND"',
    '-OutputBackend "$env:SAKURA_OUTPUT_BACKEND"',
    '-Utf16ProductionPackage "$env:SAKURA_UTF16_PRODUCTION_PACKAGE"',
    '-OutputProductionPackage "$env:SAKURA_OUTPUT_PRODUCTION_PACKAGE"',
)
MAIN_PUSH_RELEASE = (
    "if: ${{ matrix.config == 'Release' && !inputs.release_promotion"
    " && github.event_name == 'push' && github.ref == 'refs/heads/main' }}"
)
REUSED_ARTIFACTS = (
    "Release build record x64 Release",
    "Installer x64 Release",
    "Exe x64 Release",
    "Log x64 Release",
    "Asm x64 Release",
    "Dev x64 Release",
)
SHIPPED_ARTIFACTS = {
    "Release provenance x64 Release",
    "Installer x64 Release",
    "Exe x64 Release",
    "Log x64 Release",
    "Asm x64 Release",
    "Dev x64 Release",
}
JOB_KEY = re.compile(r"^  [A-Za-z0-9_-]+:\n", re.MULTILINE)


def _read_powershell(path: Path) -> str:
    raw = path.read_bytes()
    if raw[:2] in (b"\xff\xfe", b"\xfe\xff"):
        return raw.decode("utf-16")
    return raw.decode("utf-8-sig")


def _job(workflow: str, name: str) -> str:
    start = workflow.index(f"\n  {name}:\n") + 1
    following = JOB_KEY.search(workflow, start + 1)
    return workflow[start : following.start() if following else len(workflow)]


def _step(job: str, name: str) -> str:
    start = job.index(f"    - name: {name}\n")
    following = job.find("\n    - name: ", start + 1)
    return job[start : following + 1 if following >= 0 else len(job)]


def _artifact_names(job: str, action: str) -> set[str]:
    names = set()
    for step in re.split(r"\n(?=    - name: )", job):
        if f"uses: actions/{action}@" in step:
            match = re.search(r"^        name: (.+)$", step, re.MULTILINE)
            if match:
                names.add(match.group(1))
    return names


def _powershell_hosts() -> tuple[str, str]:
    windows_powershell = shutil.which("powershell.exe")
    powershell = shutil.which("pwsh.exe")
    if windows_powershell is None or powershell is None:
        raise AssertionError("release package reuse contracts require powershell.exe and pwsh.exe")
    return windows_powershell, powershell


def _run_script(host: str, script: Path, *arguments: str, cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        (host, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", str(script), *arguments),
        cwd=cwd,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=120,
        check=False,
    )


class BuildRecordProducerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = BUILD_WORKFLOW.read_text(encoding="utf-8-sig")
        cls.build = _job(cls.workflow, "build")

    def test_only_a_main_push_release_job_records_its_build(self) -> None:
        record = _step(self.build, "Record tested Release build")
        self.assertIn(f"      {MAIN_PUSH_RELEASE}\n", record)
        self.assertIn(r".\.github\scripts\Write-ReleaseBuildRecord.ps1", record)
        for argument in (
            '-SourceSha "${{ env.SAKURA_BUILD_SOURCE_SHA }}"',
            '-Repository "${{ github.repository }}"',
            '-RunId "${{ github.run_id }}"',
            '-RunAttempt "${{ github.run_attempt }}"',
            '-EventName "${{ github.event_name }}"',
            '-Ref "${{ github.ref }}"',
            '-Platform "${{ matrix.platform }}"',
            '-Configuration "${{ matrix.config }}"',
            "-OutputPath release-build-record.json",
            *CONTRACT_ARGUMENTS,
        ):
            self.assertIn(argument, record)

    def test_record_expires_with_the_archives_it_describes(self) -> None:
        upload = _step(self.build, "Upload tested Release build record")
        self.assertIn(f"      {MAIN_PUSH_RELEASE}\n", upload)
        self.assertIn("uses: actions/upload-artifact@", upload)
        self.assertIn("        name: Release build record ${{ matrix.platform }} ${{ matrix.config }}\n", upload)
        self.assertIn("        path: release-build-record.json\n", upload)
        self.assertIn("        if-no-files-found: error\n", upload)
        self.assertNotIn("retention-days", upload)
        for name in ("Upload Installer", "Upload Exe", "Upload Log", "Upload Asm", "Upload Dev"):
            with self.subTest(step=name):
                self.assertNotIn("retention-days", _step(self.build, name))

    def test_record_describes_the_finished_archives(self) -> None:
        archives = self.build.index("    - name: zipArtifacts\n")
        record = self.build.index("    - name: Record tested Release build\n")
        upload = self.build.index("    - name: Upload tested Release build record\n")
        self.assertLess(archives, record)
        self.assertLess(record, upload)

    def test_reused_artifacts_are_the_ones_a_main_push_uploads(self) -> None:
        source = _read_powershell(SELECT_SCRIPT).replace("\r\n", "\n")
        body = source[source.index("function Get-ReusedArtifactNames {") :]
        body = body[: body.index("\n}\n")]
        self.assertEqual(REUSED_ARTIFACTS, tuple(re.findall(r"'([^']+)'", body)))
        uploaded = _artifact_names(self.build, "upload-artifact")
        for name in REUSED_ARTIFACTS:
            with self.subTest(artifact=name):
                self.assertIn(name.replace("x64 Release", "${{ matrix.platform }} ${{ matrix.config }}"), uploaded)


class ShippableBuildContractTests(unittest.TestCase):
    def test_every_build_that_can_ship_is_given_the_release_contract(self) -> None:
        build = _job(BUILD_WORKFLOW.read_text(encoding="utf-8-sig"), "build")
        reuse = _job(PROMOTION_WORKFLOW.read_text(encoding="utf-8-sig"), "reuse")
        for job, names in (
            (build, ("MSBuild", "MSBuild release promotion", "Record tested Release build", "Record release provenance")),
            (reuse, ("Lay out tested main-push package", "Record release provenance")),
        ):
            for name in names:
                step = _step(job, name)
                for assignment in CONTRACT_ENVIRONMENT:
                    with self.subTest(step=name, assignment=assignment.strip()):
                        self.assertEqual(1, step.count(assignment))


class PackageSourceSelectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = PROMOTION_WORKFLOW.read_text(encoding="utf-8-sig")
        cls.resolve = _job(cls.workflow, "resolve")

    def test_dispatch_rebuilds_only_on_request(self) -> None:
        inputs = self.workflow[self.workflow.index("  workflow_dispatch:\n") : self.workflow.index("\ndefaults:\n")]
        rebuild = inputs[inputs.index("      rebuild:\n") :]
        self.assertIn("        default: false\n", rebuild)
        self.assertIn("        type: boolean\n", rebuild)
        select = _step(self.resolve, "Select release package source")
        self.assertIn(
            "        REBUILD_REQUESTED: ${{ github.event_name == 'workflow_dispatch'"
            " && inputs.rebuild && 'true' || 'false' }}\n",
            select,
        )
        self.assertIn('-RebuildRequested "$env:REBUILD_REQUESTED"', select)

    def test_selection_follows_the_source_gate_and_is_kept_as_evidence(self) -> None:
        order = [
            self.resolve.index(f"    - name: {name}\n")
            for name in (
                "Validate tag source and required main gate",
                "Select release package source",
                "Export release package source",
                "Upload release package source",
            )
        ]
        self.assertEqual(sorted(order), order)
        select = _step(self.resolve, "Select release package source")
        self.assertIn(r".\.github\scripts\Select-ReleasePackageSource.ps1", select)
        self.assertIn("-SourceEvidencePath release-source.json", select)
        self.assertIn("-OutputPath release-package-source.json", select)
        for output in ("package_mode", "artifact_run_id", "artifact_ids"):
            self.assertIn(f"      {output}: ${{{{ steps.package.outputs.{output} }}}}\n", self.resolve)
        upload = _step(self.resolve, "Upload release package source")
        self.assertIn("        name: Release package source\n", upload)
        self.assertIn("        retention-days: 30\n", upload)

    def test_exactly_one_package_path_reaches_smoke_and_publish(self) -> None:
        self.assertIn("    if: ${{ needs.resolve.outputs.package_mode == 'rebuild' }}\n", _job(self.workflow, "package"))
        self.assertIn("    if: ${{ needs.resolve.outputs.package_mode == 'reuse' }}\n", _job(self.workflow, "reuse"))
        smoke = _job(self.workflow, "smoke")
        self.assertIn("    needs: [resolve, package, reuse]\n", smoke)
        condition = smoke[smoke.index("    if: >-\n") + len("    if: >-\n") : smoke.index("    # The shipped")]
        self.assertEqual(
            "${{ !cancelled() && needs.resolve.result == 'success' && "
            "((needs.resolve.outputs.package_mode == 'reuse' && needs.reuse.result == 'success') || "
            "(needs.resolve.outputs.package_mode == 'rebuild' && needs.package.result == 'success')) }}",
            " ".join(line.strip() for line in condition.splitlines()),
        )
        publish = _job(self.workflow, "publish")
        self.assertIn("    needs: [resolve, package, reuse, smoke]\n", publish)
        self.assertIn(
            "    if: ${{ !cancelled() && needs.smoke.result == 'success'"
            " && (github.event_name == 'push' || inputs.publish) }}\n",
            publish,
        )
        self.assertEqual(1, len(re.findall(r"(?m)^ +contents: write$", self.workflow)))
        self.assertIn("      contents: write\n", publish)


class ReuseJobTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = PROMOTION_WORKFLOW.read_text(encoding="utf-8-sig")
        cls.reuse = _job(cls.workflow, "reuse")

    def test_reuse_is_read_only_and_runs_only_trusted_helpers(self) -> None:
        self.assertIn("    permissions:\n      actions: read\n      contents: read\n    steps:\n", self.reuse)
        source = _step(self.reuse, "Checkout release source")
        self.assertIn("        ref: ${{ needs.resolve.outputs.source_sha }}\n", source)
        self.assertIn("        fetch-depth: 0\n", source)
        self.assertNotIn("path:", source)
        helper = _step(self.reuse, "Checkout release provenance helper")
        self.assertIn("        ref: ${{ github.workflow_sha }}\n", helper)
        self.assertIn("        path: release-orchestration\n", helper)
        self.assertLess(self.reuse.index(source), self.reuse.index(helper))
        # The workspace root is the target source; none of its scripts may run.
        self.assertNotIn(r".\.github\scripts", self.reuse)
        self.assertIn(r".\release-orchestration\.github\scripts\Import-ReleaseBuildRecord.ps1", self.reuse)
        self.assertIn(r".\release-orchestration\.github\scripts\Write-ReleaseProvenance.ps1", self.reuse)

    def test_reuse_downloads_the_selected_run_by_artifact_id(self) -> None:
        download = _step(self.reuse, "Download tested main-push package")
        for line in (
            "        artifact-ids: ${{ needs.resolve.outputs.artifact_ids }}\n",
            "        run-id: ${{ needs.resolve.outputs.artifact_run_id }}\n",
            "        repository: ${{ github.repository }}\n",
            "        github-token: ${{ github.token }}\n",
        ):
            self.assertIn(line, download)
        self.assertNotRegex(download, r"(?m)^        name:")
        self.assertNotIn("merge-multiple", download)

    def test_reuse_lays_out_then_proves_the_package_with_the_exact_contract(self) -> None:
        layout = _step(self.reuse, "Lay out tested main-push package")
        provenance = _step(self.reuse, "Record release provenance")
        self.assertLess(self.reuse.index(layout), self.reuse.index(provenance))
        for step in (layout, provenance):
            for argument in ("-Platform x64", "-Configuration Release", *CONTRACT_ARGUMENTS):
                with self.subTest(argument=argument):
                    self.assertIn(argument, step)
        self.assertIn("-SourceRoot .", layout)
        self.assertIn('-SourceSha "${{ needs.resolve.outputs.source_sha }}"', provenance)
        self.assertIn("        RELEASE_TAG: ${{ needs.resolve.outputs.release_tag }}\n", provenance)
        self.assertIn("-OutputPath release-provenance.json", provenance)

    def test_both_package_paths_ship_the_same_artifacts(self) -> None:
        uploaded = _artifact_names(self.reuse, "upload-artifact")
        self.assertEqual(SHIPPED_ARTIFACTS, uploaded)
        build_uploads = {
            name.replace("${{ matrix.platform }} ${{ matrix.config }}", "x64 Release")
            for name in _artifact_names(_job(BUILD_WORKFLOW.read_text(encoding="utf-8-sig"), "build"), "upload-artifact")
        }
        self.assertLessEqual(SHIPPED_ARTIFACTS, build_uploads)
        published = _artifact_names(_job(self.workflow, "publish"), "download-artifact")
        smoked = _artifact_names(_job(self.workflow, "smoke"), "download-artifact")
        self.assertEqual(SHIPPED_ARTIFACTS, published - {"Release source evidence"})
        self.assertLessEqual(smoked - {"Release source evidence"}, SHIPPED_ARTIFACTS)
        for step in re.split(r"\n(?=    - name: )", self.reuse):
            if "uses: actions/upload-artifact@" in step:
                with self.subTest(step=step.splitlines()[0]):
                    self.assertIn("        if-no-files-found: error\n", step)


class PackageScriptExecutableTests(unittest.TestCase):
    def test_both_powershell_hosts_pass_the_script_self_tests(self) -> None:
        for host in _powershell_hosts():
            for script in (SELECT_SCRIPT, IMPORT_SCRIPT):
                with self.subTest(host=host, script=script.name):
                    completed = _run_script(host, script, "-SelfTest", cwd=REPO_ROOT)
                    self.assertEqual(0, completed.returncode, completed.stdout + completed.stderr)
                    self.assertIn("self-test passed", completed.stdout)

    def _write_build(self, host: str, root: Path) -> tuple[Path, str, dict[str, bytes]]:
        git = shutil.which("git")
        if git is None:
            raise AssertionError("the build record round trip requires git")
        build = root / "build"
        build.mkdir()
        subprocess.run((git, "init", "-q", str(build)), check=True)
        (build / "README").write_text("fixture\n", encoding="utf-8")
        subprocess.run((git, "-C", str(build), "add", "README"), check=True)
        subprocess.run(
            (git, "-C", str(build), "-c", "user.name=fixture", "-c", "user.email=fixture@example.invalid",
             "commit", "-q", "-m", "fixture"),
            check=True,
        )
        sha = subprocess.run(
            (git, "-C", str(build), "rev-parse", "HEAD"), capture_output=True, text=True, check=True
        ).stdout.strip()

        # Any PE image with version resources stands in for the product; the
        # installer carries an overlay so the two hashes differ.
        image = Path(_powershell_hosts()[0]).read_bytes()
        files = {
            "x64/Release/sakura.exe": image,
            "installer/Output-x64/sakura_install3-1-0-7900-x64.exe": image + b"installer overlay",
        }
        for relative, content in files.items():
            (build / relative).parent.mkdir(parents=True, exist_ok=True)
            (build / relative).write_bytes(content)
        prefix = "sakura-build7900-abcdef123-x64-Release-alpha"
        entries = {
            "Installer": {"Installer/sakura_install3-1-0-7900-x64.exe": files["installer/Output-x64/sakura_install3-1-0-7900-x64.exe"], "Installer/warning.txt": b"w"},
            "Exe": {"EXE/sakura.exe": image, "EXE/terminal-tools/tmux.exe": b"t"},
            "Log": {"Log/msbuild-x64-Release.log": b"l"},
            "Asm": {"Asm/CEditView.asm": b"a"},
            "Dev": {"DEV/sakura.pdb": b"p"},
        }
        for kind, members in entries.items():
            with zipfile.ZipFile(build / f"{prefix}-{kind}.zip", "w") as archive:
                for member, content in members.items():
                    archive.writestr(member, content)

        completed = _run_script(
            host, RECORD_SCRIPT,
            "-SourceSha", sha, "-Repository", "owner/repo", "-RunId", "700", "-RunAttempt", "2",
            "-EventName", "push", "-Ref", "refs/heads/main", "-Platform", "x64", "-Configuration", "Release",
            "-Utf16Backend", "cpp", "-OutputBackend", "rust", "-Utf16ProductionPackage", "true",
            "-OutputProductionPackage", "true", "-OutputPath", "release-build-record.json",
            cwd=build,
        )
        self.assertEqual(0, completed.returncode, completed.stdout + completed.stderr)

        download = root / "download"
        for artifact, pattern in (
            ("Release build record x64 Release", "release-build-record.json"),
            *((f"{kind} x64 Release", f"{prefix}-{kind}.zip") for kind in entries),
        ):
            (download / artifact).mkdir(parents=True)
            shutil.copy2(build / pattern, download / artifact / pattern)
        (root / "package-source.json").write_text(
            json.dumps({
                "schema": 1, "repository": "owner/repo", "source_sha": sha, "mode": "reuse",
                "run": {"id": 700, "attempt": 2},
            }),
            encoding="utf-8",
        )
        expected = {relative: content for relative, content in files.items()}
        for kind in entries:
            expected[f"{prefix}-{kind}.zip"] = (build / f"{prefix}-{kind}.zip").read_bytes()
        return download, prefix, expected

    def _import(self, host: str, root: Path, download: Path, source: Path, output_backend: str = "rust") -> subprocess.CompletedProcess[str]:
        return _run_script(
            host, IMPORT_SCRIPT,
            "-PackageSourcePath", str(root / "package-source.json"), "-DownloadRoot", str(download),
            "-SourceRoot", str(source), "-Platform", "x64", "-Configuration", "Release",
            "-Utf16Backend", "cpp", "-OutputBackend", output_backend, "-Utf16ProductionPackage", "true",
            "-OutputProductionPackage", "true",
            cwd=root,
        )

    def test_the_import_accepts_exactly_what_the_build_recorded(self) -> None:
        for host in _powershell_hosts():
            with self.subTest(host=host), tempfile.TemporaryDirectory() as temporary_directory:
                root = Path(temporary_directory)
                download, prefix, expected = self._write_build(host, root)

                source = root / "source"
                source.mkdir()
                completed = self._import(host, root, download, source)
                self.assertEqual(0, completed.returncode, completed.stdout + completed.stderr)
                for relative, content in expected.items():
                    self.assertEqual(content, (source / relative).read_bytes(), relative)

                mismatched = root / "mismatched-contract"
                mismatched.mkdir()
                completed = self._import(host, root, download, mismatched, output_backend="cpp")
                self.assertNotEqual(0, completed.returncode)
                self.assertEqual([], list(mismatched.iterdir()))

                # A payload that differs from the record after extraction is
                # rejected last, so the import must remove what it laid out.
                for role in ("application", "installer"):
                    edited = root / f"download-{role}"
                    shutil.copytree(download, edited)
                    record_path = edited / "Release build record x64 Release" / "release-build-record.json"
                    record = json.loads(record_path.read_text(encoding="utf-8-sig"))
                    record["payload"][role]["sha256"] = "0" * 64
                    record_path.write_text(json.dumps(record), encoding="utf-8")
                    rejected = root / f"rejected-{role}"
                    rejected.mkdir()
                    completed = self._import(host, root, edited, rejected)
                    self.assertNotEqual(0, completed.returncode)
                    self.assertIn(f"Reused {role} SHA-256", completed.stdout + completed.stderr)
                    self.assertEqual([], list(rejected.iterdir()), role)

                exe_archive = download / "Exe x64 Release" / f"{prefix}-Exe.zip"
                tampered_bytes = bytearray(exe_archive.read_bytes())
                tampered_bytes[-1] ^= 0xFF
                exe_archive.write_bytes(bytes(tampered_bytes))
                tampered = root / "tampered-archive"
                tampered.mkdir()
                completed = self._import(host, root, download, tampered)
                self.assertNotEqual(0, completed.returncode)
                self.assertIn("SHA-256", completed.stdout + completed.stderr)
                self.assertEqual([], list(tampered.iterdir()))


if __name__ == "__main__":
    unittest.main()
