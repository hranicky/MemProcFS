// m_handle.c : implementation of the handle info built-in module.
//
// (c) Ulf Frisk, 2019-2021
// Author: Ulf Frisk, pcileech@frizk.net
//
#include "pluginmanager.h"
#include "util.h"
#include "vmm.h"
#include "vmmdll.h"
#include "vmmwin.h"
#include "vmmwinobj.h"

#define MHANDLE_LINELENGTH       222ULL
#define MHANDLE_LINEHEADER       L"   #    PID  Handle Object Address   Access Type             Description"

VOID MHandle_ReadLine_Callback(_Inout_opt_ PVOID ctx, _In_ DWORD cbLineLength, _In_ DWORD ie, _In_ PVMM_MAP_HANDLEENTRY pe, _Out_writes_(cbLineLength + 1) LPSTR szu8)
{
    PVMMWIN_OBJECT_TYPE pOT;
    CHAR szType[32] = { 0 };
    if((pOT = VmmWin_ObjectTypeGet((BYTE)pe->iType))) {
        snprintf(szType, _countof(szType), "%S", pOT->wsz);
        szType[16] = 0;
    } else {
        *(PDWORD)szType = pe->dwPoolTag;
        szType[4] = 0;
    }
    Util_snwprintf_u8ln(szu8, cbLineLength,
        L"%04x%7i%8x %16llx %6x %-16S %s",
        ie,
        pe->dwPID,
        pe->dwHandle,
        pe->vaObject,
        pe->dwGrantedAccess,
        szType,
        pe->wszText + pe->cwszText - min(128, pe->cwszText)
    );
}

/*
* qfind callback function for locating a handle entry in its map given an id.
*/
int MHandle_HandleFromPath_qfind(_In_ PVOID pvFind, _In_ PVOID pvEntry)
{
    QWORD qwKey = (QWORD)pvFind;
    PVMM_MAP_HANDLEENTRY peEntry = (PVMM_MAP_HANDLEENTRY)pvEntry;
    if(peEntry->dwHandle > qwKey) { return -1; }
    if(peEntry->dwHandle < qwKey) { return 1; }
    return 0;
}

/*
* Retrieve a handle entry from a given path. The handle entry is identified by
* the handle id which is 1st in path after 'by-id' directory.
* -- wszPath
* -- pHandleMap
* -- return
*/
_Success_(return != NULL)
PVMM_MAP_HANDLEENTRY MHandle_HandleFromPath(_In_ LPWSTR wszPath, _In_ PVMMOB_MAP_HANDLE pHandleMap)
{
    QWORD qwHandle = 0;
    if(_wcsnicmp(L"by-id\\", wszPath, 6)) { return NULL; }
    qwHandle = wcstoull(wszPath + 6, NULL, 16);
    return Util_qfind((PVOID)qwHandle, pHandleMap->cMap, pHandleMap->pMap, sizeof(VMM_MAP_HANDLEENTRY), MHandle_HandleFromPath_qfind);
}

/*
* Read : function as specified by the module manager. The module manager will
* call into this callback function whenever a read shall occur from a "file".
* -- ctx
* -- pb
* -- cb
* -- pcbRead
* -- cbOffset
* -- return
*/
_Success_(return == 0)
NTSTATUS MHandle_Read(_In_ PVMMDLL_PLUGIN_CONTEXT ctx, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    NTSTATUS nt = VMMDLL_STATUS_FILE_INVALID;
    PVMMOB_MAP_HANDLE pObHandleMap = NULL;
    PVMM_MAP_HANDLEENTRY pe;
    if(VmmMap_GetHandle(ctx->pProcess, &pObHandleMap, TRUE)) {
        if(!_wcsicmp(ctx->wszPath, L"handles.txt")) {
            nt = Util_VfsLineFixed_Read(
                MHandle_ReadLine_Callback, NULL, MHANDLE_LINELENGTH, MHANDLE_LINEHEADER,
                pObHandleMap->pMap, pObHandleMap->cMap, sizeof(VMM_MAP_HANDLEENTRY),
                pb, cb, pcbRead, cbOffset
            );
        }
        if((pe = MHandle_HandleFromPath(ctx->wszPath, pObHandleMap))) {
            nt = VmmWinObjDisplay_VfsRead(ctx->wszPath, pe->iType, pe->vaObject, pb, cb, pcbRead, cbOffset);
        }
    }
    Ob_DECREF(pObHandleMap);
    return nt;
}

/*
* List : function as specified by the module manager. The module manager will
* call into this callback function whenever a list directory shall occur from
* the given module.
* -- ctx
* -- pFileList
* -- return
*/
BOOL MHandle_List(_In_ PVMMDLL_PLUGIN_CONTEXT ctx, _Inout_ PHANDLE pFileList)
{
    DWORD i;
    WCHAR wsz[MAX_PATH];
    PVMMWIN_OBJECT_TYPE ptp;
    PVMM_MAP_HANDLEENTRY pe;
    PVMMOB_MAP_HANDLE pObHandleMap = NULL;
    if(!ctx->wszPath[0]) {
        if(VmmMap_GetHandle(ctx->pProcess, &pObHandleMap, FALSE)) {
            VMMDLL_VfsList_AddFile(pFileList, L"handles.txt", UTIL_VFSLINEFIXED_LINECOUNT(pObHandleMap->cMap) * MHANDLE_LINELENGTH, NULL);
            VMMDLL_VfsList_AddDirectory(pFileList, L"by-id", NULL);
        }
        goto finish;
    }
    if(!VmmMap_GetHandle(ctx->pProcess, &pObHandleMap, TRUE)) { return TRUE; }
    if(!_wcsicmp(L"by-id", ctx->wszPath)) {
        for(i = 0; i < pObHandleMap->cMap; i++) {
            pe = pObHandleMap->pMap + i;
            if((ptp = VmmWin_ObjectTypeGet((BYTE)pe->iType)) && ptp->wsz) {
                _snwprintf_s(wsz, MAX_PATH, _TRUNCATE, L"%05X-%s", pe->dwHandle, ptp->wsz);
                VMMDLL_VfsList_AddDirectory(pFileList, wsz, NULL);
            }
        }
        goto finish;
    }
    if((pe = MHandle_HandleFromPath(ctx->wszPath, pObHandleMap))) {
        VmmWinObjDisplay_VfsList(pe->iType, pe->vaObject, pFileList);
        goto finish;
    }
finish:
    Ob_DECREF_NULL(&pObHandleMap);
    return TRUE;
}

/*
* Initialization function. The module manager shall call into this function
* when the module shall be initialized. If the module wish to initialize it
* shall call the supplied pfnPluginManager_Register function.
* NB! the module does not have to register itself - for example if the target
* operating system or architecture is unsupported.
* -- pPluginRegInfo
*/
VOID M_Handle_Initialize(_Inout_ PVMMDLL_PLUGIN_REGINFO pRI)
{
    if((pRI->magic != VMMDLL_PLUGIN_REGINFO_MAGIC) || (pRI->wVersion != VMMDLL_PLUGIN_REGINFO_VERSION)) { return; }
    if(!((pRI->tpSystem == VMM_SYSTEM_WINDOWS_X64) || (pRI->tpSystem == VMM_SYSTEM_WINDOWS_X86))) { return; }
    wcscpy_s(pRI->reg_info.wszPathName, 128, L"\\handles");             // module name
    pRI->reg_info.fRootModule = FALSE;                                  // module shows in root directory
    pRI->reg_info.fProcessModule = TRUE;                                // module shows in process directory
    pRI->reg_fn.pfnList = MHandle_List;                              // List function supported
    pRI->reg_fn.pfnRead = MHandle_Read;                              // Read function supported
    pRI->pfnPluginManager_Register(pRI);
}
