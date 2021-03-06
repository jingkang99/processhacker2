/*
 * Process Hacker Online Checks -
 *   Main Program
 *
 * Copyright (C) 2010-2013 wj32
 * Copyright (C) 2012-2017 dmex
 *
 * This file is part of Process Hacker.
 *
 * Process Hacker is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Process Hacker is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Process Hacker.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "onlnchk.h"
#include "db.h"

PPH_PLUGIN PluginInstance;
PH_CALLBACK_REGISTRATION PluginLoadCallbackRegistration;
PH_CALLBACK_REGISTRATION PluginShowOptionsCallbackRegistration;
PH_CALLBACK_REGISTRATION PluginMenuItemCallbackRegistration;
PH_CALLBACK_REGISTRATION MainMenuInitializingCallbackRegistration;
PH_CALLBACK_REGISTRATION ProcessesUpdatedCallbackRegistration;
PH_CALLBACK_REGISTRATION ProcessHighlightingColorCallbackRegistration;
PH_CALLBACK_REGISTRATION ProcessMenuInitializingCallbackRegistration;
PH_CALLBACK_REGISTRATION ModuleMenuInitializingCallbackRegistration;
PH_CALLBACK_REGISTRATION ServiceMenuInitializingCallbackRegistration;
PH_CALLBACK_REGISTRATION TreeNewMessageCallbackRegistration;
PH_CALLBACK_REGISTRATION ProcessTreeNewInitializingCallbackRegistration;
PH_CALLBACK_REGISTRATION ModulesTreeNewInitializingCallbackRegistration;
PH_CALLBACK_REGISTRATION ServiceTreeNewInitializingCallbackRegistration;

BOOLEAN VirusTotalScanningEnabled = FALSE;
ULONG ProcessesUpdatedCount = 0;
LIST_ENTRY ProcessListHead = { &ProcessListHead, &ProcessListHead };
PH_QUEUED_LOCK ProcessesListLock = PH_QUEUED_LOCK_INIT;

VOID ProcessesUpdatedCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    static ULONG ProcessesUpdatedCount = 0;
    PLIST_ENTRY listEntry;

    if (!VirusTotalScanningEnabled)
        return;

    ProcessesUpdatedCount++;

    if (ProcessesUpdatedCount < 2)
        return;

    listEntry = ProcessListHead.Flink;

    while (listEntry != &ProcessListHead)
    {
        PPROCESS_EXTENSION extension;
        PPH_STRING filePath = NULL;

        extension = CONTAINING_RECORD(listEntry, PROCESS_EXTENSION, ListEntry);

        if (extension->ProcessItem)
        {
            filePath = extension->ProcessItem->FileName;
        }
        else if (extension->ModuleItem)
        {
            filePath = extension->ModuleItem->FileName;
        }
        else if (extension->ServiceItem)
        {
            if (extension->FilePath)
            {
                filePath = extension->FilePath;
            }
            else
            {
                PPH_STRING serviceFileName = NULL;
                PPH_STRING serviceBinaryPath = NULL;

                if (NT_SUCCESS(QueryServiceFileName(
                    &extension->ServiceItem->Name->sr,
                    &serviceFileName,
                    &serviceBinaryPath
                    )))
                {
                    PhMoveReference(&extension->FilePath, serviceFileName);
                    if (serviceBinaryPath) PhDereferenceObject(serviceBinaryPath);
                }

                filePath = extension->FilePath;
            }
        }

        if (!PhIsNullOrEmptyString(filePath))
        {
            if (!extension->ResultValid)
            {
                PPROCESS_DB_OBJECT object;

                if (object = FindProcessDbObject(&filePath->sr))
                {
                    extension->Stage1 = TRUE;
                    extension->ResultValid = TRUE;

                    extension->Positives = object->Positives;
                    PhSwapReference(&extension->VirusTotalResult, object->Result);
                }
            }

            if (!extension->Stage1)
            {
                if (!VirusTotalGetCachedResult(filePath))
                {
                    VirusTotalAddCacheResult(filePath, extension);
                }

                extension->Stage1 = TRUE;
            }
        }

        listEntry = listEntry->Flink;
    }
}

VOID NTAPI LoadCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    if (VirusTotalScanningEnabled = !!PhGetIntegerSetting(SETTING_NAME_VIRUSTOTAL_SCAN_ENABLED))
    {
        InitializeProcessDb();
        InitializeVirusTotalProcessMonitor();
    }
}

VOID NTAPI ShowOptionsCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    ShowOptionsDialog((HWND)Parameter);
}

VOID NTAPI MenuItemCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_MENU_ITEM menuItem = Parameter;

    switch (menuItem->Id)
    {
    case ENABLE_SERVICE_VIRUSTOTAL:
        {
            ULONG scanningEnabled = !VirusTotalScanningEnabled;

            PhSetIntegerSetting(SETTING_NAME_VIRUSTOTAL_SCAN_ENABLED, scanningEnabled);

            if (VirusTotalScanningEnabled != scanningEnabled)
            {
                INT result = IDOK;
                TASKDIALOGCONFIG config;

                memset(&config, 0, sizeof(TASKDIALOGCONFIG));
                config.cbSize = sizeof(TASKDIALOGCONFIG);
                config.dwFlags = TDF_USE_HICON_MAIN | TDF_ALLOW_DIALOG_CANCELLATION;
                config.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
                config.hwndParent = menuItem->OwnerWindow;
                config.hMainIcon = PH_LOAD_SHARED_ICON_LARGE(PhImageBaseAddress, MAKEINTRESOURCE(PHAPP_IDI_PROCESSHACKER));
                config.cxWidth = 180;
                config.pszWindowTitle = L"Process Hacker - VirusTotal";
                config.pszMainInstruction = L"VirusTotal scanning requires a restart of Process Hacker.";
                config.pszContent = L"Do you want to restart Process Hacker now?";

                if (SUCCEEDED(TaskDialogIndirect(&config, &result, NULL, NULL)) && result == IDYES)
                {
                    ProcessHacker_PrepareForEarlyShutdown(PhMainWindowHandle);
                    PhShellProcessHacker(
                        PhMainWindowHandle,
                        L"-v",
                        SW_SHOW,
                        0,
                        PH_SHELL_APP_PROPAGATE_PARAMETERS | PH_SHELL_APP_PROPAGATE_PARAMETERS_IGNORE_VISIBILITY,
                        0,
                        NULL
                        );
                    ProcessHacker_Destroy(PhMainWindowHandle);
                }

                DestroyIcon(config.hMainIcon);
            }
        }
        break;
    case MENUITEM_VIRUSTOTAL_UPLOAD:
        UploadToOnlineService(menuItem->Context, MENUITEM_VIRUSTOTAL_UPLOAD);
        break;
    case MENUITEM_JOTTI_UPLOAD:
        UploadToOnlineService(menuItem->Context, MENUITEM_JOTTI_UPLOAD);
        break;
    case MENUITEM_VIRUSTOTAL_UPLOAD_FILE:
        {
            static PH_FILETYPE_FILTER filters[] =
            {
                { L"All files (*.*)", L"*.*" }
            };
            PVOID fileDialog;
            PPH_STRING fileName;

            fileDialog = PhCreateOpenFileDialog();
            PhSetFileDialogFilter(fileDialog, filters, sizeof(filters) / sizeof(PH_FILETYPE_FILTER));

            if (PhShowFileDialog(menuItem->Context, fileDialog))
            {
                fileName = PH_AUTO(PhGetFileDialogFileName(fileDialog));

                UploadToOnlineService(fileName, MENUITEM_VIRUSTOTAL_UPLOAD);
            }

            PhFreeFileDialog(fileDialog);
        }
        break;
    }
}

VOID NTAPI MainMenuInitializingCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_EMENU_ITEM onlineMenuItem;
    PPH_EMENU_ITEM enableMenuItem;
    PPH_PLUGIN_MENU_INFORMATION menuInfo = Parameter;

    if (menuInfo->u.MainMenu.SubMenuIndex != PH_MENU_ITEM_LOCATION_TOOLS)
        return;

    onlineMenuItem = PhPluginCreateEMenuItem(PluginInstance, 0, 0, L"Online Checks", NULL);
    PhInsertEMenuItem(onlineMenuItem, enableMenuItem = PhPluginCreateEMenuItem(PluginInstance, 0, ENABLE_SERVICE_VIRUSTOTAL, L"Enable VirusTotal scanning", NULL), -1);
    PhInsertEMenuItem(onlineMenuItem, PhPluginCreateEMenuItem(PluginInstance, PH_EMENU_SEPARATOR, 0, NULL, NULL), -1);
    PhInsertEMenuItem(onlineMenuItem, PhPluginCreateEMenuItem(PluginInstance, 0, MENUITEM_VIRUSTOTAL_UPLOAD_FILE, L"Upload file to VirusTotal...", NULL), -1);
    PhInsertEMenuItem(onlineMenuItem, PhPluginCreateEMenuItem(PluginInstance, 0, MENUITEM_VIRUSTOTAL_QUEUE, L"Upload unknown files to VirusTotal...", NULL), -1);
    PhInsertEMenuItem(menuInfo->Menu, onlineMenuItem, -1);

    if (VirusTotalScanningEnabled)
        enableMenuItem->Flags |= PH_EMENU_CHECKED;
}

PPH_EMENU_ITEM CreateSendToMenu(
    _In_ PPH_EMENU_ITEM Parent,
    _In_ PPH_STRING FileName
    )
{
    PPH_EMENU_ITEM sendToMenu;
    PPH_EMENU_ITEM menuItem;
    ULONG insertIndex;

    sendToMenu = PhPluginCreateEMenuItem(PluginInstance, 0, 0, L"Send to", NULL);
    PhInsertEMenuItem(sendToMenu, PhPluginCreateEMenuItem(PluginInstance, 0, MENUITEM_VIRUSTOTAL_UPLOAD, L"virustotal.com", FileName), -1);
    PhInsertEMenuItem(sendToMenu, PhPluginCreateEMenuItem(PluginInstance, 0, MENUITEM_JOTTI_UPLOAD, L"virusscan.jotti.org", FileName), -1);

    if (menuItem = PhFindEMenuItem(Parent, PH_EMENU_FIND_STARTSWITH, L"Search online", 0))
    {
        insertIndex = PhIndexOfEMenuItem(Parent, menuItem);
        PhInsertEMenuItem(Parent, sendToMenu, insertIndex + 1);
        PhInsertEMenuItem(Parent, PhPluginCreateEMenuItem(PluginInstance, PH_EMENU_SEPARATOR, 0, NULL, NULL), insertIndex + 2);
    }
    else
    {
        PhInsertEMenuItem(Parent, PhPluginCreateEMenuItem(PluginInstance, PH_EMENU_SEPARATOR, 0, NULL, NULL), -1);
        PhInsertEMenuItem(Parent, sendToMenu, -1);
    }

    return sendToMenu;
}

VOID NTAPI ProcessMenuInitializingCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_MENU_INFORMATION menuInfo = Parameter;
    PPH_PROCESS_ITEM processItem;
    PPH_EMENU_ITEM sendToMenu;

    if (menuInfo->u.Process.NumberOfProcesses == 1)
        processItem = menuInfo->u.Process.Processes[0];
    else
        processItem = NULL;

    sendToMenu = CreateSendToMenu(menuInfo->Menu, processItem ? processItem->FileName : NULL);

    // Only enable the Send To menu if there is exactly one process selected and it has a file name.
    if (!processItem || !processItem->FileName)
    {
        sendToMenu->Flags |= PH_EMENU_DISABLED;
    }
}

VOID NTAPI ModuleMenuInitializingCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_MENU_INFORMATION menuInfo = Parameter;
    PPH_MODULE_ITEM moduleItem;
    PPH_EMENU_ITEM sendToMenu;

    if (menuInfo->u.Module.NumberOfModules == 1)
        moduleItem = menuInfo->u.Module.Modules[0];
    else
        moduleItem = NULL;

    sendToMenu = CreateSendToMenu(menuInfo->Menu, moduleItem ? moduleItem->FileName : NULL);

    if (!moduleItem)
    {
        sendToMenu->Flags |= PH_EMENU_DISABLED;
    }
}

VOID NTAPI ServiceMenuInitializingCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_MENU_INFORMATION menuInfo = Parameter;
    PPH_SERVICE_ITEM serviceItem;
    PPH_EMENU_ITEM sendToMenu;
    PPH_STRING serviceFileName = NULL;
    PPH_STRING serviceBinaryPath = NULL;

    if (menuInfo->u.Service.NumberOfServices == 1)
        serviceItem = menuInfo->u.Service.Services[0];
    else
        serviceItem = NULL;

    QueryServiceFileName(
        &serviceItem->Name->sr,
        &serviceFileName,
        &serviceBinaryPath
        );

    if (serviceBinaryPath) PhDereferenceObject(serviceBinaryPath);

    //  TODO: Possible serviceFileName string memory leak.
    sendToMenu = CreateSendToMenu(menuInfo->Menu, serviceFileName ? serviceFileName : NULL);

    if (!serviceItem || !serviceFileName)
    {
        sendToMenu->Flags |= PH_EMENU_DISABLED;
    }
}

VOID ProcessHighlightingColorCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_GET_HIGHLIGHTING_COLOR getHighlightingColor = Parameter;
    PPH_PROCESS_ITEM processItem = (PPH_PROCESS_ITEM)getHighlightingColor->Parameter;
    //PPROCESS_DB_OBJECT object;

    if (getHighlightingColor->Handled)
        return;

    //if (!PhGetIntegerSetting(SETTING_NAME_VIRUSTOTAL_HIGHLIGHT_DETECTIONS))
    //    return;

    //LockProcessDb();

    //if (PhIsNullOrEmptyString(processItem->FileName))
    //    return;

    //if ((object = FindProcessDbObject(&processItem->FileName->sr)) && object->Positives)
    //{
    //    getHighlightingColor->BackColor = RGB(255, 0, 0);
    //    getHighlightingColor->Cache = TRUE;
    //    getHighlightingColor->Handled = TRUE;
    //}

    //UnlockProcessDb();
}

LONG NTAPI VirusTotalProcessNodeSortFunction(
    _In_ PVOID Node1,
    _In_ PVOID Node2,
    _In_ ULONG SubId,
    _In_ PVOID Context
    )
{
    PPH_PROCESS_NODE node1 = Node1;
    PPH_PROCESS_NODE node2 = Node2;
    PPROCESS_EXTENSION extension1 = PhPluginGetObjectExtension(PluginInstance, node1->ProcessItem, EmProcessItemType);
    PPROCESS_EXTENSION extension2 = PhPluginGetObjectExtension(PluginInstance, node2->ProcessItem, EmProcessItemType);

    return PhCompareStringWithNull(extension1->VirusTotalResult, extension2->VirusTotalResult, TRUE);
}

LONG NTAPI VirusTotalModuleNodeSortFunction(
    _In_ PVOID Node1,
    _In_ PVOID Node2,
    _In_ ULONG SubId,
    _In_ PVOID Context
    )
{
    PPH_MODULE_NODE node1 = Node1;
    PPH_MODULE_NODE node2 = Node2;
    PPROCESS_EXTENSION extension1 = PhPluginGetObjectExtension(PluginInstance, node1->ModuleItem, EmModuleItemType);
    PPROCESS_EXTENSION extension2 = PhPluginGetObjectExtension(PluginInstance, node2->ModuleItem, EmModuleItemType);

    return PhCompareStringWithNull(extension1->VirusTotalResult, extension2->VirusTotalResult, TRUE);
}

LONG NTAPI VirusTotalServiceNodeSortFunction(
    _In_ PVOID Node1,
    _In_ PVOID Node2,
    _In_ ULONG SubId,
    _In_ PVOID Context
    )
{
    PPH_SERVICE_NODE node1 = Node1;
    PPH_SERVICE_NODE node2 = Node2;
    PPROCESS_EXTENSION extension1 = PhPluginGetObjectExtension(PluginInstance, node1->ServiceItem, EmServiceItemType);
    PPROCESS_EXTENSION extension2 = PhPluginGetObjectExtension(PluginInstance, node2->ServiceItem, EmServiceItemType);

    return PhCompareStringWithNull(extension1->VirusTotalResult, extension2->VirusTotalResult, TRUE);
}

VOID NTAPI ProcessTreeNewInitializingCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_TREENEW_INFORMATION info = Parameter;
    PH_TREENEW_COLUMN column;

    memset(&column, 0, sizeof(PH_TREENEW_COLUMN));
    column.Text = L"VirusTotal";
    column.Width = 140;
    column.Alignment = PH_ALIGN_CENTER;
    column.CustomDraw = TRUE;
    column.Context = info->TreeNewHandle; // Context

    PhPluginAddTreeNewColumn(PluginInstance, info->CmData, &column, COLUMN_ID_VIUSTOTAL_PROCESS, NULL, VirusTotalProcessNodeSortFunction);
}

VOID NTAPI ModuleTreeNewInitializingCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_TREENEW_INFORMATION info = Parameter;
    PH_TREENEW_COLUMN column;

    memset(&column, 0, sizeof(PH_TREENEW_COLUMN));
    column.Text = L"VirusTotal";
    column.Width = 140;
    column.Alignment = PH_ALIGN_CENTER;
    column.CustomDraw = TRUE;
    column.Context = info->TreeNewHandle; // Context

    PhPluginAddTreeNewColumn(PluginInstance, info->CmData, &column, COLUMN_ID_VIUSTOTAL_MODULE, NULL, VirusTotalModuleNodeSortFunction);
}

VOID NTAPI ServiceTreeNewInitializingCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_TREENEW_INFORMATION info = Parameter;
    PH_TREENEW_COLUMN column;

    memset(&column, 0, sizeof(PH_TREENEW_COLUMN));
    column.Text = L"VirusTotal";
    column.Width = 140;
    column.Alignment = PH_ALIGN_CENTER;
    column.CustomDraw = TRUE;
    column.Context = info->TreeNewHandle; // Context

    PhPluginAddTreeNewColumn(PluginInstance, info->CmData, &column, COLUMN_ID_VIUSTOTAL_SERVICE, NULL, VirusTotalServiceNodeSortFunction);
}

VOID NTAPI TreeNewMessageCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_TREENEW_MESSAGE message = Parameter;

    switch (message->Message)
    {
    case TreeNewGetCellText:
        {
            PPH_TREENEW_GET_CELL_TEXT getCellText = message->Parameter1;

            switch (message->SubId)
            {
            case COLUMN_ID_VIUSTOTAL_PROCESS:
                {
                    PPH_PROCESS_NODE processNode = (PPH_PROCESS_NODE)getCellText->Node;
                    PPROCESS_EXTENSION extension = PhPluginGetObjectExtension(PluginInstance, processNode->ProcessItem, EmProcessItemType);

                    getCellText->Text = PhGetStringRef(extension->VirusTotalResult);
                }
                break;
            case COLUMN_ID_VIUSTOTAL_MODULE:
                {
                    PPH_MODULE_NODE moduleNode = (PPH_MODULE_NODE)getCellText->Node;
                    PPROCESS_EXTENSION extension = PhPluginGetObjectExtension(PluginInstance, moduleNode->ModuleItem, EmModuleItemType);

                    getCellText->Text = PhGetStringRef(extension->VirusTotalResult);
                }
                break;
            case COLUMN_ID_VIUSTOTAL_SERVICE:
                {
                    PPH_SERVICE_NODE serviceNode = (PPH_SERVICE_NODE)getCellText->Node;
                    PPROCESS_EXTENSION extension = PhPluginGetObjectExtension(PluginInstance, serviceNode->ServiceItem, EmServiceItemType);

                    getCellText->Text = PhGetStringRef(extension->VirusTotalResult);
                }
                break;
            }
        }
        break;
    case TreeNewCustomDraw:
        {
            PPH_TREENEW_CUSTOM_DRAW customDraw = message->Parameter1;
            PPROCESS_EXTENSION extension = NULL;
            PH_STRINGREF text;

            if (!VirusTotalScanningEnabled)
            {
                static PH_STRINGREF disabledText = PH_STRINGREF_INIT(L"Scanning disabled");

                DrawText(
                    customDraw->Dc,
                    disabledText.Buffer,
                    (ULONG)disabledText.Length / 2,
                    &customDraw->CellRect,
                    DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS | DT_SINGLELINE
                    );

                return;
            }

            switch (message->SubId)
            {
            case COLUMN_ID_VIUSTOTAL_PROCESS:
                {
                    PPH_PROCESS_NODE processNode = (PPH_PROCESS_NODE)customDraw->Node;

                    extension = PhPluginGetObjectExtension(PluginInstance, processNode->ProcessItem, EmProcessItemType);
                }
                break;
            case COLUMN_ID_VIUSTOTAL_MODULE:
                {
                    PPH_MODULE_NODE moduleNode = (PPH_MODULE_NODE)customDraw->Node;

                    extension = PhPluginGetObjectExtension(PluginInstance, moduleNode->ModuleItem, EmModuleItemType);
                }
                break;
            case COLUMN_ID_VIUSTOTAL_SERVICE:
                {
                    PPH_SERVICE_NODE serviceNode = (PPH_SERVICE_NODE)customDraw->Node;

                    extension = PhPluginGetObjectExtension(PluginInstance, serviceNode->ServiceItem, EmServiceItemType);
                }
                break;
            }

            if (!extension)
                break;

            text = PhGetStringRef(extension->VirusTotalResult);
            DrawText(
                customDraw->Dc,
                text.Buffer,
                (ULONG)text.Length / 2,
                &customDraw->CellRect,
                DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS | DT_SINGLELINE
                );
        }
        break;
    }
}

VOID NTAPI ProcessItemCreateCallback(
    _In_ PVOID Object,
    _In_ PH_EM_OBJECT_TYPE ObjectType,
    _In_ PVOID Extension
    )
{
    PPH_PROCESS_ITEM processItem = Object;
    PPROCESS_EXTENSION extension = Extension;

    memset(extension, 0, sizeof(PROCESS_EXTENSION));

    extension->ProcessItem = processItem;
    extension->VirusTotalResult = PhReferenceEmptyString();

    PhAcquireQueuedLockExclusive(&ProcessesListLock);
    InsertTailList(&ProcessListHead, &extension->ListEntry);
    PhReleaseQueuedLockExclusive(&ProcessesListLock);
}

VOID NTAPI ProcessItemDeleteCallback(
    _In_ PVOID Object,
    _In_ PH_EM_OBJECT_TYPE ObjectType,
    _In_ PVOID Extension
    )
{
    PPH_PROCESS_ITEM processItem = Object;
    PPROCESS_EXTENSION extension = Extension;

    PhClearReference(&extension->VirusTotalResult);

    PhAcquireQueuedLockExclusive(&ProcessesListLock);
    RemoveEntryList(&extension->ListEntry);
    PhReleaseQueuedLockExclusive(&ProcessesListLock);
}

VOID NTAPI ModuleItemCreateCallback(
    _In_ PVOID Object,
    _In_ PH_EM_OBJECT_TYPE ObjectType,
    _In_ PVOID Extension
    )
{
    PPH_MODULE_ITEM moduleItem = Object;
    PPROCESS_EXTENSION extension = Extension;

    memset(extension, 0, sizeof(PROCESS_EXTENSION));

    extension->ModuleItem = moduleItem;
    extension->VirusTotalResult = PhReferenceEmptyString();

    PhAcquireQueuedLockExclusive(&ProcessesListLock);
    InsertTailList(&ProcessListHead, &extension->ListEntry);
    PhReleaseQueuedLockExclusive(&ProcessesListLock);
}

VOID NTAPI ModuleItemDeleteCallback(
    _In_ PVOID Object,
    _In_ PH_EM_OBJECT_TYPE ObjectType,
    _In_ PVOID Extension
    )
{
    PPH_MODULE_ITEM processItem = Object;
    PPROCESS_EXTENSION extension = Extension;

    PhClearReference(&extension->VirusTotalResult);

    PhAcquireQueuedLockExclusive(&ProcessesListLock);
    RemoveEntryList(&extension->ListEntry);
    PhReleaseQueuedLockExclusive(&ProcessesListLock);
}

VOID NTAPI ServiceItemCreateCallback(
    _In_ PVOID Object,
    _In_ PH_EM_OBJECT_TYPE ObjectType,
    _In_ PVOID Extension
    )
{
    PPH_SERVICE_ITEM serviceItem = Object;
    PPROCESS_EXTENSION extension = Extension;

    memset(extension, 0, sizeof(PROCESS_EXTENSION));

    extension->ServiceItem = serviceItem;
    extension->VirusTotalResult = PhReferenceEmptyString();

    PhAcquireQueuedLockExclusive(&ProcessesListLock);
    InsertTailList(&ProcessListHead, &extension->ListEntry);
    PhReleaseQueuedLockExclusive(&ProcessesListLock);
}

VOID NTAPI ServiceItemDeleteCallback(
    _In_ PVOID Object,
    _In_ PH_EM_OBJECT_TYPE ObjectType,
    _In_ PVOID Extension
    )
{
    PPH_SERVICE_ITEM serviceItem = Object;
    PPROCESS_EXTENSION extension = Extension;

    PhClearReference(&extension->VirusTotalResult);

    PhAcquireQueuedLockExclusive(&ProcessesListLock);
    RemoveEntryList(&extension->ListEntry);
    PhReleaseQueuedLockExclusive(&ProcessesListLock);
}

LOGICAL DllMain(
    _In_ HINSTANCE Instance,
    _In_ ULONG Reason,
    _Reserved_ PVOID Reserved
    )
{
    switch (Reason)
    {
    case DLL_PROCESS_ATTACH:
        {
            PPH_PLUGIN_INFORMATION info;
            PH_SETTING_CREATE settings[] =
            {
                { IntegerSettingType, SETTING_NAME_VIRUSTOTAL_SCAN_ENABLED, L"0" },
                { IntegerSettingType, SETTING_NAME_VIRUSTOTAL_HIGHLIGHT_DETECTIONS, L"0" }
            };

            PluginInstance = PhRegisterPlugin(PLUGIN_NAME, Instance, &info);

            if (!PluginInstance)
                return FALSE;

            info->DisplayName = L"Online Checks";
            info->Author = L"dmex, wj32";
            info->Description = L"Allows files to be checked with online services.";
            info->Url = L"https://wj32.org/processhacker/forums/viewtopic.php?t=1118";
            info->HasOptions = TRUE;

            PhRegisterCallback(
                PhGetPluginCallback(PluginInstance, PluginCallbackLoad),
                LoadCallback,
                NULL,
                &PluginLoadCallbackRegistration
                );
            PhRegisterCallback(
                PhGetPluginCallback(PluginInstance, PluginCallbackShowOptions),
                ShowOptionsCallback,
                NULL,
                &PluginShowOptionsCallbackRegistration
                );
            PhRegisterCallback(
                PhGetPluginCallback(PluginInstance, PluginCallbackMenuItem),
                MenuItemCallback,
                NULL,
                &PluginMenuItemCallbackRegistration
                );
            PhRegisterCallback(
                PhGetGeneralCallback(GeneralCallbackMainMenuInitializing),
                MainMenuInitializingCallback,
                NULL,
                &MainMenuInitializingCallbackRegistration
                );
            PhRegisterCallback(
                PhGetGeneralCallback(GeneralCallbackProcessMenuInitializing),
                ProcessMenuInitializingCallback,
                NULL,
                &ProcessMenuInitializingCallbackRegistration
                );
            PhRegisterCallback(
                PhGetGeneralCallback(GeneralCallbackModuleMenuInitializing),
                ModuleMenuInitializingCallback,
                NULL,
                &ModuleMenuInitializingCallbackRegistration
                );
            PhRegisterCallback(
                PhGetGeneralCallback(GeneralCallbackServiceMenuInitializing),
                ServiceMenuInitializingCallback,
                NULL,
                &ServiceMenuInitializingCallbackRegistration
                );

            PhRegisterCallback(
                PhGetGeneralCallback(GeneralCallbackProcessesUpdated),
                ProcessesUpdatedCallback,
                NULL,
                &ProcessesUpdatedCallbackRegistration
                );

            PhRegisterCallback(
                PhGetGeneralCallback(GeneralCallbackGetProcessHighlightingColor),
                ProcessHighlightingColorCallback,
                NULL,
                &ProcessHighlightingColorCallbackRegistration
                );

            PhRegisterCallback(
                PhGetGeneralCallback(GeneralCallbackProcessTreeNewInitializing),
                ProcessTreeNewInitializingCallback,
                NULL,
                &ProcessTreeNewInitializingCallbackRegistration
                );
            PhRegisterCallback(
                PhGetGeneralCallback(GeneralCallbackModuleTreeNewInitializing),
                ModuleTreeNewInitializingCallback,
                NULL,
                &ModulesTreeNewInitializingCallbackRegistration
                );
            PhRegisterCallback(
                PhGetGeneralCallback(GeneralCallbackServiceTreeNewInitializing),
                ServiceTreeNewInitializingCallback,
                NULL,
                &ServiceTreeNewInitializingCallbackRegistration
                );

            PhRegisterCallback(
                PhGetPluginCallback(PluginInstance, PluginCallbackTreeNewMessage),
                TreeNewMessageCallback,
                NULL,
                &TreeNewMessageCallbackRegistration
                );

            PhPluginSetObjectExtension(
                PluginInstance,
                EmProcessItemType,
                sizeof(PROCESS_EXTENSION),
                ProcessItemCreateCallback,
                ProcessItemDeleteCallback
                );

            PhPluginSetObjectExtension(
                PluginInstance,
                EmModuleItemType,
                sizeof(PROCESS_EXTENSION),
                ModuleItemCreateCallback,
                ModuleItemDeleteCallback
                );

            PhPluginSetObjectExtension(
                PluginInstance,
                EmServiceItemType,
                sizeof(PROCESS_EXTENSION),
                ServiceItemCreateCallback,
                ServiceItemDeleteCallback
                );

            PhAddSettings(settings, ARRAYSIZE(settings));
        }
        break;
    }

    return TRUE;
}