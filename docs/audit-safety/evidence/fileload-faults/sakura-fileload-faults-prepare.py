from pathlib import Path
import json,hashlib
r=Path('C:/Users/developer/tmp/sakura-audit-safety');out=Path('C:/Users/developer/tmp/sakura-fileload-faults');out.mkdir(exist_ok=True)
s=(r/'sakura_core/io/CFileLoad.cpp').read_text(encoding='utf-8-sig')
s=s.replace('#include "io/CFileLoad.h"','#include "io/CFileLoad.h"\n#include <new>\nextern int gFileLoadFaultStage;\nextern const void* gFileLoadFaultView;')
s=s.replace('if (!::GetFileSizeEx(m_file.get(), &size)', 'if (gFileLoadFaultStage == 1 || !::GetFileSizeEx(m_file.get(), &size)')
s=s.replace('m_mapping.reset(::CreateFileMappingW(m_file.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));','m_mapping.reset(gFileLoadFaultStage == 2 ? nullptr : ::CreateFileMappingW(m_file.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));')
s=s.replace('m_view.reset(static_cast<const char*>(::MapViewOfFile(m_mapping.get(), FILE_MAP_READ, 0, 0, 0)));','m_view.reset(gFileLoadFaultStage == 3 ? nullptr : static_cast<const char*>(::MapViewOfFile(m_mapping.get(), FILE_MAP_READ, 0, 0, 0)));')
s=s.replace('m_pReadBufTop = m_mapping->Data();','m_pReadBufTop = m_mapping->Data();\n    if (gFileLoadFaultStage == 4) { gFileLoadFaultView = m_pReadBufTop; throw std::bad_alloc(); }')
s=s.replace('m_pCodeBase.reset(CCodeFactory::CreateCodeBase(other.m_CharCode, other.m_nFlag));','if (gFileLoadFaultStage == 5) throw std::bad_alloc();\n\tm_pCodeBase.reset(CCodeFactory::CreateCodeBase(other.m_CharCode, other.m_nFlag));')
(out/'CFileLoad-injected.cpp').write_text(s,encoding='utf-8-sig')
t=(r/'src/test/cpp/tests1/test-file.cpp').read_text(encoding='utf-8-sig')
t+='''
int gFileLoadFaultStage = 0;
const void* gFileLoadFaultView = nullptr;

TEST_F(FileLoadOptionsTest, InjectedOpenFailuresReleaseResourcesBeforeReuse)
{
    Write("payload\\r\\n");
    CFileLoad reader;
    for (int stage : {1, 2, 3, 4}) {
        SCOPED_TRACE(stage);
        DWORD before = 0, after = 0;
        ASSERT_TRUE(::GetProcessHandleCount(::GetCurrentProcess(), &before));
        gFileLoadFaultStage = stage;
        gFileLoadFaultView = nullptr;
        if (stage == 4) {
            EXPECT_THROW(reader.FileOpen(path.c_str(), false, CODE_UTF8, 0), std::bad_alloc);
        } else {
            EXPECT_THROW(reader.FileOpen(path.c_str(), false, CODE_UTF8, 0), CError_FileOpen);
        }
        gFileLoadFaultStage = 0;
        ASSERT_TRUE(::GetProcessHandleCount(::GetCurrentProcess(), &after));
        EXPECT_EQ(before, after);
        FILETIME stamp{};
        EXPECT_FALSE(reader.GetFileTime(nullptr, nullptr, &stamp));
        if (stage == 4) {
            MEMORY_BASIC_INFORMATION region{};
            ASSERT_NE(nullptr, gFileLoadFaultView);
            ASSERT_EQ(sizeof(region), ::VirtualQuery(gFileLoadFaultView, &region, sizeof(region)));
            EXPECT_EQ(static_cast<DWORD>(MEM_FREE), region.State);
        }
        ASSERT_EQ(CODE_UTF8, reader.FileOpen(path.c_str(), false, CODE_UTF8, 0));
        CNativeW line;
        CEol eol;
        EXPECT_EQ(RESULT_COMPLETE, reader.ReadLine(&line, &eol));
        EXPECT_EQ(L"payload\\r\\n", std::wstring(line.GetStringPtr(), line.GetStringLength()));
        reader.FileClose();
    }
}

TEST_F(FileLoadOptionsTest, InjectedPrepareAllocationFailureReleasesOnlyDestination)
{
    Write("payload\\r\\n");
    CFileLoad parent, reader;
    ASSERT_EQ(CODE_UTF8, parent.FileOpen(path.c_str(), false, CODE_UTF8, 0));
    DWORD before = 0, after = 0;
    ASSERT_TRUE(::GetProcessHandleCount(::GetCurrentProcess(), &before));
    gFileLoadFaultStage = 5;
    EXPECT_THROW(reader.Prepare(parent, 0, static_cast<size_t>(parent.GetFileSize())), std::bad_alloc);
    gFileLoadFaultStage = 0;
    ASSERT_TRUE(::GetProcessHandleCount(::GetCurrentProcess(), &after));
    EXPECT_EQ(before, after);
    FILETIME stamp{};
    EXPECT_FALSE(reader.GetFileTime(nullptr, nullptr, &stamp));
    EXPECT_TRUE(parent.GetFileTime(nullptr, nullptr, &stamp));
    reader.Prepare(parent, 0, static_cast<size_t>(parent.GetFileSize()));
    parent.FileClose();
    CNativeW line;
    CEol eol;
    EXPECT_EQ(RESULT_COMPLETE, reader.ReadLine(&line, &eol));
    EXPECT_EQ(L"payload\\r\\n", std::wstring(line.GetStringPtr(), line.GetStringLength()));
}
'''
(out/'test-file-injected.cpp').write_text(t,encoding='utf-8-sig')
b=Path('C:/Users/developer/tmp/sakura-fileload-asan-build.py').read_text(encoding='utf-8')
b=b.replace("sakura-fileload-asan'","sakura-fileload-faults'")
b=b.replace("str(r/rel)]", "str(out/'CFileLoad-injected.cpp' if rel=='sakura_core/io/CFileLoad.cpp' else out/'test-file-injected.cpp' if rel=='src/test/cpp/tests1/test-file.cpp' else r/rel)]")
Path('C:/Users/developer/tmp/sakura-fileload-faults-build.py').write_text(b,encoding='utf-8')
(out/'injection-sources.json').write_text(json.dumps({p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [out/'CFileLoad-injected.cpp',out/'test-file-injected.cpp']},indent=2),encoding='utf-8')