#include "applefiledrag_win.h"

#include <QDir>
#include <QDebug>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QTimer>
#include <QVector>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shldisp.h>
#include <shlwapi.h>
#include <windows.h>

namespace {

void setError(QString* error, const QString& value)
{
    if (error != nullptr) *error = value;
}

QString hresultText(HRESULT result)
{
    return QStringLiteral("Windows OLE error 0x%1")
            .arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
}

QStringList filePaths(IDataObject* dataObject)
{
    if (dataObject == nullptr) return {};
    FORMATETC format{
        static_cast<CLIPFORMAT>(CF_HDROP),
        nullptr,
        DVASPECT_CONTENT,
        -1,
        TYMED_HGLOBAL,
    };
    STGMEDIUM medium{};
    if (FAILED(dataObject->GetData(&format, &medium))) return {};

    QStringList paths;
    const HDROP drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
    if (drop != nullptr) {
        const UINT count = DragQueryFileW(drop, 0xffffffffu, nullptr, 0);
        for (UINT index = 0; index < count; ++index) {
            const UINT length = DragQueryFileW(drop, index, nullptr, 0);
            if (length == 0) continue;
            QVector<wchar_t> buffer(static_cast<qsizetype>(length) + 1);
            if (DragQueryFileW(drop, index, buffer.data(), length + 1) > 0) {
                paths.append(QString::fromWCharArray(buffer.constData()));
            }
        }
        GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return paths;
}

HGLOBAL preferredCopyEffectHandle()
{
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (handle == nullptr) return nullptr;
    auto* effect = static_cast<DWORD*>(GlobalLock(handle));
    if (effect == nullptr) {
        GlobalFree(handle);
        return nullptr;
    }
    *effect = DROPEFFECT_COPY;
    GlobalUnlock(handle);
    return handle;
}

struct PromisedFileEntry
{
    QString name;
};

QString remoteBaseName(const QString& path)
{
    const QString normalized = QString(path).replace('\\', '/');
    return normalized.section('/', -1, -1);
}

HGLOBAL fileDescriptorHandle(const std::vector<PromisedFileEntry>& entries)
{
    if (entries.empty() ||
            entries.size() > static_cast<size_t>(
                    (std::numeric_limits<UINT>::max)())) {
        return nullptr;
    }
    const SIZE_T byteCount = offsetof(FILEGROUPDESCRIPTORW, fgd) +
            entries.size() * sizeof(FILEDESCRIPTORW);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, byteCount);
    if (handle == nullptr) return nullptr;

    auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(handle));
    if (group == nullptr) {
        GlobalFree(handle);
        return nullptr;
    }
    group->cItems = static_cast<UINT>(entries.size());
    for (size_t index = 0; index < entries.size(); ++index) {
        const PromisedFileEntry& entry = entries[index];
        FILEDESCRIPTORW& descriptor = group->fgd[index];
        // Names are available from the remote drag notification. Size and
        // attributes arrive only with the file-copy stream, so advertising
        // them here would force Explorer's metadata probe to download the
        // payload on the stream window thread.
        descriptor.dwFlags = FD_PROGRESSUI;
        const std::wstring name = entry.name.toStdWString();
        std::copy(name.cbegin(), name.cend(), descriptor.cFileName);
        descriptor.cFileName[name.size()] = L'\0';
    }
    GlobalUnlock(handle);
    return handle;
}

DWORD dropEffectFromMedium(const STGMEDIUM& medium)
{
    if (medium.tymed != TYMED_HGLOBAL || medium.hGlobal == nullptr ||
            GlobalSize(medium.hGlobal) < sizeof(DWORD)) {
        return DROPEFFECT_NONE;
    }
    const auto* effect = static_cast<const DWORD*>(GlobalLock(medium.hGlobal));
    if (effect == nullptr) return DROPEFFECT_NONE;
    const DWORD value = *effect;
    GlobalUnlock(medium.hGlobal);
    return value;
}

bool formatsMatch(const FORMATETC& left, const FORMATETC& right)
{
    return left.cfFormat == right.cfFormat &&
            left.dwAspect == right.dwAspect &&
            left.lindex == right.lindex &&
            (left.tymed & right.tymed) != 0;
}

DVTARGETDEVICE* duplicateTargetDevice(const DVTARGETDEVICE* source)
{
    if (source == nullptr || source->tdSize < sizeof(DVTARGETDEVICE)) {
        return nullptr;
    }
    auto* copy = static_cast<DVTARGETDEVICE*>(
            CoTaskMemAlloc(source->tdSize));
    if (copy != nullptr) std::memcpy(copy, source, source->tdSize);
    return copy;
}

HRESULT duplicateStorageMedium(
        const FORMATETC& format,
        const STGMEDIUM& source,
        STGMEDIUM* destination)
{
    if (destination == nullptr) return E_POINTER;
    *destination = {};
    destination->tymed = source.tymed;
    if (source.pUnkForRelease != nullptr) {
        *destination = source;
        destination->pUnkForRelease->AddRef();
        return S_OK;
    }
    switch (source.tymed) {
    case TYMED_HGLOBAL:
        destination->hGlobal = static_cast<HGLOBAL>(OleDuplicateData(
                source.hGlobal, format.cfFormat, 0));
        return destination->hGlobal != nullptr ? S_OK : STG_E_MEDIUMFULL;
    case TYMED_GDI:
        destination->hBitmap = static_cast<HBITMAP>(CopyImage(
                source.hBitmap, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
        return destination->hBitmap != nullptr ? S_OK : STG_E_MEDIUMFULL;
    case TYMED_ENHMF:
        destination->hEnhMetaFile = CopyEnhMetaFileW(
                source.hEnhMetaFile, nullptr);
        return destination->hEnhMetaFile != nullptr
                ? S_OK : STG_E_MEDIUMFULL;
    case TYMED_MFPICT:
        destination->hMetaFilePict = static_cast<HMETAFILEPICT>(
                OleDuplicateData(source.hMetaFilePict, format.cfFormat, 0));
        return destination->hMetaFilePict != nullptr
                ? S_OK : STG_E_MEDIUMFULL;
    case TYMED_ISTREAM:
        destination->pstm = source.pstm;
        if (destination->pstm != nullptr) destination->pstm->AddRef();
        return destination->pstm != nullptr ? S_OK : E_POINTER;
    case TYMED_ISTORAGE:
        destination->pstg = source.pstg;
        if (destination->pstg != nullptr) destination->pstg->AddRef();
        return destination->pstg != nullptr ? S_OK : E_POINTER;
    case TYMED_FILE: {
        if (source.lpszFileName == nullptr) return E_POINTER;
        const size_t length = std::wcslen(source.lpszFileName) + 1;
        destination->lpszFileName = static_cast<LPOLESTR>(
                CoTaskMemAlloc(length * sizeof(wchar_t)));
        if (destination->lpszFileName == nullptr) return STG_E_MEDIUMFULL;
        std::memcpy(destination->lpszFileName, source.lpszFileName,
                    length * sizeof(wchar_t));
        return S_OK;
    }
    default:
        destination->tymed = TYMED_NULL;
        return DV_E_TYMED;
    }
}

QImage shellFileIcon(const QStringList& sourcePaths)
{
    if (sourcePaths.isEmpty()) return {};
    const QString nativePath = QDir::toNativeSeparators(
            sourcePaths.first());
    SHFILEINFOW info{};
    if (SHGetFileInfoW(
                reinterpret_cast<LPCWSTR>(nativePath.utf16()),
                FILE_ATTRIBUTE_NORMAL,
                &info,
                sizeof(info),
                SHGFI_ICON | SHGFI_LARGEICON |
                        SHGFI_USEFILEATTRIBUTES) == 0 ||
            info.hIcon == nullptr) {
        return {};
    }
    const QImage image = QImage::fromHICON(info.hIcon);
    DestroyIcon(info.hIcon);
    return image;
}

QImage dragImage(
        const QByteArray& png,
        const QStringList& sourcePaths,
        int size)
{
    QImage image = QImage::fromData(png, "PNG");
    if (image.isNull()) image = shellFileIcon(sourcePaths);
    if (image.isNull()) return {};
    image = image.scaled(
            size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QImage canvas(size, size, QImage::Format_ARGB32);
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    painter.drawImage((size - image.width()) / 2,
                      (size - image.height()) / 2,
                      image);
    painter.end();
    return canvas;
}

HBITMAP dragBitmap(const QImage& source)
{
    if (source.isNull()) return nullptr;
    const QImage image = source.convertToFormat(QImage::Format_ARGB32);
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = image.width();
    bitmapInfo.bmiHeader.biHeight = -image.height();
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(
            nullptr, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (bitmap == nullptr || pixels == nullptr) {
        if (bitmap != nullptr) DeleteObject(bitmap);
        return nullptr;
    }
    const qsizetype stride = image.width() * 4;
    for (int y = 0; y < image.height(); ++y) {
        std::memcpy(static_cast<BYTE*>(pixels) + y * stride,
                    image.constScanLine(y), stride);
    }
    return bitmap;
}

void initializeDragImage(
        IDataObject* dataObject,
        HWND window,
        const QByteArray& png,
        const QStringList& sourcePaths)
{
    if (dataObject == nullptr) return;
    UINT dpi = 96;
    if (window != nullptr && IsWindow(window)) dpi = GetDpiForWindow(window);
    const int size = qMax(32, MulDiv(48, static_cast<int>(dpi), 96));
    const QImage image = dragImage(png, sourcePaths, size);
    HBITMAP bitmap = dragBitmap(image);
    if (bitmap == nullptr) return;

    IDragSourceHelper* helper = nullptr;
    const HRESULT created = CoCreateInstance(
            CLSID_DragDropHelper,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&helper));
    if (SUCCEEDED(created) && helper != nullptr) {
        SHDRAGIMAGE drag{};
        drag.sizeDragImage = {image.width(), image.height()};
        drag.ptOffset = {image.width() / 2, image.height() / 2};
        drag.hbmpDragImage = bitmap;
        drag.crColorKey = CLR_NONE;
        helper->InitializeFromBitmap(&drag, dataObject);
        helper->Release();
    }
    DeleteObject(bitmap);
}

class RemoteDataObject final : public IDataObject,
                               public IDataObjectAsyncCapability
{
    struct Completion;

public:
    explicit RemoteDataObject(
            QStringList sourcePaths,
            AppleWindowsRemoteFileDragSource::Materialize materialize,
            std::shared_ptr<std::atomic_bool> cancelled,
            AppleWindowsRemoteFileDragSource::Finished finished = {})
        : m_SourcePaths(std::move(sourcePaths)),
          m_Materialize(std::move(materialize)),
          m_Cancelled(std::move(cancelled)),
          m_Finished(std::move(finished)),
          m_FileDescriptor(RegisterClipboardFormatW(
                  CFSTR_FILEDESCRIPTORW)),
          m_FileContents(RegisterClipboardFormatW(CFSTR_FILECONTENTS)),
          m_PreferredDropEffect(RegisterClipboardFormatW(
                  CFSTR_PREFERREDDROPEFFECT)),
          m_PerformedDropEffect(RegisterClipboardFormatW(
                  CFSTR_PERFORMEDDROPEFFECT)),
          m_LogicalPerformedDropEffect(RegisterClipboardFormatW(
                  CFSTR_LOGICALPERFORMEDDROPEFFECT))
    {
        for (const QString& sourcePath : std::as_const(m_SourcePaths)) {
            const QString name = remoteBaseName(sourcePath);
            if (name.isEmpty() || name.size() >= MAX_PATH) {
                m_Error = QStringLiteral(
                        "A promised file name is invalid for Windows Explorer: %1")
                                .arg(name);
                m_Entries.clear();
                break;
            }
            m_Entries.push_back({name});
        }
    }

    ~RemoteDataObject()
    {
        for (StoredMedium& value : m_Stored) {
            ReleaseStgMedium(&value.medium);
            CoTaskMemFree(value.format.ptd);
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID interfaceId,
            void** object) override
    {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (interfaceId == IID_IUnknown || interfaceId == IID_IDataObject) {
            *object = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        if (interfaceId == IID_IDataObjectAsyncCapability) {
            *object = static_cast<IDataObjectAsyncCapability*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_References; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG references = --m_References;
        if (references == 0) delete this;
        return references;
    }

    HRESULT STDMETHODCALLTYPE GetData(
            FORMATETC* format,
            STGMEDIUM* medium) override
    {
        if (format == nullptr || medium == nullptr) return E_POINTER;
        *medium = {};
        if (format->cfFormat == m_FileDescriptor &&
                (format->tymed & TYMED_HGLOBAL) != 0 &&
                format->lindex == -1) {
            if (m_Entries.empty()) return E_FAIL;
            HGLOBAL handle = fileDescriptorHandle(m_Entries);
            if (handle == nullptr) return STG_E_MEDIUMFULL;
            medium->tymed = TYMED_HGLOBAL;
            medium->hGlobal = handle;
            return S_OK;
        }
        if (format->cfFormat == m_FileContents &&
                (format->tymed & TYMED_ISTREAM) != 0 &&
                format->lindex >= 0) {
            const size_t index = static_cast<size_t>(format->lindex);
            if (index >= m_Entries.size()) return DV_E_LINDEX;
            if (!materialize()) return E_FAIL;
            if (index >= static_cast<size_t>(m_Paths.size())) return DV_E_LINDEX;
            const QFileInfo info(m_Paths.at(static_cast<qsizetype>(index)));
            if (info.isDir() && !info.isSymLink()) {
                setDataError(QStringLiteral(
                        "Dragging remote folders into Windows Explorer is not supported by the native file-promise format."));
                return DV_E_FORMATETC;
            }
            const QString nativePath = QDir::toNativeSeparators(
                    info.absoluteFilePath());
            IStream* stream = nullptr;
            const HRESULT result = SHCreateStreamOnFileEx(
                    reinterpret_cast<LPCWSTR>(nativePath.utf16()),
                    STGM_READ | STGM_SHARE_DENY_WRITE,
                    FILE_ATTRIBUTE_NORMAL,
                    FALSE,
                    nullptr,
                    &stream);
            if (FAILED(result)) return result;
            medium->tymed = TYMED_ISTREAM;
            medium->pstm = stream;
            return S_OK;
        }
        if (format->cfFormat == m_PreferredDropEffect &&
                (format->tymed & TYMED_HGLOBAL) != 0) {
            HGLOBAL handle = preferredCopyEffectHandle();
            if (handle == nullptr) return STG_E_MEDIUMFULL;
            medium->tymed = TYMED_HGLOBAL;
            medium->hGlobal = handle;
            return S_OK;
        }
        std::lock_guard<std::mutex> lock(m_StateMutex);
        for (const StoredMedium& value : m_Stored) {
            if (formatsMatch(*format, value.format)) {
                return duplicateStorageMedium(
                        value.format, value.medium, medium);
            }
        }
        return DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override
    {
        return DATA_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override
    {
        if (format == nullptr) return E_POINTER;
        if (format->dwAspect != DVASPECT_CONTENT ||
                format->tymed == TYMED_NULL) {
            return DV_E_FORMATETC;
        }
        if ((format->tymed & TYMED_HGLOBAL) != 0 &&
                (format->cfFormat == m_FileDescriptor ||
                 format->cfFormat == m_PreferredDropEffect) &&
                format->lindex == -1) {
            return S_OK;
        }
        if ((format->tymed & TYMED_ISTREAM) != 0 &&
                format->cfFormat == m_FileContents &&
                format->lindex >= 0) {
            return S_OK;
        }
        std::lock_guard<std::mutex> lock(m_StateMutex);
        for (const StoredMedium& value : m_Stored) {
            if (formatsMatch(*format, value.format)) return S_OK;
        }
        return DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(
            FORMATETC*, FORMATETC* output) override
    {
        if (output == nullptr) return E_POINTER;
        output->ptd = nullptr;
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE SetData(
            FORMATETC* format,
            STGMEDIUM* medium,
            BOOL release) override
    {
        if (format == nullptr || medium == nullptr) return E_POINTER;
        if (format->cfFormat == m_PerformedDropEffect ||
                format->cfFormat == m_LogicalPerformedDropEffect) {
            std::lock_guard<std::mutex> lock(m_StateMutex);
            m_ReportedDropEffect |= dropEffectFromMedium(*medium);
        }
        StoredMedium stored;
        stored.format = *format;
        stored.format.ptd = duplicateTargetDevice(format->ptd);
        if (format->ptd != nullptr && stored.format.ptd == nullptr) {
            return E_OUTOFMEMORY;
        }
        if (release) {
            stored.medium = *medium;
        }
        else {
            const HRESULT duplicated = duplicateStorageMedium(
                    *format, *medium, &stored.medium);
            if (FAILED(duplicated)) {
                CoTaskMemFree(stored.format.ptd);
                return duplicated;
            }
        }
        std::lock_guard<std::mutex> lock(m_StateMutex);
        for (StoredMedium& value : m_Stored) {
            if (!formatsMatch(stored.format, value.format)) continue;
            ReleaseStgMedium(&value.medium);
            CoTaskMemFree(value.format.ptd);
            value = stored;
            return S_OK;
        }
        m_Stored.push_back(stored);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(
            DWORD direction,
            IEnumFORMATETC** enumerator) override
    {
        if (enumerator == nullptr) return E_POINTER;
        *enumerator = nullptr;
        if (direction != DATADIR_GET) return E_NOTIMPL;
        std::vector<FORMATETC> formats = {
            {m_FileDescriptor, nullptr,
             DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
            {m_FileContents, nullptr,
             DVASPECT_CONTENT, -1, TYMED_ISTREAM},
            {m_PreferredDropEffect, nullptr,
             DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
        };
        {
            std::lock_guard<std::mutex> lock(m_StateMutex);
            for (const StoredMedium& value : m_Stored) {
                formats.push_back(value.format);
            }
        }
        return SHCreateStdEnumFmtEtc(
                static_cast<UINT>(formats.size()), formats.data(), enumerator);
    }

    HRESULT STDMETHODCALLTYPE DAdvise(
            FORMATETC*, DWORD, IAdviseSink*, DWORD*) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumDAdvise(
            IEnumSTATDATA** enumerator) override
    {
        if (enumerator != nullptr) *enumerator = nullptr;
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL enabled) override
    {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        m_AsyncMode = enabled != FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* enabled) override
    {
        if (enabled == nullptr) return E_POINTER;
        std::lock_guard<std::mutex> lock(m_StateMutex);
        *enabled = m_AsyncMode ? VARIANT_TRUE : VARIANT_FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx*) override
    {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        m_AsyncStarted = true;
        m_InAsyncOperation = true;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE InOperation(BOOL* inOperation) override
    {
        if (inOperation == nullptr) return E_POINTER;
        std::lock_guard<std::mutex> lock(m_StateMutex);
        *inOperation = m_InAsyncOperation ? VARIANT_TRUE : VARIANT_FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EndOperation(
            HRESULT result,
            IBindCtx*,
            DWORD effects) override
    {
        std::optional<Completion> completion;
        {
            std::lock_guard<std::mutex> lock(m_StateMutex);
            m_InAsyncOperation = false;
            m_AsyncEnded = true;
            m_AsyncResult = result;
            m_AsyncEffects = effects;
            completion = takeCompletionLocked();
        }
        deliver(std::move(completion));
        return S_OK;
    }

    void finishDragLoop(HRESULT result, DWORD effect)
    {
        std::optional<Completion> completion;
        {
            std::lock_guard<std::mutex> lock(m_StateMutex);
            m_DragLoopFinished = true;
            m_DragResult = result;
            m_DragEffect = effect;
            completion = takeCompletionLocked();
        }
        deliver(std::move(completion));
    }

private:
    struct Completion
    {
        AppleWindowsRemoteFileDragResult result =
                AppleWindowsRemoteFileDragResult::Failed;
        QString error;
    };

    struct StoredMedium
    {
        FORMATETC format{};
        STGMEDIUM medium{};
    };

    bool materialize()
    {
        std::call_once(m_MaterializeOnce, [this]() {
            QStringList paths;
            QString error;
            bool succeeded = m_Materialize && m_Cancelled != nullptr &&
                    m_Materialize(*m_Cancelled, &paths, &error) &&
                    !paths.isEmpty();
            if (succeeded && paths.size() != m_SourcePaths.size()) {
                succeeded = false;
                error = QStringLiteral(
                        "The Mac returned a different number of promised files than it advertised.");
            }
            if (succeeded) {
                for (const QString& path : std::as_const(paths)) {
                    const QFileInfo info(path);
                    if (!info.exists() && !info.isSymLink()) {
                        succeeded = false;
                        error = QStringLiteral(
                                "A promised file was not materialized: %1")
                                        .arg(QDir::toNativeSeparators(path));
                        break;
                    }
                }
            }
            if (!succeeded && error.isEmpty()) {
                error = QStringLiteral(
                        "The Mac did not provide any files for the drop.");
            }
            m_Paths = std::move(paths);
            m_Materialized = succeeded;
            if (!error.isEmpty()) setDataError(error);
        });
        return m_Materialized;
    }

    void setDataError(const QString& error)
    {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        if (m_Error.isEmpty()) m_Error = error;
    }

    std::optional<Completion> takeCompletionLocked()
    {
        if (m_CompletionDelivered || !m_DragLoopFinished ||
                (m_AsyncStarted && !m_AsyncEnded)) {
            return std::nullopt;
        }
        m_CompletionDelivered = true;
        if (!m_Error.isEmpty()) {
            return Completion{AppleWindowsRemoteFileDragResult::Failed, m_Error};
        }
        if (m_DragResult == DRAGDROP_S_CANCEL) {
            return Completion{AppleWindowsRemoteFileDragResult::Cancelled, {}};
        }
        if (m_DragResult != DRAGDROP_S_DROP) {
            return Completion{AppleWindowsRemoteFileDragResult::Failed,
                              hresultText(m_DragResult)};
        }
        if (m_AsyncEnded && FAILED(m_AsyncResult)) {
            return Completion{AppleWindowsRemoteFileDragResult::Failed,
                              hresultText(m_AsyncResult)};
        }
        const DWORD effects = m_DragEffect | m_ReportedDropEffect |
                m_AsyncEffects;
        if ((effects & (DROPEFFECT_COPY | DROPEFFECT_MOVE)) != 0) {
            return Completion{AppleWindowsRemoteFileDragResult::Dropped, {}};
        }
        return Completion{
            AppleWindowsRemoteFileDragResult::Failed,
            QStringLiteral(
                    "Windows Explorer did not accept the promised file at the selected destination.")};
    }

    void deliver(std::optional<Completion> completion)
    {
        if (!completion.has_value() || !m_Finished) return;
        m_Finished(completion->result, completion->error);
    }

    std::atomic<ULONG> m_References{1};
    QStringList m_SourcePaths;
    AppleWindowsRemoteFileDragSource::Materialize m_Materialize;
    std::shared_ptr<std::atomic_bool> m_Cancelled;
    AppleWindowsRemoteFileDragSource::Finished m_Finished;
    QStringList m_Paths;
    std::vector<PromisedFileEntry> m_Entries;
    QString m_Error;
    CLIPFORMAT m_FileDescriptor = 0;
    CLIPFORMAT m_FileContents = 0;
    CLIPFORMAT m_PreferredDropEffect = 0;
    CLIPFORMAT m_PerformedDropEffect = 0;
    CLIPFORMAT m_LogicalPerformedDropEffect = 0;
    mutable std::mutex m_StateMutex;
    DWORD m_ReportedDropEffect = DROPEFFECT_NONE;
    std::once_flag m_MaterializeOnce;
    bool m_Materialized = false;
    bool m_AsyncMode = false;
    bool m_AsyncStarted = false;
    bool m_InAsyncOperation = false;
    bool m_AsyncEnded = false;
    HRESULT m_AsyncResult = S_OK;
    DWORD m_AsyncEffects = DROPEFFECT_NONE;
    bool m_DragLoopFinished = false;
    HRESULT m_DragResult = E_UNEXPECTED;
    DWORD m_DragEffect = DROPEFFECT_NONE;
    bool m_CompletionDelivered = false;
    std::vector<StoredMedium> m_Stored;
};

class RemoteDropSource final : public IDropSource
{
public:
    explicit RemoteDropSource(std::shared_ptr<std::atomic_bool> cancelled)
        : m_Cancelled(std::move(cancelled))
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID interfaceId,
            void** object) override
    {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (interfaceId == IID_IUnknown || interfaceId == IID_IDropSource) {
            *object = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_References; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG references = --m_References;
        if (references == 0) delete this;
        return references;
    }

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(
            BOOL escapePressed,
            DWORD keyState) override
    {
        if (escapePressed ||
                (m_Cancelled != nullptr && m_Cancelled->load())) {
            return DRAGDROP_S_CANCEL;
        }
        return (keyState & MK_LBUTTON) == 0
                ? DRAGDROP_S_DROP : S_OK;
    }

    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override
    {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    std::atomic<ULONG> m_References{1};
    std::shared_ptr<std::atomic_bool> m_Cancelled;
};

} // namespace

#ifdef APPLE_FILE_DRAG_TESTS
bool testAppleWindowsPromisedFileDataObject(
        const QString& promisedPath,
        const QString& materializedPath,
        QString* descriptorName,
        QByteArray* contents,
        QString* error)
{
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    auto* dataObject = new RemoteDataObject(
            {promisedPath},
            [materializedPath](const std::atomic_bool&,
                               QStringList* paths,
                               QString*) {
                *paths = {materializedPath};
                return true;
            },
            cancelled);

    FORMATETC descriptorFormat{
        static_cast<CLIPFORMAT>(
                RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW)),
        nullptr,
        DVASPECT_CONTENT,
        -1,
        TYMED_HGLOBAL,
    };
    STGMEDIUM descriptorMedium{};
    HRESULT result = dataObject->GetData(
            &descriptorFormat, &descriptorMedium);
    if (FAILED(result)) {
        setError(error, hresultText(result));
        dataObject->Release();
        return false;
    }

    auto* descriptors = static_cast<FILEGROUPDESCRIPTORW*>(
            GlobalLock(descriptorMedium.hGlobal));
    if (descriptors == nullptr || descriptors->cItems != 1) {
        if (descriptors != nullptr) GlobalUnlock(descriptorMedium.hGlobal);
        ReleaseStgMedium(&descriptorMedium);
        dataObject->Release();
        setError(error, QStringLiteral("The promised-file descriptor was invalid."));
        return false;
    }
    if (descriptorName != nullptr) {
        *descriptorName = QString::fromWCharArray(
                descriptors->fgd[0].cFileName);
    }
    GlobalUnlock(descriptorMedium.hGlobal);
    ReleaseStgMedium(&descriptorMedium);

    FORMATETC contentsFormat{
        static_cast<CLIPFORMAT>(
                RegisterClipboardFormatW(CFSTR_FILECONTENTS)),
        nullptr,
        DVASPECT_CONTENT,
        0,
        TYMED_ISTREAM,
    };
    STGMEDIUM contentsMedium{};
    result = dataObject->GetData(&contentsFormat, &contentsMedium);
    if (FAILED(result) || contentsMedium.pstm == nullptr) {
        setError(error, hresultText(result));
        if (SUCCEEDED(result)) ReleaseStgMedium(&contentsMedium);
        dataObject->Release();
        return false;
    }

    QByteArray payload;
    char buffer[4096];
    while (true) {
        ULONG read = 0;
        result = contentsMedium.pstm->Read(buffer, sizeof(buffer), &read);
        if (FAILED(result)) break;
        if (read > 0) payload.append(buffer, static_cast<qsizetype>(read));
        if (result == S_FALSE || read == 0) break;
    }
    ReleaseStgMedium(&contentsMedium);
    dataObject->Release();
    if (FAILED(result)) {
        setError(error, hresultText(result));
        return false;
    }
    if (contents != nullptr) *contents = payload;
    return true;
}

bool testAppleWindowsPromisedFileMetadataIsLazy(QString* error)
{
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    int materializeCalls = 0;
    auto* dataObject = new RemoteDataObject(
            {QStringLiteral("/Users/test/Lazy.pkg")},
            [&materializeCalls](const std::atomic_bool&,
                                QStringList*,
                                QString*) {
                ++materializeCalls;
                return false;
            },
            cancelled);

    IDataObjectAsyncCapability* async = nullptr;
    HRESULT asyncResult = dataObject->QueryInterface(
            IID_IDataObjectAsyncCapability,
            reinterpret_cast<void**>(&async));
    BOOL asyncEnabled = VARIANT_FALSE;
    if (SUCCEEDED(asyncResult) && async != nullptr) {
        asyncResult = async->SetAsyncMode(VARIANT_TRUE);
        if (SUCCEEDED(asyncResult)) {
            asyncResult = async->GetAsyncMode(&asyncEnabled);
        }
        async->Release();
    }

    FORMATETC descriptorFormat{
        static_cast<CLIPFORMAT>(
                RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW)),
        nullptr,
        DVASPECT_CONTENT,
        -1,
        TYMED_HGLOBAL,
    };
    STGMEDIUM descriptorMedium{};
    const HRESULT result = dataObject->GetData(
            &descriptorFormat, &descriptorMedium);
    if (SUCCEEDED(result)) ReleaseStgMedium(&descriptorMedium);
    dataObject->Release();
    if (FAILED(result)) {
        setError(error, hresultText(result));
        return false;
    }
    if (FAILED(asyncResult) || asyncEnabled != VARIANT_TRUE) {
        setError(error, QStringLiteral(
                "The promised-file data object does not support asynchronous extraction."));
        return false;
    }
    if (materializeCalls != 0) {
        setError(error, QStringLiteral(
                "Reading promised-file metadata started the remote transfer."));
        return false;
    }
    return true;
}

bool testAppleWindowsPromisedFileAsyncCompletionIsReusable(QString* error)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto cancelled = std::make_shared<std::atomic_bool>(false);
        int completionCount = 0;
        AppleWindowsRemoteFileDragResult completionResult =
                AppleWindowsRemoteFileDragResult::Failed;
        auto* dataObject = new RemoteDataObject(
                {QStringLiteral("/Users/test/Repeated.pkg")},
                [](const std::atomic_bool&, QStringList*, QString*) {
                    return false;
                },
                cancelled,
                [&completionCount, &completionResult](
                        AppleWindowsRemoteFileDragResult result,
                        const QString&) {
                    ++completionCount;
                    completionResult = result;
                });
        dataObject->SetAsyncMode(VARIANT_TRUE);
        dataObject->StartOperation(nullptr);
        dataObject->finishDragLoop(DRAGDROP_S_DROP, DROPEFFECT_COPY);
        if (completionCount != 0) {
            dataObject->Release();
            setError(error, QStringLiteral(
                    "The promised-file source completed before asynchronous extraction ended."));
            return false;
        }
        dataObject->EndOperation(S_OK, nullptr, DROPEFFECT_COPY);
        dataObject->Release();
        if (completionCount != 1 ||
                completionResult !=
                        AppleWindowsRemoteFileDragResult::Dropped) {
            setError(error, QStringLiteral(
                    "A repeated asynchronous promised-file operation did not finish exactly once."));
            return false;
        }
    }
    return true;
}

bool testAppleWindowsPromisedFilesDropIntoShellFolders(
        const QString& firstPromisedPath,
        const QString& firstMaterializedPath,
        const QString& firstDestination,
        const QString& secondPromisedPath,
        const QString& secondMaterializedPath,
        const QString& secondDestination,
        QString* error)
{
    const HRESULT initialized = OleInitialize(nullptr);
    if (FAILED(initialized)) {
        setError(error, hresultText(initialized));
        return false;
    }
    const auto drop = [error](const QString& promisedPath,
                              const QString& materializedPath,
                              const QString& destination) {
        PIDLIST_ABSOLUTE itemId = nullptr;
        const QString nativeDestination = QDir::toNativeSeparators(destination);
        HRESULT result = SHParseDisplayName(
                reinterpret_cast<LPCWSTR>(nativeDestination.utf16()),
                nullptr,
                &itemId,
                0,
                nullptr);
        if (FAILED(result) || itemId == nullptr) {
            setError(error, hresultText(result));
            return false;
        }

        IShellFolder* parent = nullptr;
        PCUITEMID_CHILD child = nullptr;
        result = SHBindToParent(
                itemId, IID_PPV_ARGS(&parent), &child);
        if (FAILED(result) || parent == nullptr || child == nullptr) {
            CoTaskMemFree(itemId);
            setError(error, hresultText(result));
            return false;
        }

        IDropTarget* target = nullptr;
        result = parent->GetUIObjectOf(
                GetDesktopWindow(),
                1,
                &child,
                IID_IDropTarget,
                nullptr,
                reinterpret_cast<void**>(&target));
        parent->Release();
        CoTaskMemFree(itemId);
        if (FAILED(result) || target == nullptr) {
            setError(error, hresultText(result));
            return false;
        }

        auto cancelled = std::make_shared<std::atomic_bool>(false);
        auto* dataObject = new RemoteDataObject(
                {promisedPath},
                [materializedPath](const std::atomic_bool&,
                                   QStringList* paths,
                                   QString*) {
                    *paths = {materializedPath};
                    return true;
                },
                cancelled);
        DWORD effect = DROPEFFECT_COPY;
        POINTL point{};
        result = target->DragEnter(dataObject, MK_LBUTTON, point, &effect);
        if (SUCCEEDED(result) && (effect & DROPEFFECT_COPY) != 0) {
            effect = DROPEFFECT_COPY;
            result = target->Drop(dataObject, 0, point, &effect);
        }
        else {
            target->DragLeave();
        }
        dataObject->Release();
        target->Release();
        if (FAILED(result) || (effect & DROPEFFECT_COPY) == 0) {
            setError(error, FAILED(result) ? hresultText(result)
                                          : QStringLiteral(
                                                    "Explorer rejected the promised file."));
            return false;
        }

        const QString expectedPath = QDir(destination).filePath(
                remoteBaseName(promisedPath));
        for (int attempt = 0; attempt < 200; ++attempt) {
            if (QFileInfo::exists(expectedPath)) return true;
            Sleep(10);
        }
        setError(error, QStringLiteral(
                "Explorer accepted the drop but did not create %1.")
                .arg(QDir::toNativeSeparators(expectedPath)));
        return false;
    };

    const bool succeeded =
            drop(firstPromisedPath, firstMaterializedPath, firstDestination) &&
            drop(secondPromisedPath, secondMaterializedPath, secondDestination);
    OleUninitialize();
    return succeeded;
}
#endif

class AppleWindowsFileDropTarget::Impl
{
public:
    class Target final : public IDropTarget
    {
    public:
        Target(HWND window,
               int displayIndex,
               std::shared_ptr<AppleLocalFileDragLifecycle> lifecycle)
            : m_Window(window),
              m_DisplayIndex(displayIndex),
              m_Lifecycle(std::move(lifecycle))
        {
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(
                REFIID interfaceId,
                void** object) override
        {
            if (object == nullptr) return E_POINTER;
            *object = nullptr;
            if (interfaceId == IID_IUnknown || interfaceId == IID_IDropTarget) {
                *object = static_cast<IDropTarget*>(this);
                AddRef();
                return S_OK;
            }
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() override
        {
            return ++m_References;
        }

        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG references = --m_References;
            if (references == 0) delete this;
            return references;
        }

        HRESULT STDMETHODCALLTYPE DragEnter(
                IDataObject* dataObject,
                DWORD,
                POINTL point,
                DWORD* effect) override
        {
            IUnknown* identity = canonicalIdentity(dataObject);
            const QStringList paths = filePaths(dataObject);
            const AppleFileDragPoint local = localPoint(point);
            const bool accepted = identity != nullptr && !paths.isEmpty() &&
                    m_Lifecycle != nullptr && m_Lifecycle->enter(
                            reinterpret_cast<quintptr>(identity),
                            paths,
                            local);
            synchronizeIdentity(identity);
            setEffect(effect, accepted);
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE DragOver(
                DWORD,
                POINTL point,
                DWORD* effect) override
        {
            const bool accepted = m_Lifecycle != nullptr &&
                    m_Lifecycle->move(localPoint(point));
            setEffect(effect, accepted);
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE DragLeave() override
        {
            if (m_Lifecycle != nullptr) m_Lifecycle->leave();
            scheduleReleasePoll();
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE Drop(
                IDataObject* dataObject,
                DWORD,
                POINTL point,
                DWORD* effect) override
        {
            IUnknown* identity = canonicalIdentity(dataObject);
            if (m_Lifecycle == nullptr || !m_Lifecycle->isActive() ||
                    identity != m_DragIdentity) {
                const QStringList paths = filePaths(dataObject);
                if (m_Lifecycle != nullptr && identity != nullptr &&
                        !paths.isEmpty()) {
                    m_Lifecycle->enter(
                            reinterpret_cast<quintptr>(identity),
                            paths,
                            localPoint(point));
                }
            }
            synchronizeIdentity(identity);
            const bool accepted = m_Lifecycle != nullptr &&
                    m_Lifecycle->drop(localPoint(point));
            if (m_Lifecycle == nullptr || !m_Lifecycle->isActive()) {
                clearIdentity();
            }
            setEffect(effect, accepted);
            return S_OK;
        }

        void cancel()
        {
            if (m_Lifecycle != nullptr) m_Lifecycle->cancel();
            clearIdentity();
        }

    private:
        static IUnknown* canonicalIdentity(IDataObject* dataObject)
        {
            if (dataObject == nullptr) return nullptr;
            IUnknown* identity = nullptr;
            return SUCCEEDED(dataObject->QueryInterface(
                    IID_IUnknown,
                    reinterpret_cast<void**>(&identity)))
                    ? identity : nullptr;
        }

        void synchronizeIdentity(IUnknown* identity)
        {
            if (m_Lifecycle == nullptr || !m_Lifecycle->isActive()) {
                if (identity != nullptr) identity->Release();
                clearIdentity();
                return;
            }
            if (identity == m_DragIdentity) {
                if (identity != nullptr) identity->Release();
                return;
            }
            clearIdentity();
            m_DragIdentity = identity;
        }

        void clearIdentity()
        {
            if (m_DragIdentity == nullptr) return;
            m_DragIdentity->Release();
            m_DragIdentity = nullptr;
        }

        void scheduleReleasePoll()
        {
            if (m_ReleasePollScheduled || m_Lifecycle == nullptr ||
                    !m_Lifecycle->isActive()) return;
            m_ReleasePollScheduled = true;
            AddRef();
            QTimer::singleShot(16, [this]() {
                m_ReleasePollScheduled = false;
                if (m_Lifecycle != nullptr && m_Lifecycle->isActive() &&
                        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
                    m_Lifecycle->cancel();
                    clearIdentity();
                }
                else if (m_Lifecycle != nullptr && m_Lifecycle->isActive()) {
                    scheduleReleasePoll();
                }
                else {
                    clearIdentity();
                }
                Release();
            });
        }

        AppleFileDragPoint localPoint(POINTL screenPoint) const
        {
            POINT point{
                static_cast<LONG>(screenPoint.x),
                static_cast<LONG>(screenPoint.y),
            };
            ScreenToClient(m_Window, &point);
            return {point.x, point.y, m_DisplayIndex};
        }

        static void setEffect(DWORD* effect, bool accepted)
        {
            if (effect == nullptr) return;
            *effect = accepted && (*effect & DROPEFFECT_COPY) != 0
                    ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        }

        std::atomic<ULONG> m_References{1};
        HWND m_Window = nullptr;
        int m_DisplayIndex = 0;
        std::shared_ptr<AppleLocalFileDragLifecycle> m_Lifecycle;
        IUnknown* m_DragIdentity = nullptr;
        bool m_ReleasePollScheduled = false;
    };

    Impl(void* nativeWindow,
         int displayIndex,
         std::shared_ptr<AppleLocalFileDragLifecycle> lifecycle)
        : m_Window(static_cast<HWND>(nativeWindow))
    {
        if (m_Window == nullptr || !IsWindow(m_Window)) {
            m_Error = QStringLiteral("The stream window handle is invalid.");
            return;
        }
        const HRESULT initialized = OleInitialize(nullptr);
        if (FAILED(initialized)) {
            m_Error = hresultText(initialized);
            return;
        }
        m_UninitializeOle = true;
        m_Target = new Target(
                m_Window,
                displayIndex,
                std::move(lifecycle));
        HRESULT registered = RegisterDragDrop(m_Window, m_Target);
        if (registered == DRAGDROP_E_ALREADYREGISTERED) {
            RevokeDragDrop(m_Window);
            registered = RegisterDragDrop(m_Window, m_Target);
        }
        if (FAILED(registered)) {
            m_Error = hresultText(registered);
            m_Target->Release();
            m_Target = nullptr;
            return;
        }
        m_Registered = true;
        // SDL's WM_DROPFILES fallback reports files only after release. Disable
        // it after OLE registration so one physical drag cannot start twice.
        DragAcceptFiles(m_Window, FALSE);
    }

    ~Impl()
    {
        if (m_Target != nullptr) m_Target->cancel();
        if (m_Registered && IsWindow(m_Window)) {
            RevokeDragDrop(m_Window);
            DragAcceptFiles(m_Window, TRUE);
        }
        if (m_Target != nullptr) m_Target->Release();
        if (m_UninitializeOle) OleUninitialize();
    }

    bool isValid() const { return m_Registered; }
    QString errorString() const { return m_Error; }

private:
    HWND m_Window = nullptr;
    Target* m_Target = nullptr;
    QString m_Error;
    bool m_Registered = false;
    bool m_UninitializeOle = false;
};

AppleWindowsFileDropTarget::AppleWindowsFileDropTarget(
        void* nativeWindow,
        int displayIndex,
        std::shared_ptr<AppleLocalFileDragLifecycle> lifecycle)
    : m_Impl(std::make_unique<Impl>(
              nativeWindow,
              displayIndex,
              std::move(lifecycle)))
{
}

AppleWindowsFileDropTarget::~AppleWindowsFileDropTarget() = default;

bool AppleWindowsFileDropTarget::isValid() const
{
    return m_Impl != nullptr && m_Impl->isValid();
}

QString AppleWindowsFileDropTarget::errorString() const
{
    return m_Impl != nullptr ? m_Impl->errorString() : QString();
}

class AppleWindowsRemoteFileDragSource::Impl
{
public:
    explicit Impl(void* nativeWindow)
        : m_Window(static_cast<HWND>(nativeWindow))
    {
        if (m_Window == nullptr || !IsWindow(m_Window)) {
            m_Error = QStringLiteral("The stream window handle is invalid.");
            return;
        }
        m_StreamWindows.push_back(m_Window);
    }

    void cancel()
    {
        m_Shutdown->store(true);
    }

    bool isValid() const
    {
        return m_Window != nullptr && IsWindow(m_Window);
    }

    bool leftButtonDown() const
    {
        return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    }

    bool pointerInsideWindow() const
    {
        if (!isValid()) return false;
        POINT screenPoint{};
        if (!GetCursorPos(&screenPoint)) return false;
        for (const HWND window : m_StreamWindows) {
            if (!IsWindow(window)) continue;
            POINT point = screenPoint;
            RECT client{};
            if (ScreenToClient(window, &point) &&
                    GetClientRect(window, &client) &&
                    PtInRect(&client, point) != FALSE) {
                return true;
            }
        }
        return false;
    }

    void addStreamWindow(void* nativeWindow)
    {
        const HWND window = static_cast<HWND>(nativeWindow);
        if (window == nullptr || !IsWindow(window) ||
                std::find(m_StreamWindows.cbegin(),
                          m_StreamWindows.cend(),
                          window) != m_StreamWindows.cend()) {
            return;
        }
        m_StreamWindows.push_back(window);
    }

    bool isDragging() const
    {
        return m_Running.load();
    }

    bool begin(
            const AppleRemoteFileDrag& drag,
            AppleWindowsRemoteFileDragSource::Materialize materialize,
            AppleWindowsRemoteFileDragSource::Finished finished,
            QString* error)
    {
        if (!isValid()) {
            setError(error, m_Error);
            return false;
        }
        const DWORD ownerThreadId = GetWindowThreadProcessId(
                m_Window, nullptr);
        if (ownerThreadId == 0 || ownerThreadId != GetCurrentThreadId()) {
            setError(error, QStringLiteral(
                    "The native drag must run on the stream window thread."));
            return false;
        }
        if (!leftButtonDown()) {
            setError(error, QStringLiteral("The drag button is no longer held."));
            return false;
        }
        bool expected = false;
        if (!m_Running.compare_exchange_strong(expected, true)) {
            setError(error, QStringLiteral("A promised-file drag is already active."));
            return false;
        }
        const bool started = run(
                drag.imagePng,
                drag.sourcePaths,
                std::move(materialize),
                std::move(finished),
                error);
        m_Running.store(false);
        return started;
    }

    bool run(
            const QByteArray& imagePng,
            const QStringList& sourcePaths,
            AppleWindowsRemoteFileDragSource::Materialize materialize,
            AppleWindowsRemoteFileDragSource::Finished finished,
            QString* error)
    {
        const HRESULT initialized = OleInitialize(nullptr);
        if (FAILED(initialized)) {
            setError(error, hresultText(initialized));
            return false;
        }

        auto* dataObject = new RemoteDataObject(
                sourcePaths,
                std::move(materialize),
                m_Shutdown,
                std::move(finished));
        // The payload can require a network round trip. Explorer understands
        // IDataObjectAsyncCapability and will extract FILECONTENTS on a worker
        // after IDropTarget::Drop returns, leaving this HWND thread responsive.
        dataObject->SetAsyncMode(VARIANT_TRUE);
        initializeDragImage(
                dataObject, m_Window, imagePng, sourcePaths);
        auto* dropSource = new RemoteDropSource(m_Shutdown);
        DWORD effect = DROPEFFECT_NONE;
        const HRESULT result = DoDragDrop(
                dataObject, dropSource, DROPEFFECT_COPY, &effect);
        dataObject->finishDragLoop(result, effect);
        dataObject->Release();
        dropSource->Release();
        OleUninitialize();
        return true;
    }

    QString errorString() const { return m_Error; }

private:
    HWND m_Window = nullptr;
    std::vector<HWND> m_StreamWindows;
    QString m_Error;
    std::shared_ptr<std::atomic_bool> m_Shutdown =
            std::make_shared<std::atomic_bool>(false);
    std::atomic_bool m_Running{false};
};

AppleWindowsRemoteFileDragSource::AppleWindowsRemoteFileDragSource(
        void* nativeWindow)
    : m_Impl(std::make_shared<Impl>(nativeWindow))
{
}

AppleWindowsRemoteFileDragSource::~AppleWindowsRemoteFileDragSource()
{
    if (m_Impl != nullptr) m_Impl->cancel();
}

bool AppleWindowsRemoteFileDragSource::isValid() const
{
    return m_Impl != nullptr && m_Impl->isValid();
}

bool AppleWindowsRemoteFileDragSource::isDragging() const
{
    return m_Impl != nullptr && m_Impl->isDragging();
}

void AppleWindowsRemoteFileDragSource::addStreamWindow(void* nativeWindow)
{
    if (m_Impl != nullptr) m_Impl->addStreamWindow(nativeWindow);
}

bool AppleWindowsRemoteFileDragSource::leftButtonDown() const
{
    return m_Impl != nullptr && m_Impl->leftButtonDown();
}

bool AppleWindowsRemoteFileDragSource::pointerInsideWindow() const
{
    return m_Impl != nullptr && m_Impl->pointerInsideWindow();
}

bool AppleWindowsRemoteFileDragSource::begin(
        const AppleRemoteFileDrag& drag,
        Materialize materialize,
        Finished finished,
        QString* error)
{
    if (m_Impl == nullptr) {
        setError(error, QStringLiteral("The Windows promised-file source is unavailable."));
        return false;
    }
    const std::shared_ptr<Impl> impl = m_Impl;
    return impl->begin(
            drag, std::move(materialize), std::move(finished), error);
}
