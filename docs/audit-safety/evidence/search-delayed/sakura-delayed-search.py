from pathlib import Path
import subprocess, hashlib, json, time, difflib
r=Path('C:/Users/developer/tmp/sakura-audit-safety')
out=r/'.codex/goal-loop/audit-safety'
prod=r/'sakura_core/workbench/search/CSearchWorkbenchTool.cpp'
test=r/'src/test/cpp/tests1/workbench/SearchWorkbenchToolTest.cpp'
original={p:p.read_bytes() for p in (prod,test)}
sha=lambda b:hashlib.sha256(b).hexdigest()
receipt={'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=r,text=True).strip(),'original':{str(p.relative_to(r)):sha(b) for p,b in original.items()},'runs':[]}
def run(cmd,label,timeout=300):
    with (out/(label+'-driver.log')).open('w',encoding='utf-8') as log:
        start=time.time(); p=subprocess.Popen(cmd,cwd=r,stdout=log,stderr=subprocess.STDOUT)
        try: code=p.wait(timeout)
        except subprocess.TimeoutExpired:
            subprocess.run(['taskkill','/PID',str(p.pid),'/T','/F'],capture_output=True);p.wait();raise
    receipt['runs'].append({'label':label,'command':cmd,'exit':code,'pid':p.pid,'seconds':time.time()-start,'hashes':{str(f.relative_to(r)):sha(f.read_bytes()) for f in original}})
    (out/'delayed-search.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
    print(label,code,flush=True);return code
def build(label):
    assert run(['py','-3','tools/build/sakura_build.py','build','solution','x64','Debug','--jobs','4'],label)==0
def tests(label,filter='SearchRequestSafetyTest.DelayedWorkerPublicationAfterInvalidation'):
    return run(['pwsh','-NoProfile','-File','C:/Users/developer/tmp/sakura-audit-run-tests.ps1','-Label',label,'-Filter',filter],label,150)
decl='''
namespace audit_delayed_search {
HANDLE entered{}, resume{}, finished{};
std::atomic<bool> armed{false};
std::atomic<unsigned long long> generation{0};
std::atomic<int> dropped{0}, published{0}, accepted{0}, timeouts{0}, matches{0};
}
'''
p=original[prod].decode('utf-8-sig').replace('\r\n','\n')
anchor='namespace workbench::search {'
assert p.count(anchor)==1
p=p.replace(anchor,decl+'\n'+anchor)
anchor='\t\tacceptedRequest = result->request;'
assert p.count(anchor)==1
p=p.replace(anchor,'\t\tif (result->request.generation == audit_delayed_search::generation.load()) ++audit_delayed_search::accepted;\n'+anchor)
anchor='\t\t\t\tstd::lock_guard<std::mutex> guard(state->windowMutex);'
assert p.count(anchor)==1
barrier='''
                const bool observed = audit_delayed_search::armed.exchange(false);
                if (observed) {
                    audit_delayed_search::generation.store(request.generation);
                    audit_delayed_search::matches.store(static_cast<int>(result->results.matchCount));
                    ::SetEvent(audit_delayed_search::entered);
                    if (::WaitForSingleObject(audit_delayed_search::resume, 10000) != WAIT_OBJECT_0)
                        ++audit_delayed_search::timeouts;
                }
'''
p=p.replace(anchor,barrier+anchor)
anchor='\t\t\t\t\t|| state->generation.load(std::memory_order_acquire) != request.generation) {\n\t\t\t\t\tcontinue;'
assert p.count(anchor)==1
p=p.replace(anchor,anchor.replace('\t\t\t\t\tcontinue;', '''                    if (observed) {
                        ++audit_delayed_search::dropped;
                        ::SetEvent(audit_delayed_search::finished);
                    }
\t\t\t\t\tcontinue;'''))
anchor='\t\t\t\t\tstate->results.CancelWakeAndDiscard();\n\t\t\t\t}'
assert p.count(anchor)==1
p=p.replace(anchor,anchor+'''
                if (observed) {
                    ++audit_delayed_search::published;
                    ::SetEvent(audit_delayed_search::finished);
                }
''')
t=original[test].decode('utf-8-sig')+'''

namespace audit_delayed_search {
extern HANDLE entered, resume, finished;
extern std::atomic<bool> armed;
extern std::atomic<unsigned long long> generation;
extern std::atomic<int> dropped, published, accepted, timeouts, matches;
struct EventScope {
    EventScope() {
        entered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        resume = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        finished = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        generation = ~0ull;
        dropped = published = accepted = timeouts = matches = 0;
    }
    ~EventScope() {
        armed = false;
        ::SetEvent(resume);
        auto& retirement = workbench::WorkerRetirementService::Instance();
        const auto deadline = ::GetTickCount64() + 12000;
        while (retirement.ReservedOrPendingCount() && ::GetTickCount64() < deadline) ::Sleep(5);
        EXPECT_EQ(0u, retirement.ReservedOrPendingCount());
        if (!retirement.ReservedOrPendingCount()) {
            ::CloseHandle(entered); ::CloseHandle(resume); ::CloseHandle(finished);
            entered = resume = finished = nullptr;
        }
    }
};
}

TEST_F(SearchRequestSafetyTest, DelayedWorkerPublicationAfterInvalidation)
{
    using workbench::search::CSearchWorkbenchTool;
    namespace probe = audit_delayed_search;
    for (int scenario = 0; scenario < 7; ++scenario) {
        SCOPED_TRACE(scenario);
        probe::EventScope events;
        ASSERT_NE(nullptr, probe::entered);
        ASSERT_NE(nullptr, probe::resume);
        ASSERT_NE(nullptr, probe::finished);
        const cxx::ResourceHolder<&::DestroyWindow> parent{::CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
            0, 0, 400, 300, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr)};
        ASSERT_NE(nullptr, parent.get());
        CSearchWorkbenchTool tool;
        ASSERT_TRUE(tool.Create(parent.get()));
        tool.SetRoot(root.wstring());
        probe::armed = true;
        tool.SetQueryText(L"needle");
        const auto window = tool.GetHwnd();
        const auto list = ::GetDlgItem(window, 3);
        ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(probe::entered, 5000));
        ASSERT_EQ(1, probe::matches.load()); // Real search completed before invalidation.
        if (scenario == 0) tool.SetQueryText(L"");
        if (scenario == 1) ::SetWindowTextW(::GetDlgItem(window, 1), L"different");
        if (scenario == 2) {
            ::SetWindowTextW(::GetDlgItem(window, 1), L"different");
            ::SetWindowTextW(::GetDlgItem(window, 1), L"needle");
        }
        if (scenario == 3) tool.SetRoot(L"");
        if (scenario == 4) tool.Close();
        if (scenario == 5) {
            RECT client{};
            ASSERT_TRUE(::GetClientRect(window, &client));
            const auto geometry = workbench::search::CalculateSearchWidgetGeometry(client, 96, false, 0);
            ::SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(geometry.queryBox.right - 32,
                (geometry.queryBox.top + geometry.queryBox.bottom) / 2));
        }
        // Scenario 6 is the unchanged-query control: the delayed result must be accepted.
        ::SetEvent(probe::resume);
        ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(probe::finished, 5000));
        MSG message{};
        constexpr UINT complete = WM_APP + 0x5e1;
        while (::PeekMessageW(&message, window, complete, complete, PM_REMOVE)) ::DispatchMessageW(&message);
        EXPECT_EQ(0, probe::timeouts.load());
        EXPECT_EQ(scenario == 6 ? 0 : 1, probe::dropped.load());
        EXPECT_EQ(scenario == 6 ? 1 : 0, probe::published.load());
        EXPECT_EQ(scenario == 6 ? 1 : 0, probe::accepted.load());
        if (scenario == 4) {
            EXPECT_FALSE(::IsWindow(window));
            EXPECT_FALSE(::IsWindow(list));
        }
        if (scenario == 6) EXPECT_EQ(2, ::SendMessageW(list, LB_GETCOUNT, 0, 0));
        tool.Close();
    }
}
'''
t=t.replace('#include <cstdlib>','#include <cstdlib>\n#include <atomic>')
variants={prod:p.encode('utf-8-sig'),test:t.encode('utf-8-sig')}
patch=''
for f,b in variants.items():
    patch+=''.join(difflib.unified_diff(original[f].decode('utf-8-sig').replace('\r\n','\n').splitlines(True),b.decode('utf-8-sig').replace('\r\n','\n').splitlines(True),fromfile=str(f.relative_to(r)),tofile=str(f.relative_to(r)),n=0))
(out/'delayed-search-injection.patch').write_text(patch,encoding='utf-8')
try:
    for f,b in variants.items(): f.write_bytes(b)
    build('delayed-search-build')
    assert tests('delayed-search-green')==0
    before=b'|| state->generation.load(std::memory_order_acquire) != request.generation) {'
    assert variants[prod].count(before)==1
    prod.write_bytes(variants[prod].replace(before,b'|| false) { // Deliberate publication-guard mutant.'))
    build('delayed-search-mutant-build')
    assert tests('delayed-search-mutant')==1
    prod.write_bytes(variants[prod])
    build('delayed-search-probe-restored-build')
    assert tests('delayed-search-probe-restored')==0
finally:
    for f,b in original.items(): f.write_bytes(b)
    receipt['restored']={str(f.relative_to(r)):sha(f.read_bytes()) for f in original}
    (out/'delayed-search.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
    build('delayed-search-normal-build')
    assert tests('delayed-search-normal','SearchRequestSafetyTest.*:FileLoadOptionsTest.*:SearchWorkbenchToolGeometry.*:ExplorerTool.ProductionWorkerDisplaysJunctionsAsLeaves')==0
