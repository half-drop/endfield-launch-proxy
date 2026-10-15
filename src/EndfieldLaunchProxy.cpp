#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <softpub.h>
#include <wintrust.h>
#include <wincrypt.h>
#include <sddl.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>
#include <utility>

namespace fs=std::filesystem;
struct Handle {
 HANDLE value=nullptr;
 Handle()=default; explicit Handle(HANDLE h):value(h){}
 Handle(const Handle&)=delete; Handle& operator=(const Handle&)=delete;
 ~Handle(){reset();}
 void reset(HANDLE h=nullptr){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=h;}
 bool valid()const{return value&&value!=INVALID_HANDLE_VALUE;}
};
static fs::path gamePath=LR"(D:\Hypergryph Launcher\games\Arknights Endfield\Endfield.exe)";
static fs::path logPath;
static DWORD startupSeconds=180,postcheckSeconds=30;
static bool Equal(const std::wstring&a,const std::wstring&b){return _wcsicmp(a.c_str(),b.c_str())==0;}
static std::string Utf8(const std::wstring&text){if(text.empty())return{};int n=WideCharToMultiByte(CP_UTF8,0,text.data(),(int)text.size(),nullptr,0,nullptr,nullptr);std::string s(n,'\0');WideCharToMultiByte(CP_UTF8,0,text.data(),(int)text.size(),s.data(),n,nullptr,nullptr);return s;}
static void Log(const std::wstring&text){
 if(logPath.empty())return;
 std::error_code ec;fs::create_directories(logPath.parent_path(),ec);
 Handle file(CreateFileW(logPath.c_str(),FILE_APPEND_DATA|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));
 if(!file.valid())return;
 LARGE_INTEGER size{};if(GetFileSizeEx(file.value,&size)&&size.QuadPart>262144){file.reset();file.reset(CreateFileW(logPath.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));if(!file.valid())return;}
 SYSTEMTIME t{};GetLocalTime(&t);wchar_t stamp[64]{};swprintf_s(stamp,L"%04u-%02u-%02u %02u:%02u:%02u ",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
 auto bytes=Utf8(std::wstring(stamp)+text+L"\r\n");DWORD written=0;WriteFile(file.value,bytes.data(),(DWORD)bytes.size(),&written,nullptr);
}
static int Fail(int code,const std::wstring&why,DWORD error=0){Log(L"FAIL code="+std::to_wstring(code)+L" reason="+why+L" win32="+std::to_wstring(error));return code;}
static std::wstring ImagePath(HANDLE h){std::wstring path(32768,L'\0');DWORD n=(DWORD)path.size();if(!QueryFullProcessImageNameW(h,0,path.data(),&n))return{};path.resize(n);return fs::path(path).lexically_normal().wstring();}
static std::wstring ReadRegistry(HKEY root,const std::wstring&key,const wchar_t*name,REGSAM view=0){
 HKEY raw=nullptr;if(RegOpenKeyExW(root,key.c_str(),0,KEY_QUERY_VALUE|view,&raw)!=ERROR_SUCCESS)return{};
 DWORD type=0,bytes=0;LONG status=RegQueryValueExW(raw,name,nullptr,&type,nullptr,&bytes);
 std::vector<wchar_t> data(bytes/sizeof(wchar_t)+1,L'\0');
 if(status==ERROR_SUCCESS&&bytes<65536&&(type==REG_SZ||type==REG_EXPAND_SZ))status=RegQueryValueExW(raw,name,nullptr,&type,(BYTE*)data.data(),&bytes);else status=ERROR_INVALID_DATA;
 RegCloseKey(raw);if(status!=ERROR_SUCCESS)return{};
 if(type==REG_EXPAND_SZ){std::wstring expanded(32768,L'\0');DWORD n=ExpandEnvironmentStringsW(data.data(),expanded.data(),(DWORD)expanded.size());if(!n||n>expanded.size())return{};expanded.resize(n-1);return expanded;}return data.data();
}
static std::vector<std::wstring> RegistryChildren(HKEY root,const std::wstring&path,REGSAM view=0){
 HKEY raw=nullptr;std::vector<std::wstring> result;if(RegOpenKeyExW(root,path.c_str(),0,KEY_ENUMERATE_SUB_KEYS|view,&raw)!=ERROR_SUCCESS)return result;
 for(DWORD i=0;;++i){wchar_t name[256]{};DWORD n=256;LONG s=RegEnumKeyExW(raw,i,name,&n,nullptr,nullptr,nullptr,nullptr);if(s==ERROR_NO_MORE_ITEMS)break;if(s==ERROR_SUCCESS)result.emplace_back(name,n);}RegCloseKey(raw);return result;
}
static bool IsTrustedOfficial(const fs::path&path){
 WINTRUST_FILE_INFO file{};file.cbStruct=sizeof(file);file.pcwszFilePath=path.c_str();
 WINTRUST_DATA trust{};trust.cbStruct=sizeof(trust);trust.dwUIChoice=WTD_UI_NONE;trust.fdwRevocationChecks=WTD_REVOKE_NONE;trust.dwUnionChoice=WTD_CHOICE_FILE;trust.pFile=&file;trust.dwStateAction=WTD_STATEACTION_VERIFY;trust.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL|WTD_REVOCATION_CHECK_NONE;
 GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;LONG status=WinVerifyTrust(nullptr,&action,&trust);bool official=false;
 if(status==ERROR_SUCCESS){auto provider=WTHelperProvDataFromStateData(trust.hWVTStateData);auto signer=provider?WTHelperGetProvSignerFromChain(provider,0,FALSE,0):nullptr;if(signer&&signer->csCertChain){wchar_t publisher[512]{};CertGetNameStringW(signer->pasCertChain[0].pCert,CERT_NAME_ATTR_TYPE,0,(void*)szOID_ORGANIZATION_NAME,publisher,512);official=Equal(publisher,L"Shanghai Hypergryph Network Technology Co., Ltd.");}}
 trust.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&action,&trust);return official;
}
struct Candidate{fs::path path;std::vector<unsigned long> version;};
static fs::path FindLauncher(fs::path&root){
 std::vector<fs::path> roots;roots.push_back(gamePath.parent_path().parent_path().parent_path());
 for(auto&key:RegistryChildren(HKEY_CLASSES_ROOT,L"")){if(key.rfind(L"hypergryphlauncher",0)!=0)continue;auto command=ReadRegistry(HKEY_CLASSES_ROOT,key+L"\\shell\\open\\command",nullptr);int n=0;auto args=CommandLineToArgvW(command.c_str(),&n);if(args){if(n>0)roots.push_back(fs::path(args[0]).parent_path());LocalFree(args);}}
 for(HKEY hive:{HKEY_CURRENT_USER,HKEY_LOCAL_MACHINE})for(REGSAM view:{KEY_WOW64_64KEY,KEY_WOW64_32KEY}){std::wstring base=L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";for(auto&child:RegistryChildren(hive,base,view)){auto key=base+L"\\"+child;auto name=ReadRegistry(hive,key,L"DisplayName",view);if(Equal(name,L"鹰角启动器")||Equal(name,L"Hypergryph Launcher")){auto location=ReadRegistry(hive,key,L"InstallLocation",view);if(!location.empty())roots.emplace_back(location);}}}
 std::vector<Candidate> candidates;std::error_code ec;
 for(auto&r:roots){if(r.empty())continue;if(fs::is_regular_file(r/L"Games.exe",ec))candidates.push_back({r/L"Games.exe",{}});ec.clear();for(fs::directory_iterator it(r,ec),end;!ec&&it!=end;it.increment(ec)){auto name=it->path().filename().wstring();if(name.empty()||name.find_first_not_of(L"0123456789.")!=std::wstring::npos)continue;std::vector<unsigned long> version;size_t start=0;bool valid=true;try{while(start<name.size()){size_t consumed=0;version.push_back(std::stoul(name.substr(start),&consumed));start+=consumed;if(start<name.size()&&name[start++]!=L'.'){valid=false;break;}}}catch(...){valid=false;}auto path=it->path()/L"Games.exe";if(valid&&fs::is_regular_file(path,ec))candidates.push_back({path,version});ec.clear();}}
 std::stable_sort(candidates.begin(),candidates.end(),[](auto&a,auto&b){return a.version>b.version;});
 for(auto&c:candidates){if(IsTrustedOfficial(c.path)){root=c.version.empty()?c.path.parent_path():c.path.parent_path().parent_path();return fs::absolute(c.path).lexically_normal();}}return{};
}
static bool ExistingProcess(const std::wstring&filename,const fs::path&expected,bool matchRoot){
 DWORD session=0;ProcessIdToSessionId(GetCurrentProcessId(),&session);Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));if(!snapshot.valid())return true;PROCESSENTRY32W item{};item.dwSize=sizeof(item);if(!Process32FirstW(snapshot.value,&item))return true;
 do{if(!Equal(item.szExeFile,filename))continue;DWORD otherSession=0;if(!ProcessIdToSessionId(item.th32ProcessID,&otherSession)||otherSession!=session)continue;Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,item.th32ProcessID));if(!process.valid())return true;auto path=ImagePath(process.value);if(path.empty())return true;if(!matchRoot&&Equal(path,expected.wstring()))return true;if(matchRoot){auto normalized=fs::path(path).lexically_normal().wstring();auto prefix=expected.lexically_normal().wstring()+L"\\";if(normalized.size()>prefix.size()&&_wcsnicmp(normalized.c_str(),prefix.c_str(),prefix.size())==0)return true;}}while(Process32NextW(snapshot.value,&item));return false;
}
static std::wstring MutexName(){
 std::wstring identity=gamePath.wstring();Handle token;HANDLE raw=nullptr;if(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw)){token.reset(raw);DWORD n=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&n);std::vector<BYTE> data(n);if(n&&GetTokenInformation(token.value,TokenUser,data.data(),n,&n)){LPWSTR sid=nullptr;if(ConvertSidToStringSidW(((TOKEN_USER*)data.data())->User.Sid,&sid)){identity+=sid;LocalFree(sid);}}}
 unsigned long long hash=14695981039346656037ULL;for(auto c:identity){hash^=(unsigned short)towlower(c);hash*=1099511628211ULL;}wchar_t name[80]{};swprintf_s(name,L"Local\\EndfieldLaunchProxy_%016llX",hash);return name;
}
static bool HasRequiredToken(){
 HANDLE raw=nullptr;
 if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw))return false;
 Handle token(raw);TOKEN_ELEVATION elevation{};DWORD bytes=0;
 if(!GetTokenInformation(token.value,TokenElevation,&elevation,sizeof(elevation),&bytes))return false;
 // The installed official images requireAdministrator. Reject before executing their bootstrap code.
 if(!elevation.TokenIsElevated){SetLastError(ERROR_ELEVATION_REQUIRED);return false;}
 SetLastError(ERROR_SUCCESS);BOOL restricted=IsTokenRestricted(token.value);DWORD restrictionError=GetLastError();
 if(restricted){SetLastError(ERROR_ACCESS_DENIED);return false;}
 if(restrictionError!=ERROR_SUCCESS){SetLastError(restrictionError);return false;}
 return true;
}
struct Parent {
 Handle process,job;DWORD pid=0;
 bool Start(const fs::path&exe){
  job.reset(CreateJobObjectW(nullptr,nullptr));if(!job.valid())return false;JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;if(!SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))return false;
  STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION p{};std::wstring cmd=L"\""+exe.wstring()+L"\"";
  if(!CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED|CREATE_NO_WINDOW|CREATE_BREAKAWAY_FROM_JOB,nullptr,exe.parent_path().c_str(),&startup,&p))return false;
  process.reset(p.hProcess);Handle thread(p.hThread);pid=p.dwProcessId;
  if(!AssignProcessToJobObject(job.value,process.value))return false;
  Log(L"official_parent_created_suspended pid="+std::to_wstring(pid));return true;
 }
 bool IsGameIsolated(HANDLE game){BOOL member=TRUE;if(!IsProcessInJob(game,job.value,&member)||member)return false;Log(L"game_job_isolation=verified");return true;}
 bool Close(){if(!process.valid())return true;bool ok=true;if(WaitForSingleObject(process.value,0)==WAIT_TIMEOUT){ok=TerminateProcess(process.value,0)!=FALSE;if(ok)ok=WaitForSingleObject(process.value,5000)==WAIT_OBJECT_0;}Log(L"official_suspended_parent_cleanup pid="+std::to_wstring(pid)+L" success="+(ok?L"true":L"false"));process.reset();job.reset();return ok;}
 ~Parent(){Close();}
};
struct WindowSearch{DWORD pid;HWND result=nullptr;};
static BOOL CALLBACK FindGameWindow(HWND window,LPARAM data){auto&search=*(WindowSearch*)data;DWORD pid=0;GetWindowThreadProcessId(window,&pid);if(pid!=search.pid||!IsWindowVisible(window)||GetWindow(window,GW_OWNER))return TRUE;wchar_t cls[128]{};GetClassNameW(window,cls,128);if(wcscmp(cls,L"UnityWndClass")!=0)return TRUE;RECT r{};GetClientRect(window,&r);if(r.right-r.left<320||r.bottom-r.top<200)return TRUE;DWORD_PTR result=0;if(!SendMessageTimeoutW(window,WM_NULL,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,500,&result))return TRUE;search.result=window;return FALSE;}
static HWND ReadyWindow(DWORD pid){WindowSearch search{pid};EnumWindows(FindGameWindow,(LPARAM)&search);return search.result;}
static bool Alive(HANDLE game){return WaitForSingleObject(game,0)==WAIT_TIMEOUT;}
struct LaunchCheck {
 fs::path path;ULONGLONG offset=0;
 LaunchCheck(){PWSTR low=nullptr;if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppDataLow,KF_FLAG_DEFAULT,nullptr,&low))){path=fs::path(low)/L"Hypergryph"/L"Endfield"/L"sdklogs"/L"OneChannelLC.log";CoTaskMemFree(low);}Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));LARGE_INTEGER size{};if(file.valid()&&GetFileSizeEx(file.value,&size))offset=(ULONGLONG)size.QuadPart;}
 bool Succeeded(DWORD parentPid){if(path.empty())return false;Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));if(!file.valid())return false;LARGE_INTEGER size{};if(!GetFileSizeEx(file.value,&size))return false;ULONGLONG end=(ULONGLONG)size.QuadPart,start=end<offset?0:offset;if(end<=start)return false;if(end-start>262144)start=end-262144;LARGE_INTEGER position{};position.QuadPart=(LONGLONG)start;if(!SetFilePointerEx(file.value,position,nullptr,FILE_BEGIN))return false;std::string text((size_t)(end-start),'\0');DWORD read=0;if(!ReadFile(file.value,text.data(),(DWORD)text.size(),&read,nullptr))return false;text.resize(read);std::string marker="Parent process check success:";size_t at=0;while((at=text.find(marker,at))!=std::string::npos){at+=marker.size();while(at<text.size()&&(text[at]==' '||text[at]=='\t'))++at;std::string expected=std::to_string(parentPid);if(text.compare(at,expected.size(),expected)==0&&(at+expected.size()>=text.size()||text[at+expected.size()]<'0'||text[at+expected.size()]>'9'))return true;}return false;}
};
static bool Parse(){
 int count=0;LPWSTR*args=CommandLineToArgvW(GetCommandLineW(),&count);if(!args)return false;bool valid=true;
 for(int i=1;i<count;++i){std::wstring arg=args[i];if(arg.rfind(L"--log=",0)==0)logPath=arg.substr(6);else if(arg.rfind(L"--game=",0)==0)gamePath=arg.substr(7);else if(arg.rfind(L"--startup-timeout=",0)==0){try{auto value=arg.substr(18);size_t consumed=0;auto n=std::stoul(value,&consumed);if(consumed!=value.size()||n<5||n>600)valid=false;else startupSeconds=n;}catch(...){valid=false;}}else if(arg.rfind(L"--postcheck=",0)==0){try{auto value=arg.substr(12);size_t consumed=0;auto n=std::stoul(value,&consumed);if(consumed!=value.size()||n>120)valid=false;else postcheckSeconds=n;}catch(...){valid=false;}}else valid=false;}
 LocalFree(args);gamePath=fs::absolute(gamePath).lexically_normal();if(!Equal(gamePath.filename().wstring(),L"Endfield.exe"))valid=false;return valid;
}
static int Run(){
 PWSTR local=nullptr;if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_DEFAULT,nullptr,&local))){logPath=fs::path(local)/L"EndfieldLaunchProxy"/L"launch.log";CoTaskMemFree(local);}
 if(!Parse())return Fail(10,L"invalid_options");
 Handle mutex(CreateMutexW(nullptr,FALSE,MutexName().c_str()));DWORD mutexError=GetLastError();if(!mutex.valid())return Fail(11,L"mutex_unavailable",mutexError);if(mutexError==ERROR_ALREADY_EXISTS){Log(L"SKIP duplicate_proxy");return 2;}
 Log(L"BEGIN version=1.0.2 mode=suspended_official_parent");
 std::error_code ec;if(!fs::is_regular_file(gamePath,ec))return Fail(12,L"game_file_missing");
 if(ExistingProcess(L"Endfield.exe",gamePath,false)){Log(L"SKIP game_already_running");return 3;}
 if(!HasRequiredToken())return Fail(16,L"required_elevated_token_unavailable",GetLastError());
 fs::path root;auto launcher=FindLauncher(root);if(launcher.empty())return Fail(13,L"trusted_official_launcher_not_found");
 if(ExistingProcess(L"Games.exe",root,true)||ExistingProcess(L"Launcher.exe",root,true)||ExistingProcess(L"Updater.exe",root,true)){Log(L"SKIP existing_official_launcher_or_updater");return 4;}
 Handle imageLock(CreateFileW(launcher.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));if(!imageLock.valid())return Fail(14,L"official_launcher_busy",GetLastError());
 if(!IsTrustedOfficial(launcher))return Fail(15,L"official_signature_changed");
 Log(L"official_launcher="+launcher.wstring());
 Parent parent;if(!parent.Start(launcher))return Fail(16,L"cannot_create_suspended_official_parent",GetLastError());
 SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);if(!bytes)return Fail(17,L"attribute_size",GetLastError());std::vector<BYTE> storage(bytes);auto attrs=(LPPROC_THREAD_ATTRIBUTE_LIST)storage.data();if(!InitializeProcThreadAttributeList(attrs,1,0,&bytes))return Fail(17,L"attribute_init",GetLastError());
 struct AttrCleanup{LPPROC_THREAD_ATTRIBUTE_LIST p;~AttrCleanup(){DeleteProcThreadAttributeList(p);}}attributeCleanup{attrs};
 HANDLE parentHandle=parent.process.value;if(!UpdateProcThreadAttribute(attrs,0,PROC_THREAD_ATTRIBUTE_PARENT_PROCESS,&parentHandle,sizeof(parentHandle),nullptr,nullptr))return Fail(17,L"parent_attribute",GetLastError());
 STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.lpAttributeList=attrs;PROCESS_INFORMATION p{};std::wstring command=L"\""+gamePath.wstring()+L"\"";
 LaunchCheck launchCheck;
 if(!CreateProcessW(gamePath.c_str(),command.data(),nullptr,nullptr,FALSE,EXTENDED_STARTUPINFO_PRESENT|CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,gamePath.parent_path().c_str(),&startup.StartupInfo,&p))return Fail(18,L"game_creation_failed",GetLastError());
 Handle game(p.hProcess),thread(p.hThread);
 struct PendingGame{HANDLE process;bool running=false;~PendingGame(){if(!running){TerminateProcess(process,1);WaitForSingleObject(process,5000);}}}pending{game.value};
 if(!parent.IsGameIsolated(game.value))return Fail(19,L"game_job_isolation_failed",GetLastError());
 Log(L"game_created pid="+std::to_wstring(p.dwProcessId)+L" parent="+std::to_wstring(parent.pid));
 auto actual=ImagePath(game.value);if(!Equal(actual,gamePath.wstring()))return Fail(19,L"game_image_identity_unconfirmed");
 Log(L"game_image_verified="+actual);
 DWORD previous=ResumeThread(thread.value);if(previous==0){pending.running=true;return Fail(19,L"game_thread_already_running");}if(previous!=1)return Fail(19,L"game_thread_resume_failed",GetLastError());pending.running=true;thread.reset();
 ULONGLONG started=GetTickCount64(),stable=0;HWND last=nullptr;bool ready=false,checked=false;
 while(GetTickCount64()-started<(ULONGLONG)startupSeconds*1000){if(!Alive(game.value))return Fail(20,L"game_exited_before_ready");if(!checked&&launchCheck.Succeeded(parent.pid)){checked=true;Log(L"new_sdk_parent_check_success pid="+std::to_wstring(parent.pid));}HWND window=ReadyWindow(p.dwProcessId);if(window){if(window!=last){stable=GetTickCount64();last=window;}if(checked&&GetTickCount64()-stable>=10000){ready=true;break;}}else{stable=0;last=nullptr;}Sleep(200);}
 if(!ready)return Fail(21,L"game_window_timeout");
 Log(L"game_window_ready pid="+std::to_wstring(p.dwProcessId)+L" hwnd="+std::to_wstring((ULONG_PTR)last)+L" stable_seconds=10");
 if(!parent.Close())return Fail(22,L"owned_suspended_parent_cleanup_failed");imageLock.reset();
 ULONGLONG after=GetTickCount64();while(GetTickCount64()-after<(ULONGLONG)postcheckSeconds*1000){if(!Alive(game.value))return Fail(23,L"game_exited_after_parent_cleanup");if(!ReadyWindow(p.dwProcessId))return Fail(24,L"game_window_unresponsive_after_cleanup");Sleep(500);}
 Log(L"SUCCESS game_pid="+std::to_wstring(p.dwProcessId)+L" parent_removed=true postcheck_seconds="+std::to_wstring(postcheckSeconds));return 0;
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,LPWSTR,int){
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);SetProcessDPIAware();
 try{return Run();}catch(...){return Fail(99,L"unexpected_exception");}
}
