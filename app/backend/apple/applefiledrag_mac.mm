#include "applefiledrag_mac.h"

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <QByteArray>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>

#include <SDL.h>
#include <SDL_syswm.h>

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace {

NSString* const FileNameKey = @"fileName";
NSString* const SourcePathKey = @"sourcePath";
NSString* const PromiseErrorDomain = @"com.moonlight-stream.AppleFilePromise";

void setError(QString* error, const QString& value)
{
    if (error != nullptr) *error = value;
}

QString remoteBaseName(const QString& sourcePath)
{
    return QString(sourcePath).replace('\\', '/').section('/', -1, -1);
}

QString fileNameWithSuffix(const QString& originalName, int suffix)
{
    const int dot = originalName.lastIndexOf('.');
    if (dot <= 0 || dot == originalName.size() - 1) {
        return QStringLiteral("%1 %2").arg(originalName).arg(suffix);
    }
    return QStringLiteral("%1 %2%3")
            .arg(originalName.left(dot))
            .arg(suffix)
            .arg(originalName.mid(dot));
}

struct PromisedFile
{
    QString sourcePath;
    QString fileName;
};

std::vector<PromisedFile> promisedFiles(const QStringList& sourcePaths)
{
    std::vector<PromisedFile> promises;
    QSet<QString> usedNames;
    for (const QString& sourcePath : sourcePaths) {
        const QString originalName = remoteBaseName(sourcePath);
        if (originalName.isEmpty()) continue;
        QString fileName = originalName;
        for (int suffix = 2;
             usedNames.contains(fileName) && suffix <= 65'535;
             ++suffix) {
            fileName = fileNameWithSuffix(originalName, suffix);
        }
        if (usedNames.contains(fileName)) continue;
        usedNames.insert(fileName);
        promises.push_back({sourcePath, fileName});
    }
    return promises;
}

NSView* nativeViewForSdlWindow(SDL_Window* window)
{
    if (window == nullptr) return nil;
    SDL_SysWMinfo windowInfo = {};
    SDL_VERSION(&windowInfo.version);
    if (!SDL_GetWindowWMInfo(window, &windowInfo) ||
            windowInfo.subsystem != SDL_SYSWM_COCOA ||
            windowInfo.info.cocoa.window == nil) {
        return nil;
    }
    return windowInfo.info.cocoa.window.contentView;
}

UTType* contentTypeForFileName(const QString& fileName)
{
    NSString* name = fileName.toNSString();
    UTType* type = name.pathExtension.length > 0
            ? [UTType typeWithFilenameExtension:name.pathExtension]
            : nil;
    // Match the native Swift viewer. Unknown extensions still need a type
    // conforming to public.data or AppKit rejects the promise provider.
    return type ?: UTTypePlainText;
}

NSError* promiseError(const QString& message)
{
    return [NSError errorWithDomain:PromiseErrorDomain
                               code:1
                           userInfo:@{
                               NSLocalizedDescriptionKey: message.toNSString()
                           }];
}

struct MacDragState
{
    AppleMacRemoteFileDragSource::Materialize materialize;
    AppleMacRemoteFileDragSource::Finished finished;
    std::atomic_bool cancelled{false};
    std::atomic_bool dragging{false};
};

using MacDragStatePtr = std::shared_ptr<MacDragState>;

} // namespace

@interface MoonlightAppleFilePromiseDelegate
        : NSObject <NSDraggingSource, NSFilePromiseProviderDelegate> {
@private
    MacDragStatePtr _state;
    NSOperationQueue* _promiseQueue;
}

- (instancetype)initWithState:(MacDragStatePtr)state;

@end

@implementation MoonlightAppleFilePromiseDelegate

- (instancetype)initWithState:(MacDragStatePtr)state
{
    self = [super init];
    if (self != nil) {
        _state = std::move(state);
        _promiseQueue = [[NSOperationQueue alloc] init];
        _promiseQueue.name = @"Moonlight Apple promised-file transfer";
        _promiseQueue.maxConcurrentOperationCount = 1;
        _promiseQueue.qualityOfService = NSQualityOfServiceUtility;
    }
    return self;
}

- (void)dealloc
{
    _state->cancelled.store(true);
    [_promiseQueue cancelAllOperations];
    [_promiseQueue release];
    _promiseQueue = nil;
    _state.reset();
    [super dealloc];
}

- (NSDragOperation)draggingSession:(NSDraggingSession*)session
        sourceOperationMaskForDraggingContext:(NSDraggingContext)context
{
    Q_UNUSED(session);
    Q_UNUSED(context);
    return NSDragOperationCopy;
}

- (BOOL)ignoreModifierKeysForDraggingSession:(NSDraggingSession*)session
{
    Q_UNUSED(session);
    return YES;
}

- (void)draggingSession:(NSDraggingSession*)session
              endedAtPoint:(NSPoint)screenPoint
                 operation:(NSDragOperation)operation
{
    Q_UNUSED(session);
    Q_UNUSED(screenPoint);
    _state->dragging.store(false);
    if (operation == NSDragOperationNone) {
        _state->cancelled.store(true);
    }
    if (_state->finished) {
        _state->finished(operation == NSDragOperationNone
                                 ? AppleMacRemoteFileDragResult::Cancelled
                                 : AppleMacRemoteFileDragResult::Dropped,
                         {});
    }
}

- (NSString*)filePromiseProvider:(NSFilePromiseProvider*)provider
                 fileNameForType:(NSString*)fileType
{
    Q_UNUSED(fileType);
    NSDictionary* information =
            static_cast<NSDictionary*>(provider.userInfo);
    NSString* fileName = information[FileNameKey];
    return fileName ?: @"Remote File";
}

- (void)filePromiseProvider:(NSFilePromiseProvider*)provider
          writePromiseToURL:(NSURL*)url
          completionHandler:(void (^)(NSError* errorOrNil))completionHandler
{
    NSDictionary* information =
            static_cast<NSDictionary*>(provider.userInfo);
    NSString* sourceValue = information[SourcePathKey];
    if (sourceValue == nil || url == nil || !_state->materialize) {
        completionHandler(promiseError(QStringLiteral(
                "The remote file promise is unavailable.")));
        return;
    }

    const QString sourcePath = QString::fromNSString(sourceValue);
    const QString destinationPath = QString::fromNSString(url.path);
    QString completedPath;
    QString error;
    const bool succeeded = _state->materialize(
            sourcePath,
            destinationPath,
            _state->cancelled,
            &completedPath,
            &error);
    const QString promisedPath = QFileInfo(destinationPath).absoluteFilePath();
    if (succeeded &&
            QFileInfo(completedPath).absoluteFilePath() != promisedPath) {
        error = QStringLiteral(
                "The promised file was written to an unexpected path.");
    }
    if (!succeeded || !error.isEmpty()) {
        if (error.isEmpty()) {
            error = QStringLiteral("The remote file could not be downloaded.");
        }
        qWarning().noquote() << "Apple macOS promised-file transfer:" << error;
        completionHandler(promiseError(error));
        return;
    }
    completionHandler(nil);
}

- (NSOperationQueue*)operationQueueForFilePromiseProvider:
        (NSFilePromiseProvider*)provider
{
    Q_UNUSED(provider);
    return _promiseQueue;
}

@end

namespace {

NSArray<NSDraggingItem*>* draggingItems(
        const std::vector<PromisedFile>& promises,
        const AppleRemoteFileDrag& drag,
        NSPoint location,
        MoonlightAppleFilePromiseDelegate* delegate,
        NSMutableArray<NSFilePromiseProvider*>* providers = nil)
{
    NSMutableArray<NSDraggingItem*>* items = [NSMutableArray array];
    NSImage* providedImage = nil;
    if (!drag.imagePng.isEmpty()) {
        NSData* data = [NSData dataWithBytes:drag.imagePng.constData()
                                     length:drag.imagePng.size()];
        providedImage = [[[NSImage alloc] initWithData:data] autorelease];
    }
    for (size_t index = 0; index < promises.size(); ++index) {
        const PromisedFile& promise = promises[index];
        NSString* fileName = promise.fileName.toNSString();
        UTType* contentType = contentTypeForFileName(promise.fileName);
        NSString* fileType = contentType.identifier;
        NSFilePromiseProvider* provider = [[[NSFilePromiseProvider alloc]
                initWithFileType:fileType
                        delegate:delegate] autorelease];
        provider.userInfo = @{
            FileNameKey: fileName,
            SourcePathKey: promise.sourcePath.toNSString(),
        };
        NSImage* image = providedImage ?:
                [[NSWorkspace sharedWorkspace] iconForContentType:contentType];
        NSDraggingItem* item = [[[NSDraggingItem alloc]
                initWithPasteboardWriter:provider] autorelease];
        const CGFloat offset = static_cast<CGFloat>(index) * 3.0;
        [item setDraggingFrame:NSMakeRect(
                                       location.x - 24.0 + offset,
                                       location.y - 24.0 - offset,
                                       48.0,
                                       48.0)
                        contents:image];
        [items addObject:item];
        if (providers != nil) [providers addObject:provider];
    }
    return items;
}

} // namespace

class AppleMacRemoteFileDragSource::Impl
{
public:
    explicit Impl(SDL_Window* window)
    {
        @autoreleasepool {
            m_View = [nativeViewForSdlWindow(window) retain];
            m_Delegates = [[NSMutableArray alloc] init];
        }
    }

    ~Impl()
    {
        @autoreleasepool {
            if (m_State) m_State->cancelled.store(true);
            [m_Delegates release];
            m_Delegates = nil;
            [m_View release];
            m_View = nil;
        }
    }

    bool isValid() const { return m_View != nil; }

    bool isDragging() const
    {
        return m_State != nullptr && m_State->dragging.load();
    }

    bool leftButtonDown() const
    {
        return ([NSEvent pressedMouseButtons] & 1) != 0;
    }

    bool pointerInsideWindow() const
    {
        if (m_View == nil || m_View.window == nil) return false;
        const NSPoint windowPoint = [m_View.window convertPointFromScreen:
                [NSEvent mouseLocation]];
        const NSPoint viewPoint = [m_View convertPoint:windowPoint
                                              fromView:nil];
        return NSPointInRect(viewPoint, m_View.visibleRect);
    }

    bool begin(const AppleRemoteFileDrag& drag,
               const void* nativeEvent,
               AppleMacRemoteFileDragSource::Materialize materialize,
               AppleMacRemoteFileDragSource::Finished finished,
               QString* error)
    {
        @autoreleasepool {
            NSEvent* event = static_cast<NSEvent*>(
                    const_cast<void*>(nativeEvent));
            if (m_View == nil || event == nil ||
                    event.type != NSEventTypeLeftMouseDragged ||
                    ![NSThread isMainThread]) {
                setError(error, QStringLiteral(
                        "The native macOS drag must start from the active mouse-drag event."));
                return false;
            }
            if (!leftButtonDown()) {
                setError(error, QStringLiteral(
                        "The left mouse button is no longer held."));
                return false;
            }
            if (isDragging()) {
                setError(error, QStringLiteral(
                        "A native macOS file drag is already active."));
                return false;
            }
            const NSPoint location = [m_View convertPoint:event.locationInWindow
                                                  fromView:nil];
            if (NSPointInRect(location, m_View.visibleRect)) {
                setError(error, QStringLiteral(
                        "The remote file is still inside the stream window."));
                return false;
            }
            const std::vector<PromisedFile> promises =
                    promisedFiles(drag.sourcePaths);
            if (promises.empty() || !materialize) {
                setError(error, QStringLiteral(
                        "The remote drag did not contain a usable file promise."));
                return false;
            }

            MacDragStatePtr state = std::make_shared<MacDragState>();
            state->materialize = std::move(materialize);
            state->finished = std::move(finished);
            state->dragging.store(true);
            MoonlightAppleFilePromiseDelegate* delegate =
                    [[MoonlightAppleFilePromiseDelegate alloc]
                            initWithState:state];
            NSArray<NSDraggingItem*>* items = draggingItems(
                    promises, drag, location, delegate);
            NSDraggingSession* session = [m_View
                    beginDraggingSessionWithItems:items
                                            event:event
                                           source:delegate];
            if (session == nil) {
                state->dragging.store(false);
                state->cancelled.store(true);
                [delegate release];
                setError(error, QStringLiteral(
                        "AppKit could not create the promised-file drag session."));
                return false;
            }
            session.animatesToStartingPositionsOnCancelOrFail = YES;
            // NSFilePromiseProvider keeps a weak delegate. Retain each drag's
            // provider delegate until the screen-sharing session ends so a
            // slow Finder copy remains valid after the drag animation ends.
            [m_Delegates addObject:delegate];
            [delegate release];
            m_State = std::move(state);
            return true;
        }
    }

private:
    NSView* m_View = nil;
    NSMutableArray* m_Delegates = nil;
    MacDragStatePtr m_State;
};

AppleMacRemoteFileDragSource::AppleMacRemoteFileDragSource(SDL_Window* window)
    : m_Impl(std::make_shared<Impl>(window))
{
}

AppleMacRemoteFileDragSource::~AppleMacRemoteFileDragSource() = default;

bool AppleMacRemoteFileDragSource::isValid() const
{
    return m_Impl->isValid();
}

bool AppleMacRemoteFileDragSource::isDragging() const
{
    return m_Impl->isDragging();
}

bool AppleMacRemoteFileDragSource::leftButtonDown() const
{
    return m_Impl->leftButtonDown();
}

bool AppleMacRemoteFileDragSource::pointerInsideWindow() const
{
    return m_Impl->pointerInsideWindow();
}

bool AppleMacRemoteFileDragSource::begin(
        const AppleRemoteFileDrag& drag,
        const void* nativeEvent,
        Materialize materialize,
        Finished finished,
        QString* error)
{
    return m_Impl->begin(
            drag,
            nativeEvent,
            std::move(materialize),
            std::move(finished),
            error);
}

#ifdef APPLE_FILE_DRAG_TESTS
bool testAppleMacPromisedFileAdapter(QString* error)
{
    @autoreleasepool {
        AppleRemoteFileDrag drag;
        drag.sourcePaths = {
            QStringLiteral("/Remote/First/Report.txt"),
            QStringLiteral("/Remote/Second/Report.txt"),
        };
        const std::vector<PromisedFile> promises =
                promisedFiles(drag.sourcePaths);
        if (promises.size() != 2 ||
                promises[0].fileName != QStringLiteral("Report.txt") ||
                promises[1].fileName != QStringLiteral("Report 2.txt")) {
            setError(error, QStringLiteral(
                    "duplicate remote names were not made unique"));
            return false;
        }

        QTemporaryDir temporary;
        if (!temporary.isValid()) {
            setError(error, QStringLiteral(
                    "the test destination could not be created"));
            return false;
        }
        const QString expectedDestination = temporary.filePath(
                promises[1].fileName);
        QString receivedSource;
        QString receivedDestination;
        MacDragStatePtr state = std::make_shared<MacDragState>();
        state->materialize = [&](const QString& sourcePath,
                                 const QString& destinationPath,
                                 const std::atomic_bool&,
                                 QString* completedPath,
                                 QString* materializeError) {
            receivedSource = sourcePath;
            receivedDestination = destinationPath;
            QFile output(destinationPath);
            if (!output.open(QIODevice::WriteOnly) ||
                    output.write("promised contents") != 17) {
                setError(materializeError, QStringLiteral(
                        "the promised destination could not be written"));
                return false;
            }
            output.close();
            *completedPath = destinationPath;
            return true;
        };
        MoonlightAppleFilePromiseDelegate* delegate =
                [[MoonlightAppleFilePromiseDelegate alloc]
                        initWithState:state];
        NSMutableArray<NSFilePromiseProvider*>* providers =
                [NSMutableArray array];
        draggingItems(promises, drag, NSZeroPoint, delegate, providers);
        if (providers.count != 2 ||
                ![[delegate filePromiseProvider:providers[1]
                                  fileNameForType:providers[1].fileType]
                        isEqualToString:@"Report 2.txt"]) {
            [delegate release];
            setError(error, QStringLiteral(
                    "Finder was not offered the unique promised name"));
            return false;
        }

        __block NSError* completionError = nil;
        [delegate filePromiseProvider:providers[1]
                    writePromiseToURL:[NSURL fileURLWithPath:
                                              expectedDestination.toNSString()]
                    completionHandler:^(NSError* value) {
                        completionError = [value retain];
                    }];
        const QByteArray contents = [&]() {
            QFile input(expectedDestination);
            return input.open(QIODevice::ReadOnly)
                    ? input.readAll() : QByteArray();
        }();
        const bool succeeded = completionError == nil &&
                receivedSource == promises[1].sourcePath &&
                receivedDestination == expectedDestination &&
                contents == QByteArrayLiteral("promised contents");
        if (!succeeded) {
            setError(error, completionError != nil
                    ? QString::fromNSString(completionError.localizedDescription)
                    : QStringLiteral(
                              "the provider did not materialize Finder's exact target URL"));
        }
        [completionError release];
        [delegate release];
        return succeeded;
    }
}
#endif
