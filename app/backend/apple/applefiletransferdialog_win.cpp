#include "applefiletransferdialog.h"

#include <QDir>

#include <objbase.h>
#include <shobjidl.h>
#include <windows.h>

QString chooseAppleFileTransferDirectory(const QString& title,
                                         const QString& initialDirectory,
                                         void* ownerWindow)
{
    const HRESULT initialized = CoInitializeEx(
            nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninitializes = SUCCEEDED(initialized);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return {};

    IFileOpenDialog* dialog = nullptr;
    HRESULT result = CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog));
    if (FAILED(result) || dialog == nullptr) {
        if (uninitializes) CoUninitialize();
        return {};
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                           FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT);
    }
    dialog->SetTitle(reinterpret_cast<LPCWSTR>(title.utf16()));

    IShellItem* initial = nullptr;
    const QString nativeInitial = QDir::toNativeSeparators(initialDirectory);
    if (!nativeInitial.isEmpty() &&
            SUCCEEDED(SHCreateItemFromParsingName(
                    reinterpret_cast<LPCWSTR>(nativeInitial.utf16()),
                    nullptr, IID_PPV_ARGS(&initial)))) {
        dialog->SetFolder(initial);
        initial->Release();
    }

    QString selected;
    result = dialog->Show(static_cast<HWND>(ownerWindow));
    if (SUCCEEDED(result)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) &&
                    path != nullptr) {
                selected = QDir::fromNativeSeparators(
                        QString::fromWCharArray(path));
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    if (uninitializes) CoUninitialize();
    return selected;
}
