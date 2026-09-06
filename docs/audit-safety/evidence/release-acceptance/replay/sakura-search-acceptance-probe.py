from pathlib import Path
import subprocess,hashlib,json,time,difflib
r=Path('C:/Users/developer/tmp/sakura-audit-safety')
out=r/'.codex/goal-loop/audit-safety'
prod=r/'sakura_core/workbench/search/CSearchWorkbenchTool.cpp'
test=r/'src/test/cpp/tests1/workbench/SearchWorkbenchToolTest.cpp'
original={p:p.read_bytes() for p in (prod,test)}
sha=lambda b:hashlib.sha256(b).hexdigest()
receipt={'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=r,text=True).strip(),
         'kind':'diagnostic identity injection at actual native acceptance and replacement boundaries; synthetic mismatches, not ordinary UI reachability',
         'original':{str(p.relative_to(r)):sha(b) for p,b in original.items()},'runs':[]}
def run(cmd,label,expected=0):
    start=time.monotonic()
    with (out/(label+'-driver.log')).open('w',encoding='utf-8') as log:
        p=subprocess.Popen(cmd,cwd=r,stdout=log,stderr=subprocess.STDOUT)
        try: code=p.wait(600)
        except subprocess.TimeoutExpired:
            subprocess.run(['taskkill','/PID',str(p.pid),'/T','/F'],capture_output=True);p.wait();raise
    receipt['runs'].append({'label':label,'command':cmd,'exit':code,'seconds':time.monotonic()-start,
                           'hashes':{str(f.relative_to(r)):sha(f.read_bytes()) for f in original}})
    (out/'search-acceptance-probe.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
    print(label,code,flush=True)
    assert code==expected,(label,code,expected)
def build(label):
    run(['py','-3','tools/build/sakura_build.py','build','solution','x64','Debug','--jobs','4'],label)
def tests(label,expected=0,filter='SearchRequestSafetyTest.InjectedAcceptanceIdentityContract:SearchRequestSafetyTest.InjectedReplaceIdentityContract'):
    run(['pwsh','-NoProfile','-File','C:/Users/developer/tmp/sakura-audit-run-tests.ps1','-Label',label,
         '-Filter',filter,'-TimeoutSeconds','120'],label,expected)

p=original[prod].decode('utf-8-sig').replace('\r\n','\n')
anchor='namespace workbench::search {'
assert p.count(anchor)==1
p=p.replace(anchor,'namespace audit_acceptance { int mode{}, phase{}, reached{}, committed{}; }\n'+anchor)
anchor='\t\tacceptedRequest = result->request;'
assert p.count(anchor)==1
p=p.replace(anchor,'\t\tif (audit_acceptance::phase == 1) ++audit_acceptance::committed;\n'+anchor)
anchor='\t\timpl.AcceptResult(impl.TakeLatestResult());'
assert p.count(anchor)==1
p=p.replace(anchor,'''        auto result = impl.TakeLatestResult();
        const bool wasClosed = impl.closed;
        if (audit_acceptance::phase == 1 && result) {
            ++audit_acceptance::reached;
            auto request = result->request;
            switch (audit_acceptance::mode) {
            case 1: ++request.generation; break;
            case 2: request.root += L"-stale"; break;
            case 3: request.query.text += L"-stale"; break;
            case 4: request.query.matchCase = !request.query.matchCase; break;
            case 5: request.query.wholeWord = !request.query.wholeWord; break;
            case 6: request.query.useRegex = !request.query.useRegex; break;
            case 7: impl.closed = true; break;
            }
            auto changed = std::make_unique<WorkerResult>(std::move(request));
            changed->results = std::move(result->results);
            result = std::move(changed);
        }
        impl.AcceptResult(std::move(result));
        impl.closed = wasClosed;''')
anchor='\t\tif (slice.empty() || model.text.empty() || !acceptedRequest || closed'
assert p.count(anchor)==1
p=p.replace(anchor,'''        if (audit_acceptance::phase == 2) {
            ++audit_acceptance::reached;
            switch (audit_acceptance::mode) {
            case 1: ++acceptedRequest->generation; break;
            case 2: acceptedRequest->root += L"-stale"; break;
            case 3: acceptedRequest->query.text += L"-stale"; break;
            case 4: acceptedRequest->query.matchCase = !acceptedRequest->query.matchCase; break;
            case 5: acceptedRequest->query.wholeWord = !acceptedRequest->query.wholeWord; break;
            case 6: acceptedRequest->query.useRegex = !acceptedRequest->query.useRegex; break;
            case 7: closed = true; break;
            }
        }
        // Restore the diagnostic closed bit even when admission returns early.
        const auto restoreClosed = std::unique_ptr<Impl, void(*)(Impl*)>(this, [](Impl* self) {
            if (audit_acceptance::phase == 2 && audit_acceptance::mode == 7) self->closed = false;
        });
'''+anchor)
anchor='\t\tconst auto outcome = ReplaceMatches(slice, model, {});'
assert p.count(anchor)==1
p=p.replace(anchor,'\t\tif (audit_acceptance::phase == 2) ++audit_acceptance::committed;\n'+anchor)
t=original[test].decode('utf-8-sig').replace('\r\n','\n')+'\n'+r'''
namespace audit_acceptance {
extern int mode, phase, reached, committed;
class Scope final {
public:
    Scope(int selected, int stage) { mode=selected; phase=stage; reached=committed=0; }
    ~Scope() { phase=mode=0; }
};
}

// The diagnostic build injects one mismatch after a real worker result arrives.
TEST_F(SearchRequestSafetyTest, InjectedAcceptanceIdentityContract)
{
    for (int scenario=0; scenario<8; ++scenario) {
        SCOPED_TRACE(scenario);
        audit_acceptance::Scope scope(scenario,1);
        const cxx::ResourceHolder<&::DestroyWindow> parent{::CreateWindowExW(0,L"STATIC",L"",WS_POPUP,
            0,0,400,300,nullptr,nullptr,::GetModuleHandleW(nullptr),nullptr)};
        ASSERT_NE(nullptr,parent.get());
        workbench::search::CSearchWorkbenchTool tool;
        ASSERT_TRUE(tool.Create(parent.get()));
        tool.Layout(RECT{0,0,400,300},96);
        tool.SetRoot(root.wstring());
        tool.SetQueryText(L"needle");
        MSG message{};
        const auto deadline=::GetTickCount64()+5000;
        while (!audit_acceptance::reached && ::GetTickCount64()<deadline) {
            if (::PeekMessageW(&message,tool.GetHwnd(),WM_APP+0x5e1,WM_APP+0x5e1,PM_REMOVE))
                ::DispatchMessageW(&message);
            else ::Sleep(1);
        }
        EXPECT_EQ(1,audit_acceptance::reached);
        EXPECT_EQ(scenario==0?1:0,audit_acceptance::committed);
        EXPECT_EQ(scenario==0?2:0,::SendMessageW(::GetDlgItem(tool.GetHwnd(),3),LB_GETCOUNT,0,0));
        tool.Close();
        auto& retirement=workbench::WorkerRetirementService::Instance();
        const auto retired=::GetTickCount64()+5000;
        while(retirement.ReservedOrPendingCount() && ::GetTickCount64()<retired) ::Sleep(1);
        EXPECT_EQ(0u,retirement.ReservedOrPendingCount());
    }
}

// Injection occurs after ReadBoxes and before the destructive admission guard.
TEST_F(SearchRequestSafetyTest, InjectedReplaceIdentityContract)
{
    for (int scenario=0; scenario<8; ++scenario) {
        SCOPED_TRACE(scenario);
        audit_acceptance::Scope scope(scenario,2);
        Write("needle\r\n");
        int changed=0;
        const cxx::ResourceHolder<&::DestroyWindow> parent{::CreateWindowExW(0,L"STATIC",L"",WS_POPUP,
            0,0,400,300,nullptr,nullptr,::GetModuleHandleW(nullptr),nullptr)};
        ASSERT_NE(nullptr,parent.get());
        workbench::search::CSearchWorkbenchTool tool;
        tool.SetFilesChangedCallback([&](const auto&) { ++changed; });
        ASSERT_TRUE(tool.Create(parent.get()));
        tool.Layout(RECT{0,0,400,300},96);
        tool.FocusReplace();
        tool.SetRoot(root.wstring());
        tool.SetQueryText(L"needle");
        MSG message{};
        bool posted=false;
        const auto deadline=::GetTickCount64()+5000;
        while (!posted && ::GetTickCount64()<deadline) {
            if (::PeekMessageW(&message,tool.GetHwnd(),WM_APP+0x5e1,WM_APP+0x5e1,PM_REMOVE)) {
                ::DispatchMessageW(&message); posted=true;
            } else ::Sleep(1);
        }
        ASSERT_TRUE(posted);
        ASSERT_EQ(2,::SendMessageW(::GetDlgItem(tool.GetHwnd(),3),LB_GETCOUNT,0,0));
        ASSERT_TRUE(::SetWindowTextW(::GetDlgItem(tool.GetHwnd(),2),L"changed"));
        RECT client{};
        ASSERT_TRUE(::GetClientRect(tool.GetHwnd(),&client));
        const auto geometry=workbench::search::CalculateSearchWidgetGeometry(client,96,true,0);
        const auto& button=geometry.replaceAll;
        ::SendMessageW(tool.GetHwnd(),WM_LBUTTONUP,0,
            MAKELPARAM((button.left+button.right)/2,(button.top+button.bottom)/2));
        EXPECT_EQ(1,audit_acceptance::reached);
        EXPECT_EQ(scenario==0?1:0,audit_acceptance::committed);
        EXPECT_EQ(scenario==0?1:0,changed);
        std::ifstream file(root/L"input.txt",std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
        EXPECT_EQ(std::string("\xef\xbb\xbf")+(scenario==0?"changed\r\n":"needle\r\n"),bytes);
        file.close();
        tool.Close();
        auto& retirement=workbench::WorkerRetirementService::Instance();
        const auto retired=::GetTickCount64()+5000;
        while(retirement.ReservedOrPendingCount() && ::GetTickCount64()<retired) ::Sleep(1);
        EXPECT_EQ(0u,retirement.ReservedOrPendingCount());
    }
}
'''
probe={prod:p.encode('utf-8-sig'),test:t.encode('utf-8-sig')}
patch=''
for f,b in probe.items():
    patch+=''.join(difflib.unified_diff(original[f].decode('utf-8-sig').replace('\r\n','\n').splitlines(True),
        b.decode('utf-8-sig').splitlines(True),fromfile=str(f.relative_to(r)),tofile=str(f.relative_to(r)),n=0))
(out/'search-acceptance-probe.patch').write_text(patch,encoding='utf-8')
variants={
    'generation':[(b'result->request.generation != shared->generation.load(std::memory_order_acquire)',b'false'),
                  (b'acceptedRequest->generation != shared->generation.load(std::memory_order_acquire)',b'false')],
    'root':[(b'result->request.root != root',b'false'),(b'acceptedRequest->root != root',b'false')],
    'pattern':[(b'!SameSearchPattern(result->request.query, model)',b'false'),
               (b'!SameSearchPattern(acceptedRequest->query, model)',b'false')],
    'closed':[(b'!result || closed ||',b'!result || false ||'),(b'!acceptedRequest || closed',b'!acceptedRequest || false')],
}
try:
    for f,b in probe.items(): f.write_bytes(b)
    build('search-acceptance-probe-build');tests('search-acceptance-probe-green')
    for name,changes in variants.items():
        changed=probe[prod]
        for old,new in changes:
            assert changed.count(old)==1,(name,old)
            changed=changed.replace(old,new)
        prod.write_bytes(changed)
        build('search-acceptance-'+name+'-build');tests('search-acceptance-'+name,1)
    prod.write_bytes(probe[prod])
    build('search-acceptance-probe-restored-build');tests('search-acceptance-probe-restored')
finally:
    for f,b in original.items(): f.write_bytes(b)
    receipt['restored']={str(f.relative_to(r)):sha(f.read_bytes()) for f in original}
    build('search-acceptance-normal-build')
    tests('search-acceptance-normal',0,'SearchRequestSafetyTest.*:FileLoadOptionsTest.*:SearchWorkbenchToolGeometry.*:ExplorerTool.ProductionWorkerDisplaysJunctionsAsLeaves')
