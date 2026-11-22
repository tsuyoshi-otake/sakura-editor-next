# Rust document model migration design

Issue #425 (P1-1 of #267) · Source baseline `8ace1ede9903e522556b4353e546bcde13281c12`

Use an **indexed sequence of logical lines containing raw UTF-16 code units** for
P1. Adopt Rust text, undo, layout, and session state together when creating a
new document session. Move file codecs through their own bulk-operation gate
first. `CEditDoc` remains the native integration point during P1. Process
inversion belongs to P2 and native view/UI migration to P3. Preserve real VS Code
behavior at the Workbench boundary and freeze the existing Sakura editing and
encoding behavior before porting it. Any observable change needs separate
specification review. [Repository rule](../CLAUDE.md#highest-priority-rule-real-vs-code-compatibility-not-a-look-alike)
(`CLAUDE.md:3–45`) · [Phase plan and amendment](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/267)
· [Design issue](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/425)

## 1. Current-state inventory

Thread affinity below describes the inspected production paths. A claim of
general thread safety or complete caller coverage is **unverified**.

| Component | Owner, lifetime, and thread | Mutation entry points and read APIs |
| --- | --- | --- |
| `CEditDoc` and `CDocEditor` | `CEditApp` owns the native document through `unique_ptr`, injects it into native and Workbench adapters, and resets it at teardown. The document contains the line manager, layout manager, file metadata, file operations, editor, type and agents by value. Creation and normal command flow are on the editor process's window thread. (`sakura_core/CEditApp.h:130`, `sakura_core/CEditApp.cpp:235–275,315–367`, `sakura_core/_main/CNormalProcess.cpp:652–661,958–963`, `sakura_core/doc/CEditDoc.h:110–126`) | `Create`, `InitDoc`, `Clear`, `SetFilePathAndIcon`, `SetDocumentEncoding` and `HandleCommand` are aggregate entry points. `Clear` unlocks the file and clears history and logical lines. `CDocEditor` owns dirty state, input EOL, and the undo buffer, and supplies line-append helpers. Readers use `GetDocument`/`GetEditDoc`, the public collaborators, `GetDocumentEncoding`, `GetDocumentBomExist`, `GetSaveInfo` and `GetEditInfo`. (`sakura_core/doc/CEditDoc.cpp:137–153,235–257,276–305,417–505,585–616`, `sakura_core/doc/CDocEditor.h:42–74`, `sakura_core/doc/CDocEditor.cpp:41–53,203–217`) |
| `CDocLineMgr` and `CDocLine` | The manager owns a doubly linked list of pooled nodes and frees them on removal/destruction. A node owns its `CNativeW`, EOL and line marks. The live manager is used by the editor thread and has mutable lookup/iterator cursors even on reads. File-read workers construct separate managers sharing the allocation resource, then the caller joins and splices the lists in file order. (`sakura_core/doc/logic/CDocLineMgr.h:147–182`, `sakura_core/doc/logic/CDocLineMgr.cpp:55–164,276–357`, `sakura_core/doc/logic/CDocLine.h:99–135`, `sakura_core/env/CReadManager.cpp:181–264`) | Interactive replacement enters `CSearchAgent::ReplaceData` via the view or layout, with `ScopedDocMutation` collecting a precise change. Bulk `AddNewLine`, `InsertNewLine`, `DeleteLine`, `DeleteAllLine` and `AppendAsMove` can emit full invalidation. Nodes expose `SetDocLineString[Move]`, `SetEol` and mutable `_GetDocLineData`. Read APIs include indexed `GetLine`, count, head/tail and next/prev, explicit-length spans and EOL-specific lengths, plus revision and `CollectTextEdits`. `CDocReader` wraps the same line pointers and also has a CRLF-normalized whole-text helper. (`sakura_core/view/CEditView_Command_New.cpp:803–830`, `sakura_core/doc/layout/CLayoutMgr_New2.cpp:24–74`, `sakura_core/agent/CSearchAgent.cpp:762–865,950–1038`, `sakura_core/doc/logic/CDocLineMgr.cpp:72–265`, `sakura_core/doc/logic/CDocLine.h:51–117`, `sakura_core/doc/CDocReader.cpp:13–126`) |
| `CLayoutMgr` and `CLayout` | `CEditDoc` owns the primary manager. Its layout nodes borrow text from `CDocLine` at a logical offset and are destroyed by the manager. Full layout can give disjoint line ranges to temporary worker managers and join them on the initiating thread. Range-based color state selects the serial path. Native command, caret and paint consumers use the published layout on the editor thread. (`sakura_core/doc/CEditDoc.cpp:168–178`, `sakura_core/doc/layout/CLayout.h:60–110`, `sakura_core/doc/layout/CLayoutMgr.cpp:63–113,390–418`, `sakura_core/doc/layout/CLayoutMgr_DoLayout.cpp:422–515,520–575`, `sakura_core/view/CEditView_Paint.cpp:704–717,1227–1235`) | `SetLayoutInfo`, `_DoLayout`, `ReplaceData_CLayoutMgr`, `DoLayout_Range` and width recalculation update derived rows, color continuation and widths. The replacement adapter first translates the range and invokes logical replacement. Readers use `SearchLineByLayoutY`, `GetLineStr`, row traversal, `GetDocLineRef` and `LogicToLayout` / `LayoutToLogic[Ex]`. (`sakura_core/doc/layout/CLayoutMgr.cpp:117–215,222–320,436–459,798–1040`, `sakura_core/doc/layout/CLayoutMgr_New.cpp:202–315`, `sakura_core/doc/layout/CLayoutMgr_New2.cpp:24–86`, `sakura_core/doc/layout/CLayoutMgr_DoLayout.cpp:666–783`) |
| `CDocFileOperation` and `CDocFile` | Embedded in `CEditDoc`, with the operation adapter borrowing its owner. The editor thread owns picker, lock, listener and macro/plugin sequencing. `CLoadAgent` and `CSaveAgent` call scoped readers/writers. Read partitioning is worker-local as above. The save path traverses the live document on the caller and pumps progress. (`sakura_core/doc/CEditDoc.h:114–124`, `sakura_core/doc/CDocFileOperation.h:26,80–83`, `sakura_core/doc/CDocFileOperation.cpp:81–119,126–183,344–413`, `sakura_core/agent/CLoadAgent.cpp:235–308`, `sakura_core/agent/CSaveAgent.cpp:89–105`, `sakura_core/env/CWriteManager.cpp:52–136`) | `FileLoad` / `DoLoadFlow`, `ReloadCurrentFile`, `DoSaveFlow` / `FileSave[As]`, `PrepareFileClose` / `CommitFileClose` and `FileCloseOpen` drive effects and finalization. Read surfaces include last save/close terminal, encoding, BOM, path and file time. The load barrier aggregates all listeners before publishing success. Save obtains line payloads in order and updates file metadata through the save agent. (`sakura_core/doc/CDocFileOperation.cpp:126–243,344–482,501–630`, `sakura_core/doc/CDocFileOperation.h:58–73`, `sakura_core/doc/CDocListener.cpp:96–123`, `sakura_core/doc/CDocFile.h:43–60`, `sakura_core/agent/CSaveAgent.cpp:89–117`, `sakura_core/env/CWriteManager.cpp:58–136`) |

### Buffers and undo

| Component | Owner, lifetime, and thread | Mutation and read APIs |
| --- | --- | --- |
| `CMemory` | The object owns raw allocation, with copy of data and move by swap. Capacity changes can invalidate borrowed addresses. Buffers follow their caller, for example per-conversion locals in save and per-line buffers in each read job. Exclusive mutation is inferred from those uses and the exposed mutable byte pointer. (`sakura_core/mem/CMemory.cpp:39–61,124–166,189–250,265–288`, `sakura_core/env/CWriteManager.cpp:62–74,93–116`, `sakura_core/env/CReadManager.cpp:353–397`) | `SetRawData[HoldBuffer]`, `AppendRawData`, allocation, length changes, clear and swap mutate the buffer. `GetRawPtr` and `GetRawLength` report byte storage and explicit byte length. (`sakura_core/mem/CMemory.h:45–76`, `sakura_core/mem/CMemory.cpp:189–250,265–288`) |
| `CNative` and `CNativeW` | `CNative` is a protected `CMemory` base wrapper, and `CNativeW` derives from it. A `CDocLine` embeds its own `CNativeW`. Temporary `CNativeW` instances also have local lifetimes. `CStringRef` borrows the pointer/length pair and does not extend the buffer lifetime. Affinity follows the owning live line or local conversion job. (`sakura_core/mem/CNative.h:15–29`, `sakura_core/mem/CNativeW.h:17–56,82–92`, `sakura_core/mem/CNativeW.cpp:32–35,57–64`, `sakura_core/doc/logic/CDocLine.h:121–123`, `sakura_core/env/CReadManager.cpp:353–397`) | `CNative::Clear` and `CNativeW::SetString`, `SetStringHoldBuffer`, `AppendString`, `Replace` and mutable `GetStringPtr` can change content. `GetStringLength` divides bytes by `sizeof(wchar_t)`. Read helpers for character span, width and next/previous boundary operate on an explicit pointer and length. (`sakura_core/mem/CNative.cpp:11–14`, `sakura_core/mem/CNativeW.h:60–92`, `sakura_core/mem/CNativeW.cpp:72–115,335–383,394–497`) |
| `COpe` records and `COpeBlk` | `COpe` variants contain logical caret positions, owned insert/delete/replace payloads (wide text and per-line sequence), or caret-only data. The commander builds a reference-counted block during the UI command. A block deletes its owned operation pointers and ownership passes to the buffer when the outer grouping scope releases a nonempty block. (`sakura_core/cmd/COpe.h:24–111`, `sakura_core/cmd/COpeBlk.h:29–66`, `sakura_core/cmd/COpeBlk.cpp:22–65`, `sakura_core/cmd/CViewCommander.cpp:649–691`, `sakura_core/view/CEditView.cpp:3035–3058`) | Callers construct and fill variants, `AppendOpe` records them, and `GetNum` / `GetOpe` enumerate for replay. The editor view builds replacement operations, clears selection and applies the resulting caret. (`sakura_core/cmd/COpeBlk.cpp:42–70`, `sakura_core/view/CEditView_Command_New.cpp:775–830,870–942`) |
| `COpeBuf` | Embedded in `CDocEditor`. The buffer owns committed blocks, the current history cursor, and the last clean index. Read and write paths inspected are on the editor thread. (`sakura_core/doc/CDocEditor.h:69–74`, `sakura_core/cmd/COpeBuf.h:45–67`, `sakura_core/cmd/COpeBuf.cpp:23–40`, `sakura_core/view/CEditView.cpp:3035–3040`) | `AppendOpeBlk` drops the redo tail, `ClearAll` resets, and `SetNoModified` records the save point. `IsEnableUndo/Redo`, `GetCurrentPointer`, `GetNextSeq` and `GetNoModifiedSeq` read state. `DoUndo/Redo` advance the cursor and return borrowed blocks and the modified flag. The commander replays undo in reverse and redo forward. (`sakura_core/cmd/COpeBuf.h:45–58`, `sakura_core/cmd/COpeBuf.cpp:45–131`, `sakura_core/cmd/CViewCommander_Edit.cpp:318–350,579–616`) |

### File codec boundary

`CFileLoad` picks a codec or autodetects it before preparing partition-local
converters. `CFileLoad::ReadLine` converts from the mapped byte input and
`GetNextLineOffset` aligns partitions for the chosen encoding. `CWriteManager`
owns a converter for the save and writes converted bytes. The factory also
selects UTF-7, UTF-8, CESU-8, UTF-16, EUC and other encodings. Preserve the
factory's supported modes and flags when migrating the load/save pipeline.
(`sakura_core/io/CFileLoad.cpp:155–167,203–221,250–268,303–397,423–445`,
`sakura_core/env/CWriteManager.cpp:27–47,58–136`,
`sakura_core/charset/CCodeFactory.cpp:25–52`)

| Component | Owner, lifetime, and thread | Mutation and read APIs |
| --- | --- | --- |
| `CESI` | A `CCodeMediator` detection invocation constructs an instance with its encoding configuration. Detection is on the file-open caller before read workers start. (`sakura_core/charset/CCodeMediator.cpp:25–50`, `sakura_core/charset/CESI.h:69–108`, `sakura_core/io/CFileLoad.cpp:203–221`, `sakura_core/env/CReadManager.cpp:159–223`) | `CheckKanjiCode` checks BOM, scans bytes and evaluates candidate encodings. `SetInformation` / `SetEvaluation` populate instance evidence and `GetEvaluation` / `GetDataLen` read it. `DetectUnicodeBom` reads a byte span. (`sakura_core/charset/CESI.h:87–123,227–248`, `sakura_core/charset/CESI.cpp:70–143,638–670,1233–1269`) |
| `codechecker` | Free classifiers consume caller-owned spans with no document lifetime. They run in detection and conversion paths on the caller or a partition worker with its own inputs and outputs. (`sakura_core/charset/codechecker.h:395–434`, `sakura_core/charset/CESI.cpp:180–204,408–477`, `sakura_core/charset/CShiftJis.cpp:108–148`, `sakura_core/env/CReadManager.cpp:214–223,353–397`) | `CheckSjisChar`, `DetectJisEscseq`, `_CheckJisAnyPart`, `CheckUtf16leChar/CheckUtf16beChar`, `CheckUtf8Char[2]` and `CheckCesu8Char` report consumed length/type or parse position/error through return values and caller out parameters. They read source, writing the requested classification or scan state. (`sakura_core/charset/codechecker.h:395–434`, `sakura_core/charset/codechecker.cpp:127–167,246–362,426–498,639–710,740–789`) |
| `CCodePage` | Factory instance holds the code-page ID. Conversion runs in scoped converter instances with caller-owned buffers. Windows code-page enumeration has a separate process-local list used by UI and uses a static initialization path. (`sakura_core/charset/CCodeFactory.cpp:39–52`, `sakura_core/charset/CCodePage.h:37–74`, `sakura_core/charset/CCodePage.cpp:345–416`, `sakura_core/io/CFileLoad.cpp:155–167`) | `CodeToUnicode` / `UnicodeToCode` use `CPToUnicode` / `UnicodeToCP` to size/fill destination buffers, using Windows conversion wrappers or special-case implementations. Read/query APIs include encoding traits, code-page names and enumeration. Preserve effective ACP/OEM and flags, substitution and failure results. (`sakura_core/charset/CCodePage.h:39–69`, `sakura_core/charset/CCodePage.cpp:76–195,206–339,430–455`) |
| `CJis` and `CShiftJis` | Factory-owned JIS instance carries the MIME/Base64 option. Shift-JIS delegates to static converters. Both use operation-local inputs/output, with separately prepared converter instances on load workers. (`sakura_core/charset/CCodeFactory.cpp:25–33`, `sakura_core/charset/CJis.h:15–38`, `sakura_core/charset/CShiftJis.h:18–32`, `sakura_core/io/CFileLoad.cpp:155–167`) | `JISToUnicode` / `UnicodeToJIS` and `SJISToUnicode` / `UnicodeToSJIS` write destination buffers and return conversion status. JIS scans escape-shifted blocks and uses the option during decode. Shift-JIS exposes `GetSizeOfChar` and uses Japanese mapping routines. Both have `UnicodeToHex` for consumers of code information. (`sakura_core/charset/CJis.cpp:66–126,244–284,445–475`, `sakura_core/charset/CShiftJis.cpp:28–60,108–151,218–252`, `sakura_core/charset/CShiftJis.h:50–115`) |

The existing C++ `editor::document::DocumentSession` is a separate
`std::string`-based implementation with its own undo and position projection.
The inspected Working Copy capture adapter constructs it locally, collects
the live document, encodes strict UTF-8 and imports the result before returning
a captured snapshot. This does not establish a second native editable
document. Use the existing Editor Core identity and lifecycle contract and
replace its text projection at the adapter boundary, preserving the current
persisted format. (`sakura_core/workbench/editor/document/DocumentSession.cpp:18–56,60–136,139–188`,
`sakura_core/workbench/editor/persistence/CEditDocWorkingCopyPersistenceAdapter.cpp:116–154,169–192,382–404`,
`sakura_core/workbench/editor/CLAUDE.md:42–79,114–125`)

The Editor Core owns input identity, group membership and active selection.
The same canonical resource can share a Working Copy under the pure contract.
The current native bridge projects one backing document. Preserve the
independent identity, content version and persistence fence through save and
close, the zero-input state, and the typed `Unsupported` terminal on native
Revert until its full rollback contract is verified. Use the existing
`EditorCommandIds.h` command IDs at the adapter boundary.
(`sakura_core/workbench/editor/CLAUDE.md:42–79,138–169`)

## 2. Document text reader fan-in

Counts are lexical match occurrences in `sakura_core` C++ source/headers at
the stated revision, grouped by immediate directory under `sakura_core`.
Each match has a file and line via the read-only command below. Counts include
wrapper forwarding, and one source line can contain two matches. A receiver
filter disambiguates generic names such as `GetLine`, `GetLineStr` and `GetPtr`.
These are counts of the listed source forms. Runtime frequencies, conditionally
compiled reachability, and full semantic resolution of aliases, templates,
unqualified calls and indirect wrappers are **unverified**. Omitted directories have zero matches for that form. The two unqualified `CLayoutMgr::GetLineStr`
calls inside `LayoutToLogicEx` and the unqualified `LayoutToLogicEx` call
inside `LayoutToLogic` are additional internal uses
(`sakura_core/doc/layout/CLayoutMgr.cpp:969,995,1038`).

| Logical API or wrapper | Total | Counts by directory |
| --- | ---: | --- |
| `CDocLineMgr::GetLine` | 104 | `_main` 1, `agent` 10, `cmd` 26, `doc` 11, `docplus` 14, `macro` 2, `outline` 3, `parse` 2, `recent` 1, `types` 15, `view` 13, `window` 6 |
| `CDocLineMgr::GetLineCount` | 83 | `agent` 2, `cmd` 27, `dlg` 3, `doc` 15, `docplus` 1, `env` 2, `macro` 2, `outline` 1, `types` 13, `view` 10, `window` 6, `workbench` 1 |
| `GetDocLineTop` | 19 | `cmd` 1, `doc` 8, `docplus` 4, `env` 3, `outline` 2, `workbench` 1 |
| `GetDocLineBottom` | 4 | `cmd` 3, `doc` 1 |
| `CDocLine::GetNextLine` | 52 | `agent` 5, `cmd` 3, `doc` 25, `docplus` 11, `env` 4, `outline` 2, `view` 1, `workbench` 1 |
| `CDocLine::GetPrevLine` | 23 | `agent` 5, `cmd` 3, `doc` 11, `docplus` 3, `view` 1 |
| `GetDocLineStrWithEOL` | 46 | `agent` 7, `cmd` 6, `doc` 6, `docplus` 3, `macro` 1, `outline` 1, `types` 15, `view` 5, `window` 2 |
| `GetDocLineStrWithEOL_Safe` | 4 | `cmd` 1, `view` 2, `window` 1 |
| `GetStringRefWithEOL` | 5 | `cmd` 1, `doc` 3, `print` 1 |
| `GetStringRefWithEOL_Safe` | 1 | `view` 1 |
| `CDocLine::GetPtr` | 32 | `agent` 2, `cmd` 5, `doc` 9, `env` 2, `parse` 2, `print` 1, `view` 8, `window` 2, `workbench` 1 |
| `CMemoryIterator generic GetPtr` | 2 | `mem` 2 |
| `_GetDocLineDataWithEOL` | 6 | `agent` 2, `cmd` 1, `env` 3 |
| `CDocReader::GetLineStr` | 1 | `doc` 1 |
| `GetLineStrWithoutEOL` | 2 | `view` 2 |
| `CDocReader::GetAllData` | 1 | `cmd` 1 |
| `GetFirstLinrStr` | 0 | — |
| `GetNextLinrStr` | 0 | — |
| `GetTextRevision` | 1 | `view` 1 |
| `CollectTextEdits` | 1 | `view` 1 |

| Layout API or wrapper | Total | Counts by directory |
| --- | ---: | --- |
| `SearchLineByLayoutY` | 67 | `cmd` 35, `doc` 2, `macro` 1, `print` 3, `types` 1, `view` 24, `window` 1 |
| `CLayoutMgr::GetLineStr` | 41 | `cmd` 7, `view` 33, `window` 1 |
| `GetTopLayout` | 5 | `doc` 4, `view` 1 |
| `GetBottomLayout` | 7 | `doc` 1, `view` 6 |
| `GetNextLayout` | 54 | `cmd` 9, `doc` 32, `view` 13 |
| `GetPrevLayout` | 25 | `cmd` 1, `doc` 15, `print` 1, `view` 8 |
| `GetDocLineRef` | 27 | `cmd` 15, `doc` 2, `print` 3, `view` 7 |
| `CLayout::GetPtr` | 10 | `cmd` 6, `print` 1, `view` 3 |
| `LayoutToLogic` | 50 | `cmd` 20, `macro` 1, `view` 27, `window` 2 |
| `LayoutToLogicEx` | 5 | `window` 5 |
| `LogicToLayout` | 101 | `_main` 1, `cmd` 53, `doc` 1, `macro` 1, `outline` 1, `plugin` 1, `recent` 1, `types` 18, `view` 22, `window` 2 |

<details>
<summary>Reproduce the counts and inspect source locations</summary>

Run from the repository root in PowerShell. Remove `-o` for the surrounding
source line. Ripgrep exit code 1 means zero matches.

```powershell
$apis = [ordered]@{
    'CDocLineMgr::GetLine' = '\b(?:(?:cDocLineMgr|m_cDocLineMgr|m_pcDocLineMgr|pcDocLineMgr|docMgr|lineManager|lines|m_mgr|rDocLineMgr|refDocLineMgr)\s*(?:->|\.)|static_cast<const CDocLineMgr\*>\(this\)->)\s*GetLine\s*\('
    'CDocLineMgr::GetLineCount' = '\b(?:(?:cDocLineMgr|m_cDocLineMgr|m_pcDocLineMgr|pcDocLineMgr|docMgr|lineManager|lines|m_mgr|rDocLineMgr|refDocLineMgr)\s*(?:->|\.)|static_cast<const CDocLineMgr\*>\(this\)->)\s*GetLineCount\s*\('
    'GetDocLineTop' = '(?:->|\.)\s*GetDocLineTop\s*\('
    'GetDocLineBottom' = '(?:->|\.)\s*GetDocLineBottom\s*\('
    'CDocLine::GetNextLine' = '(?:->|\.)\s*GetNextLine\s*\('
    'CDocLine::GetPrevLine' = '(?:->|\.)\s*GetPrevLine\s*\('
    'GetDocLineStrWithEOL' = '(?:->|\.)\s*GetDocLineStrWithEOL\s*\('
    'GetDocLineStrWithEOL_Safe' = '\bCDocLine::GetDocLineStrWithEOL_Safe\s*\('
    'GetStringRefWithEOL' = '(?:->|\.)\s*GetStringRefWithEOL\s*\('
    'GetStringRefWithEOL_Safe' = '\bCDocLine::GetStringRefWithEOL_Safe\s*\('
    'CDocLine::GetPtr' = '(?:\b(?:pDocLine|pCDocLine|pcDocLine|m_pCDocLine|docLine|line|pLine|first|pcCurDocLine)|\b(?:GetDocLineRef|GetNextLine|GetPrevLine)\(\))\s*->\s*GetPtr\s*\('
    'CMemoryIterator generic GetPtr' = '\bpcT\s*->\s*GetPtr\s*\('
    '_GetDocLineDataWithEOL' = '(?:->|\.)\s*_GetDocLineDataWithEOL\s*\('
    'CDocReader::GetLineStr' = '\bCDocReader\(.*?\)\.GetLineStr\s*\('
    'GetLineStrWithoutEOL' = '(?:->|\.)\s*GetLineStrWithoutEOL\s*\('
    'CDocReader::GetAllData' = '(?:->|\.)\s*GetAllData\s*\('
    'GetFirstLinrStr' = '(?:->|\.)\s*GetFirstLinrStr\s*\('
    'GetNextLinrStr' = '(?:->|\.)\s*GetNextLinrStr\s*\('
    'GetTextRevision' = '(?:->|\.)\s*GetTextRevision\s*\('
    'CollectTextEdits' = '(?:->|\.)\s*CollectTextEdits\s*\('
    'SearchLineByLayoutY' = '(?:->|\.)\s*SearchLineByLayoutY\s*\('
    'CLayoutMgr::GetLineStr' = '\b(?:m_cLayoutMgr|pLayoutMgr)\s*(?:->|\.)\s*GetLineStr\s*\('
    'GetTopLayout' = '(?:->|\.)\s*GetTopLayout\s*\('
    'GetBottomLayout' = '(?:->|\.)\s*GetBottomLayout\s*\('
    'GetNextLayout' = '(?:->|\.)\s*GetNextLayout\s*\('
    'GetPrevLayout' = '(?:->|\.)\s*GetPrevLayout\s*\('
    'GetDocLineRef' = '(?:->|\.)\s*GetDocLineRef\s*\('
    'CLayout::GetPtr' = '\b(?:pLayout|pcLayout)\s*->\s*GetPtr\s*\('
    'LayoutToLogic' = '(?:->|\.)\s*LayoutToLogic\s*\('
    'LayoutToLogicEx' = '(?:->|\.)\s*LayoutToLogicEx\s*\('
    'LogicToLayout' = '(?:->|\.)\s*LogicToLayout\s*\('
}
foreach ($api in $apis.GetEnumerator()) {
    $matchesByApi = @(rg --no-heading --with-filename -n -o -g '*.{cpp,h,hpp,inl}' $api.Value sakura_core)
    if ($LASTEXITCODE -gt 1) { throw "rg failed for $($api.Key)" }
    '{0}: {1}' -f $api.Key, $matchesByApi.Count
    $matchesByApi | Group-Object { ($_ -split '[\\/]')[1] } |
        Sort-Object Name | Select-Object Name, Count
}
```

</details>

- **Viewport and commands.** Paint and printing consume wrapped rows and
  sometimes borrow the backing logical line. Commands use both coordinates
  and text for movement, selection, and clipboard operations. Macro and
  window-message surfaces also expose logical line reads. Serve batches of
  visible rows and a revisioned logical neighborhood. Keep local row and
  character iteration in C++ during P1.
  (`sakura_core/view/CEditView_Paint.cpp:704–717,1227–1235`,
  `sakura_core/print/CPrintPreview.cpp:1389,1526–1527`,
  `sakura_core/cmd/CViewCommander_Clipboard.cpp:514,748,845`,
  `sakura_core/macro/CMacro.cpp:1522–1535`,
  `sakura_core/window/CEditWnd.cpp:14496–14510`)
- **Whole file and range scans.** Save and encoding checks walk from the
  head. Language-specific outline implementations use indexed reads,
  `CDocOutline` also delegates through its text source, and search iterates
  line spans with explicit lengths. Batch logical ranges for legacy parsers
  and move the literal scan over contiguous units behind a coarse operation.
  (`sakura_core/env/CWriteManager.cpp:58–136`,
  `sakura_core/env/CCodeChecker.cpp:29–65`,
  `sakura_core/types/CType_Html.cpp:84,291`,
  `sakura_core/doc/CDocOutline.cpp:62–78,241–249,546–550`,
  `sakura_core/agent/CSearchAgent.cpp:633–728`)
- **Versioned readers and metadata.** TextMate paint reads the change log
  before requesting lines. Working Copy capture walks every logical node and
  encodes the assembled text. Bookmarks, modified sequence and diff marks
  also depend on the logical nodes. Offer changed ranges or full
  invalidation, a revision-pinned streaming capture, and a separate batched
  metadata payload for callers that need it.
  (`sakura_core/view/CEditView_Paint.cpp:875–956`,
  `sakura_core/workbench/editor/persistence/CEditDocWorkingCopyPersistenceAdapter.cpp:169–192`,
  `sakura_core/doc/logic/CDocLine.h:124–132`,
  `sakura_core/docplus/CBookmarkManager.cpp:21–32,45–61`,
  `sakura_core/docplus/CModifyManager.cpp:20–32`,
  `sakura_core/docplus/CDiffManager.cpp:118–136`)

## 3. Observable equivalence and existing tests

Test names below were verified by source inspection. Execution and coverage
beyond their assertions are **unverified**. Paths in the test column are
relative to `src/test/cpp/tests1/`.

| Contract to freeze | Existing tests | Gaps to close against the C++ reference |
| --- | --- | --- |
| Logical text, indexing and revision. Preserve empty/EOF representation, split/join, inclusive/exclusive endpoints, range deltas, invalidation on a missed or full record, and revision stability on rewrap. (`sakura_core/agent/CSearchAgent.cpp:762–865,1094–1243`, `sakura_core/doc/logic/CDocLineMgr.cpp:169–265,276–357`, `sakura_core/doc/CLAUDE.md:24–39`) | `test-cdoclinemgr.cpp:12,124` — `CDocLineMgr.ListManipulations`, `RandomAccess`. `test-cdocline.cpp:42,79,102,139,209` — `CDocLine.GetLengthWithoutEOL`, `GetLengthWithEOL`, `GetDocLineStrWithEOL`, `GetStringRefWithEOL`, `SetDocLineStringMove`. `test-csearchagent.cpp:103–469,828–1109` — `CSearchAgent.ReplaceData1` through `ReplaceData10`, `CDocLineMgrTextEditLog.InsertAfterFinalEol`, `DeleteFinalEolRecordsTwoLines`, `OverflowYieldsFullInvalidation`, `PushFailureClearsTheLogSoConsumersSeeAGap`. `textmate/TextMateLineCacheTest.cpp:691,873,882` — `TextMateLineCacheTest.ApplyEditsFromRealReplaceDataMarksExpectedLines`, `RandomRealEditsMatchFromScratchOracle`, `RandomRealEditsMatchFromScratchOracleRealGrammar`. | Golden replay covering revision and line metadata after actual commands, recovery and rewrap. Worker/caller publication boundaries and pointer-to-snapshot migration. |
| Undo/redo grouping and dirty state. Preserve nested command grouping, empty blocks, multi-edit replay ordering, redo truncation, save-point dirty flags and per-line change sequences. (`sakura_core/cmd/COpeBuf.cpp:61–131`, `sakura_core/view/CEditView.cpp:3035–3058`, `sakura_core/cmd/CViewCommander_Edit.cpp:318–350,579–616`, `sakura_core/doc/CDocEditor.cpp:124–131`, `sakura_core/docplus/CModifyManager.cpp:20–32`) | `test-csearchagent.cpp:62,70,78,91` — `CDeleteOpe.Dump001`, `CInsertOpe.Dump001`, `COpeBlk.AppendOpe001`, `AppendOpe101`. `workbench/SenpReadonlyWorkbenchTest.cpp:284,427` — `SenpReadonlyWorkbench.NativeSwitchRetainsSelectionScrollAndExecutableUndo`, `RealCEditDocTextAndUndoBufferSurviveReadonlyLifetime`. | No dedicated full command grouping matrix located. Freeze typing, paste, replace-all, macro/nested dispatch, box edits, repeat counts, edit-after-undo and save followed by undo/redo before cutover. |
| Caret and selection. Preserve logical offset, visual row/column and desired column, direction/anchor, virtual space, selection clearing, and pane behavior across edits/undo/redo. `CViewSelect` owns the range and `SelectionSession` owns its anchor. (`sakura_core/view/CEditView_Command_New.cpp:775–830,870–942`, `sakura_core/cmd/CViewCommander_Edit.cpp:335–350,445–482,596–616,720–732`, `sakura_core/workbench/editor/CLAUDE.md:12–29`) | `workbench/SenpReadonlyWorkbenchTest.cpp:284` — `SenpReadonlyWorkbench.NativeSwitchRetainsSelectionScrollAndExecutableUndo` covers retention at a surface switch. `test-cnative.cpp:774,781,887,901` — `CNativeW.GetSizeOfChar_SurrogatePair`, `GetSizeOfChar_IVS`, `GetCharNext`, `GetCharPrev` cover underlying boundaries. | No direct native caret/selection trace of a grouped command, undo and redo located. Include box/reversed ranges, tabs, wide characters, surrogate pairs, IVS, wrap boundaries, EOF and inactive panes. |
| EOL and file metadata. Preserve mixed CR/LF/CRLF and existing extended NEL/LS/PS policy, trailing EOL/EOF, BOM/encoding, and the first logical line's EOL when present, with the legacy fallback otherwise. Ordinary save requests no normalization. Explicit EOL save normalizes live text as an undo-recorded pre-save action and restores the view position through the visitor. (`sakura_core/doc/logic/CDocLine.cpp:46–77`, `sakura_core/io/CFileLoad.cpp:250–268,423–445,471–598`, `sakura_core/doc/CDocEditor.cpp:98–112`, `sakura_core/doc/CDocFileOperation.cpp:424–461`, `sakura_core/agent/CSaveAgent.cpp:81–105`, `sakura_core/doc/CDocVisitor.cpp:15–89`) | `test-ceol.cpp:17,50,91` — `CEol.CEol`, `CEolTest.test`, `DetectEolTest.test`. `test-ccodebase.cpp:640,692` — `GetBomTest.test`, `GetEolTest.test`. `test-file.cpp:965,1190,1227` — `CFileLoadScannerBounds.Utf32CrLfPairRemainsOneEol`, `FileLoadOptionsTest.PreparedReaderPreservesExtendedEolBoundaries`, `ExtendedEolPolicyIsStableUntilReopen`. | End-to-end mixed-EOL byte round trip across load partitions and save groups, with/without final EOL or BOM. Freeze save normalization followed by cancel, failure and undo. |
| Raw code units and encoding. Preserve length-bearing NUL, valid pairs and unpaired UTF-16 units through the model. The Sakura converter maps raw bytes to U+DC00–U+DCFF where its mode does so and reverses that mapping where supported. Strict Working Copy UTF-8 capture has a separate contract. Preserve per-codec distinctions between success, loss and failure. (`sakura_core/doc/logic/CDocLine.cpp:62–76`, `sakura_core/charset/CCodeBase.h:101–120`, `sakura_core/charset/CUtf8.cpp:66–103,164–185`, `sakura_core/workbench/editor/persistence/CEditDocWorkingCopyPersistenceAdapter.cpp:116–154,174–192`) | `test-ccodebase.cpp:155,220,266,326,387,420,453,497,530,563,596` — `CCodeBase.codeSJis`, `codeJis`, `codeEucJp`, `codeLatin1`, `codeUtf8`, `codeUtf8_OracleImplementation` (CESU-8), `codeUtf7`, `codeUtf16Le`, `codeUtf16Be`, `codeUtf32Le`, `codeUtf32Be`. `test-codechecker.cpp:10,24` — `ConvertToUtf32.SurrogatePair`, `BinaryOnSurrogate`. `test-file.cpp:924,981,1104,1123,1164,1180,1209,1343` — `CFileLoadScannerBounds.Utf32ShortInputLengthsAreSafeAndPreserved`, `EbcdicTrailingMarkerDoesNotReadPastBuffer`, `DiffFileLoadRouteTest.Utf32LeAndBeReadValidLinesAndShortTails`, `EbcdicValidLinesAndTrailingMarkerUseDiffReader`, `FileLoadOptionsTest.MimeOptionIsAppliedOnEachOpenAndReopen`, `AutoDetectionUsesConstructionSnapshot`, `PreparedUtf7ReaderResetsPreviousDecodedLineOffset`, `PreparedReadersOwnConvertersWithInheritedMimeOptions`. `test-csearchagent.cpp:576` — `CSearchAgent.Utf16DispatchCallerFindsFirstLastNulSurrogateAndMultipleMatches`. | All 256 escape bytes, malformed suffixes and isolated high and low units in insert/edit/undo/save, plus detection ties, code-page flag/locale fixtures, MIME switches and strict Working Copy capture of malformed native units. |
| Layout and wrap. Preserve wrap row starts/lengths, EOL display width and EOF row, tab/TSV placement, indentation, word wrap and Japanese kinsoku, logical ↔ visual maps, and the resulting caret and scroll. Rewrap leaves logical revision intact. (`sakura_core/doc/layout/CLayout.h:60–99`, `sakura_core/doc/layout/CLayoutMgr.cpp:493–525,798–1040`, `sakura_core/doc/layout/CLayoutMgr_New.cpp:36–154`, `sakura_core/doc/layout/CLayoutMgr_DoLayout.cpp:162–399,520–575,666–783`, `sakura_core/doc/CLAUDE.md:37–39`) | `test-ctextmetrics.cpp:16,29,315–409` — `IndentDecorationLayout.ProjectsVisualColumnsIntoEditorLayoutUnits`, `IndentGuideLayout.MeasuresLeadingSpacesAndTabsInVisualColumns`, `CTextMetrics.GenerateDxArray1` through `GenerateDxArray8`, `CalcTextWidth`, `CalcTextWidth2`. `test-cnative.cpp:792,840` — `CNativeW.GetKetaOfChar`, `GetHabaOfChar`. These exercise metrics and unit helpers. | Full/incremental layout row and mapping oracle **unverified**. Add a matrix of fonts, widths, ambiguous ranges and punctuation, coloring across a range, multi-pane changes, cancellation, and worker/serial equivalence. |
| File and Working Copy transactions. Preserve final load barrier, lossy view mode, partial failure cleanup, staged recovery, save/close cancellation and stale generation fences. A failed replacement after native load starts fail-closes the Core and retains its recovery artifact. (`sakura_core/doc/CDocFileOperation.cpp:131–183,508–630`, `sakura_core/agent/CLoadAgent.cpp:364–399`, `sakura_core/doc/CLAUDE.md:43–79`) | `agent/LoadFinalizationTest.cpp:43,50` — `LoadFinalization.MapsEveryReadTerminalWithoutPromotingFailure`, `CallsEveryListenerAndAggregatesFailureAndExceptions`. `workbench/EditorWorkingCopyCoordinatorTest.cpp:133,152,175,575` — `EditorWorkingCopyCoordinator.DirtySavePublishesCleanOnlyAfterBackendSuccess`, `LateSaveCompletionCannotCleanANewerEdit`, `CancelledAndFailedSaveLeaveCoreUntouched`, `ClosePreparationIsAtomicAndLastCloseProducesTheLegitimateZeroInputState`. `workbench/CEditDocWorkingCopyPersistenceAdapterTest.cpp:128,176,209,286,321` — `CEditDocWorkingCopyPersistenceAdapterTest.NamedDocumentRoundTripsWithoutReadingDisk`, `CaptureProjectsLegacyLogicalTextThroughDocumentCoreWithoutMutation`, `InvalidUtf8FailsPrepareWithoutMutatingTarget`, `NonInertTargetRejectsPrepareAndCommitCannotOverwriteIt`, `AbortPreparedDiscardsTheStagedRecoveryDocument`. | Freeze full agent/writer errors with on-disk bytes, notifications and dirty/clean state. Include worker failure, cancellation, replacement failure, supersession and recovery after forced termination. |

## 4. Text representation decision

Let `L` be the logical line count and `N` the number of UTF-16 units. Costs are
design estimates. Performance at product callers is **unverified**.

| Option | Fit and trade-off |
| --- | --- |
| Keep a logical line list | Keep each line contiguous, with its exact EOL suffix and line metadata. A direct linked-list port would keep O(L) cold random seeks even though adjacent access is cheap (current lookup walks from a cached position or an end in `sakura_core/doc/logic/CDocLineMgr.cpp:276–357`). Bounded blocks and an order-statistic index give O(log L) seek, O(N + L) streaming, and local updates to affected blocks. A single giant line remains a worst case for replacement copies. |
| Piece tree over raw `u16` | Original/add buffers and subtree counts suit large insertions, random edits and immutable revision roots. Text of one logical line can span several pieces. Search, Unicode boundaries, EOL classification and layout then need boundary-aware scans or assembly. Piece fragmentation and retention of dead add-buffer ranges need limits and measurement. Line metadata and affinity when split/join become an independent indexing problem. |
| Rope over raw `u16` | Balanced chunks enable cheap splits, revision sharing and long-line edits with bounded leaf copies. It requires chunk-aware CRLF/surrogate handling and a logical line index. Making whole-line borrows contiguous shifts cost into scans, layout and adapters unless those consumers become chunk-aware. A UTF-8 string rope would also impose a different validity and coordinate model. |

**Recommend the indexed logical line list.** Use bounded blocks of line
records and a balanced index carrying line count and UTF-16 length. Keep
physical EOL units in each line buffer and record the EOL kind/length when the
line is classified. Preserve the existing empty/terminal line behavior through
the projection API. Store owned `u16` without Unicode normalization. Layout
and plugin adapters can add terminators to local staging buffers when needed.
Keep bookmarks, diff and modified marks with the line records or in a
session-owned side table whose updates follow the same split/join transaction.

Sequential spans feed existing scan kernels and codec/layout batches
directly. Build concatenable blocks in parallel on file load and merge the
results in input order without re-copying the whole document. For an edit,
seek once, replace the affected lines, update only affected blocks and index
paths, and publish the existing invalidation shape. Pin immutable block
references for capture and layout. Retain changed text for undo as grouped
deltas and share untouched blocks across revisions where the gate supports
it. An incremental pane refresh fetches only the affected or newly exposed
range. Benchmark single-line, highly fragmented, very large and repeated
random edits before choosing block capacity. If the long-line or memory gate
fails, bring the data back to the owner before changing the representation.

## 5. Rust crates and transitional C ABI

The native workspace already uses domain libraries, a strict Unicode leaf and
one `sakura_native_ffi` archive. Keep its lockfile and audit separate from
SENP. Use the existing immutable CPU policy handoff before SIMD use.
Start with workspace code and standard-library types. Any new dependency needs
its locked provenance, release-age and audit evidence under the applicable
repository and #267 rules. The
UTF-16 production package is currently C++ and the Rust comparison path
exists. G0 disposition remains a prerequisite. (`rust/native/README.md:3–41`,
`rust/native/Cargo.toml:1–20`, `src/main/msbuild/sakura-rust-core.targets:11–15,55–64,84–95`,
`src/main/cmake/sakura.cmake:757–768,788–851,928–936`,
`.github/workflows/build-sakura.yml:274–280,573–575`)
[Phase gates](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/267)

| Proposed package under `rust/native` | Responsibility and dependency |
| --- | --- |
| `sakura_document_codec` (rlib) | Detector and file conversion policy, encoded EOL split, per-format binary/loss rules and explicit bounded operation state. Use `sakura_unicode_core` only for shared strict primitives and `sakura_simd` for admitted kernels. Windows-specific mappings use the host boundary in batches until the P2 binding decision. |
| `sakura_document_text` (rlib) | Line storage, position and range types, raw UTF-16, immutable read snapshots, line metadata transforms, revision/delta log and transactional text replacement. Literal scan runs over spans through the admitted native kernels. |
| `sakura_document_layout` (rlib) | Derived row index, wrap and coordinate conversion. Consume text snapshots and typed settings, color-state and width inputs. Text storage never depends on layout or native views. |
| `sakura_document` (rlib) | `DocumentSession` owns text, grouped undo/redo, clean save point, operation generation, and publication of derived layout for the current revision/settings. Undo is a module of the session transaction so cursor and text commit together. It consumes the three lower packages. The save coordinator owns publication of file metadata only after the host's matching result. |
| `sakura_native_ffi` (existing staticlib) | Sole export, handle validation, panic policy and any paired deallocation. Application and test binaries each link one archive. No domain crate exports a C ABI. |

**Versioned operation families.** The names describe proposed signatures.
Have C++ own picker, platform file access/locking, recovery-store I/O,
listener/command routing, native rendering and per-view caret/selection.
C++ consumes committed values and owns the resulting native effects.
A Rust session is the sole owner of its logical text, history and derived
layout after adoption. C++ caches immutable derived values by session,
text revision and layout settings/metrics generation.

| Operation | Exchange |
| --- | --- |
| Create, stop, destroy | Creation binds a typed opaque session handle to its owner thread and one provider. Pass options and explicit initial state. Stop ends admission and invalidates pending publish tickets. Destroy retires the handle after outstanding owned jobs have drained or released their pinned data. |
| Load and save | Negotiate format and options once per operation. Supply one mapped span or bounded volume-sized batches, with an explicit end-of-input call for streams. Decode complete partitions or bounded batches into staged line blocks. Make state carry across chunk boundaries explicit, including CRLF, incomplete units and any JIS/UTF-7 mode behavior established by the reference. For save, encode from a pinned revision or from packed logical blocks while C++ owns the model. Return byte buffers and terminal/loss information. The host retains responsibility for final persistence and finalization; acknowledge the existing save terminal for the matching generation before marking clean. Submit save-time EOL edits as a batch at the corresponding legacy phase. |
| Edit, undo, redo | One command-level transaction accepts ordered replacements and caret/group information. Return a terminal result, new revision, line/mark deltas, history state, and logical caret effects for the C++ selection/view policy. For nested or interactive commands that need intermediate read-after-edit results, use an explicit bounded group and batch each meaningful edit phase, with one completion owner. Undo/redo each replay a whole group within Rust, with an ordered summary of effects required by native callers. Match the observed iteration and revision sequence where consumers depend on it. |
| Read, scan, capture | Two-pass caller-owned buffers for a range of logical lines or window rows, with packed UTF-16, offset/length/EOL arrays, optional marks and revision receipt. Snapshot handles pin immutable source for bounded chunk reads and carry no C++ pointer. Search accepts a whole bounded range and query, returns typed positions/matches, and handles cross-span semantics in the owning operation. Legacy regex, parser and plugin adapters can iterate local batches. A one-line user query may read from a neighborhood batch, while line-by-line, row-by-row and character-by-character FFI loops are prohibited. |
| Layout and mapping | Submit revision, changed range or full invalidation, wrap policy, and batch of resolved metric inputs. Fetch row ranges or map an array of requested positions in one call. Resolve any required font/width/shape cache misses on the C++ owner in bulk and resume with generation-matched results. Stateful color or metric behavior that cannot be represented by this boundary blocks layout cutover until its contract is verified. |

**Boundary contract.** Use fixed-width fields, `struct_size`, `abi_version`
and zeroed reserved fields. Lengths/capacities are explicit in bytes, UTF-16
units or records. Specify zero-based logical and visual coordinates separately.
Validate bounds, multiplication, alignment, token provenance and thread
ownership before touching caller memory. Null is valid only for explicitly
empty spans or the sizing pass. A short or stale fill reports the required
size or a typed stale status with no partially published output. Revision
receipts allow a measure/fill pair to refer to the same immutable data, with
bounded retention and explicit release. Input pointers are borrowed for the
duration of the call. Retained inputs are owned copies or transferred through
an explicitly paired ownership contract. No C++/Rust containers, exceptions,
unwinds or raw legacy node pointers cross the ABI.

Mutable calls are owner-thread-only and reject reentrancy. Detached workers
consume owned immutable inputs and publish results tagged with session,
operation, revision and settings generation. Limit in-flight work and discard
stale or cancelled results at the commit boundary. Hosts can poll a bounded
completion queue, with at most a `noexcept` wake-up if needed. Never hold a
Rust lock across I/O, a wake-up or a host call. Cancellation/Stop retires the
operation and produces one terminal outcome. Null/stale/foreign/double
destroy returns a typed handle status. After Stop only status, outstanding
release and destroy are legal. Every export contains unwind and reports
`InternalError`. A panic during stateful work poisons the affected handle
unless transactional no-change was proven. Diagnostic read and destruction
remain legal; existing independently owned read snapshots can release
through their own handles. OOM, abort and double panic are process-fatal.
Use one staticlib allocator/deallocator pair for exceptional owned
outputs. Diagnostics contain status, operation and safe lengths/hashes,
with document, path and recovery payloads excluded.
[Phase ABI, authority and evidence rules](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/267)

## 6. Ordered migration and production authority

Complete P0 and G0–G2 before implementation. Require the relevant URI identity,
filesystem/version, search and Working Copy/storage contracts before adopting
the dependent surface. Record `authority_domain`, mode, provider by context,
lifecycle owner, thread affinity and permissions in schema v4. Use copied
synthetic input or immutable replay logs for shadow comparison. The shadow
has no external I/O, publication or repair authority. See
`src/main/modules/schema-v4.json:84–98,117–124,162–171` and the
[phase gates and authority rules](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/267).

| Order | Candidate | Shadow | Cutover | Bake | C++ removal |
| --- | --- | --- | --- | --- | --- |
| 1. Charset codec and encoded EOL (A2/A3) | Freeze detection, format coverage, partition boundaries and byte/error corpus. Implement whole-buffer or batched decode/encode and EOL scanning. Bring up UTF-8/CESU-8 and aligned UTF-16/32 scan cells first inside the file codec. Keep ASCII widening internal. Preserve the supported legacy code pages and option paths. | Replay the actual load/save bytes and per-format options against frozen outputs. Compare bytes/units, line offsets, status, loss, and boundary cases with no file effects. | Select an approved codec at file-operation creation in each approved context. The I/O adapter still owns files and finalizers. Route legacy native conversion clients through batched adapters before retiring their providers. | Track per-format digests, failures, loss/cancellation, throughput and package behavior for the agreed interval. | Delete adopted C++ algorithm/provider paths after the interval and after their consumers and contexts have migrated. Retain distinct Win32 mapping adapters and review any still-required legacy providers explicitly. |
| 2. Logical line model | Depends on `1's decoded line-block contract. Implement the indexed line sequence, marked lines, bulk assembly, edit log, range reads and raw-unit scan. Inventory writer paths including save EOL and recovery. | Compare copied edit and load traces, revisions, metadata and TextMate invalidation against C++. Exercise old caller adapters against staged snapshots. | Use Rust lines as the only text provider at step 5. Through step 4 they are candidate state. | Track capture, edit/range/scan cost, stale revisions and memory during the session bake. | Remove C++ line storage and mutators after step 5 bake and caller migration. Eliminate raw `CDocLine` dependencies in adapters and retained range owners. |
| 3. Undo and redo | Depends on `2's typed edits and changed ranges. Implement grouping, history cursor, saved position, replay and explicit group terminal results in the session crate. | Replay command groups, cancellation, saved checkpoint, caret and line sequence against native traces. | Switch history with the text at step 5. A session commits text and its history under one owner. | Compare history, dirty/clean, caret and redo-branch outcome throughout the session bake. | Remove `COpe`, `COpeBlk` and `COpeBuf` from migrated contexts once the session and command adapters no longer reference them. |
| 4. Layout and wrap | Depends on `2 and the C++ metrics, color and view boundary. Build controlled full/incremental layouts from revisioned text and settings. Include undo/edit deltas from `3. | Compare row tuples, mappings and cancellation against native serial and parallel layout. Observe actual-caller copy/metrics/ABI cost. | Switch derived layout with text and history at step 5, keyed by the same text revision and layout generation. | Verify geometry, repaint, and layout hot-path thresholds during the session bake. | Remove `CLayoutMgr`'s retired algorithms and node ownership after step 5 bake and range/snapshot adoption. Keep C++ render/metrics adapters through their own owning phases. |
| 5. `CEditDoc` session cutover | Requires the codec and steps 2–4 to pass their respective tiers. Compose the Rust session behind the native facade and existing Editor Core and Working Copy boundaries. Wire load/save/edit/close/recovery so each operation has one terminal and stale work cannot publish. | Replay full application flows, working-copy generations and notifications without external effects. Verify the rendered editor on controlled paired builds before adoption. | Independent adoption change at new session/process creation. The Rust provider owns text, history, and layout in the selected context. Legacy views read through revisioned batches and apply only committed effects. Existing sessions finish with their original provider. | Measure the approved contexts through the agreed bake and exercise rollback with a restarted comparison build. No runtime fallback or dual-write. | Delete the retired C++ document session provider, remaining live legacy text/undo/layout ownership and obsolete facade internals after the bake gate. Hand remaining Win32 integration to the later phases. |

For MinGW, #303 makes the GNU Rust archive/candidate available and the
amended policy keeps C++ production unless separately adopted. Keep the
matching corpus in each supported context. MSVC adoption cannot retire the
only MinGW provider. The owner's MinGW decision below is therefore on the
final deletion path. Rollback before removal selects the previous reviewed
build/provider at the same creation boundary and restores from the
existing working-copy contract. It involves no in-flight handle swaps.
[MinGW amendment and rollback contract](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/267)

## 7. Verification and adoption gates

The source and test inventory is read-only evidence for #425. Test execution,
benchmarks, and all gates below are **unverified** at this design revision.
For implementation Issues, add the proposed
`tools/verify-document-migration.ps1` runner as a reviewable prerequisite.
Its invocation contract below uses PowerShell from the repository root. Each
check exits nonzero on a missing artifact, timeout, empty test selection,
missing context/cell, digest mismatch or gate failure. It takes artifact
paths and preregistration from a run manifest supplied by `-RunManifest`. Put
the manifest and transient receipts under an isolated task directory
(`$env:DOC_MIGRATION_RUN` points to the manifest). The manifest pins revision,
baseline/candidate artifacts and digests, supported contexts, corpus, seeds,
numeric gates, suite filters, host data and timeouts. CI and test owners use
the in-repository profile isolation and timeout guidance (`src/test/CLAUDE.md:100–129`).
Check for surviving runners/editor children at the end of every run with
parent-first cleanup, then assert an empty scoped job/process set. Execute
UI checks in an isolated interactive session.

### Hot-path tier

Preregister by operation × ISA × approved toolchain/context before taking
measurements. Require exact result digests and end-to-end median regression
≤2% and p95 regression ≤5%. For performance-motivated SIMD cells require
≥5% weighted-median improvement, the 95% CI excluding parity, the chosen
no-material-reversal threshold, and explicit evidence for AVX-512 against
AVX2. Record startup initialization regression ≤2% and ≤1 ms absolute.
Record clean/incremental build and link time and binary size; any regression
above 5% requires a separate approved exception. Collect at least seven
independent process runs per comparison on both AMD and Intel with fixed
affinity where applicable, CPU/microcode/OS/toolchain, raw feature and OS
state masks, p50/p95, dispersion and confidence intervals. Include direct
and actual caller results, allocations/peak memory, copied bytes, FFI call
volume, and clean startup/relink evidence. Run cold/warm and streaming cases
with realistic and adversarial sizes. Keep each cell's result and artifact
SHA-256, command and raw evidence reference in the #267 append-only ledger.
A NO-GO cell stays with its approved provider in its context. The existing
UTF-16 phase harness is direct and actual-caller scan evidence, with
`TryRunFromEnvironment` in the disabled benchmark test
(`src/test/cpp/tests1/Utf16Phase8Benchmark.cpp:824–855`,
`src/test/cpp/tests1/test-cpudispatch.cpp:1962–1965`). Extend the actual-caller
paths for this migration, including when the calling path moves inside Rust.
Existing correctness anchors are `CpuDispatchTest.Utf16ScannersMatchReferenceAtLengthsAndPositions`,
`Utf16ScannersTreatNonAsciiAndSurrogatesAsOrdinaryCodeUnits`,
`Utf16ScannersDoNotReadPastGuardPage`, and
`RustByteCandidateMatchesCppAtEveryBoundaryAndAlignment`
(`src/test/cpp/tests1/test-cpudispatch.cpp:359,436,458,1326`).
[Performance contract](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/267)

| Criterion | Verify: | Expect: |
| --- | --- | --- |
| Load/save and conversion | `pwsh -NoProfile -File tools/verify-document-migration.ps1 -RunManifest $env:DOC_MIGRATION_RUN -Check LoadSavePerformance` | Identical encoded bytes, decoded units, EOL/offset map and result digests including NUL, short/misaligned tails, mixed newlines, Japanese, MIME, BOM, malformed input and giant single lines. Measure native open/decode to committed editor and save through close/finalization, including partitioning and adapter copies. Numeric gates pass independently for adopted cells. |
| Literal search and scan | `pwsh -NoProfile -File tools/verify-document-migration.ps1 -RunManifest $env:DOC_MIGRATION_RUN -Check ScanPerformance` | Match indices and offsets agree across C++ scalar/reference, direct kernels and real find/scan commands. Fixtures include legal alignments, boundary and guard pages, first/last/multiple/absent matches, dense/sparse, NUL and surrogates. Hot-path thresholds pass in each selected cell, with no unsupported ISA activation. |
| Full and incremental layout | `pwsh -NoProfile -File tools/verify-document-migration.ps1 -RunManifest $env:DOC_MIGRATION_RUN -Check LayoutPerformance` | Identical row and mapping digests for full open, local edit/undo and viewport rewrap with varied font metrics, widths, lines, wrap modes and kinsoku. Actual callers include native row consumption and cross-boundary work. Only volume/range batches cross the boundary. Hot-path thresholds pass and document revision stays stable for rewrap. |

### Behavior-equivalence tier

Apply a frozen contract/golden corpus, differential replay while a C++
provider exists, existing tests and existing startup/benchmark baseline
limits. New per-cell performance studies apply only to the hot-path rows
above. Validate inherited baselines using the existing startup tool and its
readiness semantics (`tools/measure-startup-performance.ps1:15–22,6416–6424`,
`tools/startup-performance.md:8–29`). For changes affecting visible layout,
include isolated CopyFromScreen/PrintWindow paired capture and noise-floor
checks per `CLAUDE.md:138–146`. Preserve baseline data and process receipts.
For example, the planned runner can invoke the existing commands below
with its bounded process wrapper after both comparison builds are available.
Use the same startup arguments and corpus with a separate receipt directory
for the reference build.

```powershell
& .\x64\Release\tests1.exe '--gtest_filter=CDocLine.*:CDocLineMgr.*:CDocLineMgrTextEditLog.*:CSearchAgent.*:LoadFinalization.*:CFileLoadScannerBounds.*:FileLoadOptionsTest.*'
pwsh -NoProfile -File tools/measure-startup-performance.ps1 -SakuraExe .\x64\Release\sakura.exe -Iterations 7 -OutputDirectory "$env:USERPROFILE/tmp/document-migration/candidate-startup"
```

| Criterion | Verify: | Expect: |
| --- | --- | --- |
| Raw model, revision, metadata and range contracts | `pwsh -NoProfile -File tools/verify-document-migration.ps1 -RunManifest $env:DOC_MIGRATION_RUN -Check ModelEquivalence` | Existing line/buffer/codec/checker tests and frozen deterministic/property replay pass. Identical line content, metadata, positions, revisions, invalidation and boundaries for every step. Overflow, log gap, load assembly, cancelled or superseded stage produce the recorded terminal and reader result. |
| Undo and native caret/selection | `pwsh -NoProfile -File tools/verify-document-migration.ps1 -RunManifest $env:DOC_MIGRATION_RUN -Check EditHistoryEquivalence` | Ordered command fixture compares text, block count, undo/redo availability, clean index, line change sequences, directed selection, caret coordinates and pane state after each operation. Reference and Rust traces agree including cancellation and redo-tail truncation. |
| Lifecycle, file failure and persistence | `pwsh -NoProfile -File tools/verify-document-migration.ps1 -RunManifest $env:DOC_MIGRATION_RUN -Check SessionEquivalence` | Load finalization and Working Copy suites plus frozen native failure/recovery traces pass. Open/save/close notifications are exactly once at the recorded boundaries. No stale completion publishes and every failed/cancelled path matches the recorded bytes, dirty state and recovery artifacts. Unsupported native capabilities return their typed terminal. |
| ABI, lifecycle safety and authority | `pwsh -NoProfile -File tools/verify-document-migration.ps1 -RunManifest $env:DOC_MIGRATION_RUN -Check AbiAndAuthority` | Negative tests reject invalid version/size, handle/thread, reentry, overflow, capacity and stale tokens. Panic/Stop/poison and cancellation fixtures reach the documented terminals. Schema-v4 and link/symbol checks prove one provider per domain/context, no shadow effects, one archive, no crossed unwind, and valid CPU policy. |
| Existing suites, startup baselines and release lifecycle | `pwsh -NoProfile -File tools/verify-document-migration.ps1 -RunManifest $env:DOC_MIGRATION_RUN -Check RegressionAndRetirement` | Locked Rust tests, C++ Debug/Release suites, MinGW compatibility corpus, static analysis and supported package/launch smoke are green. Startup and benchmark baselines pass. Visible-layout evidence stays within approved capture noise. A completed rollback rehearsal, bake receipt and zero remaining production references prove that a retired provider can be removed. |

## 8. Owner decisions before implementation or adoption

1. **MinGW and remaining provider contexts.** Decide whether to keep
   `x86_64-pc-windows-gnu` with separately qualified Rust production or retire
   MinGW through an explicit product decision. Until resolved, its current
   provider contract applies. Confirm G0 and the P0 dispositions in #267.
2. **Save failure, undo and recovery contract.** Decide what becomes a
   separately reviewed specification change when current behavior and intent
   differ. `CSaveAgent::OnBeforeSave` can edit EOL before the file write, and
   `OnSave` does not inspect `CWriteManager`'s returned conversion result
   before setting metadata. The exact outcomes of late failures, partial
   disk writes and cancellation are **unverified**. Freeze these outcomes
   and the current redo/save-point behavior before choosing any correction.
   (`sakura_core/agent/CSaveAgent.cpp:81–105`,
   `sakura_core/env/CWriteManager.cpp:111–155`,
   `sakura_core/cmd/COpeBuf.cpp:61–131`)
3. **Recovery and encoding scope.** Confirm whether the strict UTF-8
   Working Copy format should eventually support the full raw-unit back-up
   domain. A format/version change needs its own spec, upgrade/rollback and
   corpus. Confirm the supported Windows/code-page/locale and MIME matrix
   including native clients of charset helpers. (`sakura_core/workbench/editor/persistence/CEditDocWorkingCopyPersistenceAdapter.cpp:116–154,169–192,448–458`,
   `sakura_core/charset/CCodePage.cpp:76–195,280–339`,
   `sakura_core/charset/CJis.h:15–38`)
4. **Layout compatibility and capacity.** Approve the native font/metrics
   and syntax/color corpus, coordinate and caret expectations at ambiguous
   units, the legacy plugin/message contracts, and the memory/maximum-input
   limits for pinned snapshots and long lines. If a reference mismatch with
   real VS Code or a platform constraint is found, record the observed
   discrepancy and get a specification decision before behavioral change.
   (`CLAUDE.md:3–45`, `sakura_core/doc/layout/CLayoutMgr.cpp:798–1040`,
   `sakura_core/window/CEditWnd.cpp:14496–14510`)
5. **Adoption evidence and bake.** Approve AMD/Intel host matrix,
   per-cell weights, no-reversal threshold, memory/copy budgets, exact
   release/canary exposure, bake duration and rollback triggers before
   collecting data. Confirm when each aliasing line/selection consumer has
   moved and where any reviewed C++ retention exception expires.
   [Adoption and evidence contract](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/267)

### Review scope and residual risk

- **Scope used.** Native document composition, logical lines, buffers,
  undo, layout, load/save and charset entry points, Workbench/Working Copy
  adapters, targeted C++ tests and the native workspace/build-provider
  contracts documented above.
- **Scope expansion.** The text revision log required inspection of TextMate's
  paint consumer, the EOL save path required inspection of `CDocVisitor`,
  and shared recovery required inspection of the existing C++
  `DocumentSession` projection. These consumers define observable ordering
  and coordinate contracts at cutover.
- **Architecture impact.** Design only. Production authority and state
  ownership are unchanged. The proposed session would own the text,
  history and derived layout and give C++ typed read and effect batches.
- **Future reading volume.** With the proposed boundaries implemented, a
  codec change can be reviewed against codec corpus and stream contract
  without reading native view internals. A text/history change will use
  session transactions and replay traces without reading charset or Win32
  metrics internals. Future issues must still understand snapshot
  generations, raw-unit coordinate semantics and the appropriate gate.
  No reduction has been measured. Residual risk is highest where
  save failure, grouped native caret/selection, long-line performance
  and renderer-dependent wrap evidence are still unverified.
