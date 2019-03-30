#ifndef RUBY_TYPER_LSPLOOP_H
#define RUBY_TYPER_LSPLOOP_H

#include "ast/ast.h"
#include "common/concurrency/WorkerPool.h"
#include "common/kvstore/KeyValueStore.h"
#include "core/ErrorQueue.h"
#include "core/core.h"
#include "main/lsp/LSPMessage.h"
#include "main/lsp/json_types.h"
#include "main/options/options.h"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include <deque>
#include <optional>

//  _     ____  ____
// | |   / ___||  _ _\
// | |   \___ \| |_) |
// | |___ ___) |  __/
// |_____|____/|_|
//
//
// This is an implementation of LSP protocol(version 3.0) for Ruby typer
// So far we only support errors but the intention is to
// continue adding support to features already in LSP:
// - code navigation(jump to definition, find all usages, etc)
// - refactorings(rename classes)
//
// So far, we only handle changes via "textDocument/didChange" request.
// This is the main request that is used by VSCode.
// VI uses "textDocument/didSave" that is very similar and should be easy to support.
namespace sorbet::realmain::lsp {

/** This structure represents a method defined by LSP.
 * It is used as an enum to indicate properties of method in common request handling code.
 */
struct LSPMethod {
    /* What is the name of this method as specified in the protocol */
    const std::string name;
    /* Is this a notification? Otherwise this is a request and it would need a response */
    const bool isNotification;
    /* Who initiates this request */
    enum class Kind {
        ServerInitiated,
        ClientInitiated,
        Both,
    };
    const Kind kind;
    /* Do we support this method? */
    const bool isSupported = true;

    inline bool operator==(const LSPMethod &other) const {
        return other.name == this->name;
    }
    inline bool operator!=(const LSPMethod &other) const {
        return other.name != this->name;
    }
    static const inline LSPMethod CancelRequest() {
        return LSPMethod{"$/cancelRequest", true, LSPMethod::Kind::Both};
    };
    static const inline LSPMethod Initialize() {
        return LSPMethod{"initialize", false, LSPMethod::Kind::ClientInitiated};
    };
    static const inline LSPMethod Initialized() {
        return LSPMethod{"initialized", true, LSPMethod::Kind::ClientInitiated};
    };
    static const inline LSPMethod Shutdown() {
        return LSPMethod{"shutdown", false, LSPMethod::Kind::ClientInitiated};
    };
    static const inline LSPMethod Exit() {
        return LSPMethod{"exit", true, LSPMethod::Kind::ClientInitiated};
    };
    static const inline LSPMethod RegisterCapability() {
        return LSPMethod{"client/registerCapability", false, LSPMethod::Kind::ServerInitiated};
    };
    static const inline LSPMethod UnRegisterCapability() {
        return LSPMethod{"client/unregisterCapability", false, LSPMethod::Kind::ServerInitiated};
    };
    static const inline LSPMethod DidChangeWatchedFiles() {
        return LSPMethod{"workspace/didChangeWatchedFiles", true, LSPMethod::Kind::ClientInitiated};
    };
    static const inline LSPMethod PushDiagnostics() {
        return LSPMethod{"textDocument/publishDiagnostics", true, LSPMethod::Kind::ServerInitiated};
    };
    static const inline LSPMethod TextDocumentDidOpen() {
        return LSPMethod{"textDocument/didOpen", true, LSPMethod::Kind::ClientInitiated};
    };
    static const inline LSPMethod TextDocumentDidChange() {
        return LSPMethod{"textDocument/didChange", true, LSPMethod::Kind::ClientInitiated};
    };
    static const inline LSPMethod TextDocumentDidClose() {
        return LSPMethod{"textDocument/didClose", true, LSPMethod::Kind::ClientInitiated};
    };
    static const inline LSPMethod TextDocumentDocumentSymbol() {
        return LSPMethod{"textDocument/documentSymbol", false, LSPMethod::Kind::ClientInitiated};
    };
    static const inline LSPMethod TextDocumentDefinition() {
        return LSPMethod{"textDocument/definition", false, LSPMethod::Kind::ClientInitiated};
    };
    static const inline LSPMethod TextDocumentHover() {
        return LSPMethod{"textDocument/hover", false, LSPMethod::Kind::ClientInitiated};
    };
    static const inline LSPMethod TextDocumentCompletion() {
        return LSPMethod{"textDocument/completion", false, LSPMethod::Kind::ClientInitiated};
    };
    static const inline LSPMethod TextDocumentRefernces() {
        return LSPMethod{"textDocument/references", false, LSPMethod::Kind::ClientInitiated};
    };
    static const inline LSPMethod TextDocumentSignatureHelp() {
        return LSPMethod{"textDocument/signatureHelp", false, LSPMethod::Kind::ClientInitiated};
    };
    static const inline LSPMethod WorkspaceSymbols() {
        return LSPMethod{"workspace/symbol", false, LSPMethod::Kind::ClientInitiated};
    };
    static const inline LSPMethod WindowShowMessage() {
        return LSPMethod{"window/showMessage", true, LSPMethod::Kind::ServerInitiated};
    };
    static const inline LSPMethod Pause() {
        return LSPMethod{"__PAUSE__", true, LSPMethod::Kind::ClientInitiated};
    };
    static const inline LSPMethod Resume() {
        return LSPMethod{"__RESUME__", true, LSPMethod::Kind::ClientInitiated};
    };
    static const inline LSPMethod SorbetWatchmanFileChange() {
        return LSPMethod{"sorbet/watchmanFileChange", true, LSPMethod::Kind::ClientInitiated};
    };
    static const inline LSPMethod SorbetWatchmanExit() {
        return LSPMethod{"sorbet/watchmanExit", true, LSPMethod::Kind::ClientInitiated};
    };
    static const inline LSPMethod SorbetShowOperation() {
        return LSPMethod{"sorbet/showOperation", true, LSPMethod::Kind::ServerInitiated};
    };
    static const std::vector<LSPMethod> ALL_METHODS;
    static const LSPMethod getByName(std::string_view name);
};

/** List of all LSP Methods that we are aware of */

enum class LSPErrorCodes {
    // Defined by JSON RPC
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602, // todo
    InternalError = -32603,
    ServerErrorStart = -32099,
    ServerErrorEnd = -32000,
    ServerNotInitialized = -32002, // todo: can be found by finalGs = nullptr
    UnknownErrorCode = -32001,

    // Defined by the LSP
    RequestCancelled = -32800,
};

class LSPLoop {
    friend class LSPWrapper;

    /** Used to store the state of LSPLoop's internal request queue.  */
    struct QueueState {
        std::deque<std::unique_ptr<LSPMessage>> pendingRequests;
        bool terminate;
        bool paused;
        int requestCounter;
        int errorCode;
        // Counters collected from worker threads.
        CounterState counters;
    };

    struct ResponseHandler {
        std::function<void(rapidjson::Value &)> onResult;
        std::function<void(rapidjson::Value &)> onError;
    };

    /**
     * Object that uses the RAII pattern to notify the client when a *slow* operation
     * starts and ends. Is used to provide user feedback in the status line of VS Code.
     */
    class ShowOperation final {
    private:
        LSPLoop &loop;
        const std::string operationName;
        const std::string description;

    public:
        ShowOperation(LSPLoop &loop, std::string_view operationName, std::string_view description);
        ~ShowOperation();
    };

    UnorderedMap<std::string, ResponseHandler> awaitingResponse;
    /** LSP loop reuses a single arena for all json allocations. We never free memory used for JSON */
    rapidjson::MemoryPoolAllocator<> alloc;
    /** Trees that have been indexed and can be reused between different runs */
    std::vector<ast::ParsedFile> indexed;
    /** Hashes of global states obtained by resolving every file in isolation. Used for fastpath. */
    std::vector<unsigned int> globalStateHashes;
    /** List of files that have had errors in last run*/
    std::vector<core::FileRef> filesThatHaveErrors;
    /** Root of LSP client workspace */
    std::string rootUri;
    /** File system root of LSP client workspace. May be empty if it is the current working directory. */
    std::string rootPath;

    /** Concrete error queue shared by all global states */
    std::shared_ptr<core::ErrorQueue> errorQueue;
    /**
     * `initialGS` is used for indexing. It accumulates a huge nametable of all global things,
     * and is updated as global things are added/removed/updated. It is never discarded.
     *
     * Typechecking is never run on `initialGS` directly. Instead, LSPLoop clones `initialGS` and runs type checking on
     * the clone. This clone is what LSPLoop returns within a `TypecheckRun`.
     */
    std::unique_ptr<core::GlobalState> initialGS;
    const options::Options &opts;
    std::unique_ptr<KeyValueStore> kvstore; // always null for now.
    std::shared_ptr<spdlog::logger> logger;
    WorkerPool &workers;
    /**
     * Whether or not the active client has support for snippets in CompletionItems.
     * Note: There is a generated ClientCapabilities class, but it is cumbersome to work with as most fields are
     * optional.
     */
    bool clientCompletionItemSnippetSupport = false;
    /** Input stream; used by runLSP to receive LSP messages */
    std::istream &inputStream;
    /** Output stream; used by LSP to output messages */
    std::ostream &outputStream;
    /** If true, LSPLoop will skip configatron during type checking */
    const bool skipConfigatron;
    /** If true, all queries will hit the slow path. */
    const bool disableFastPath;
    /** The set of files currently open in the user's editor. */
    UnorderedSet<std::string> openFiles;
    /** The set of files that have been updated before initialization completes. Will be processed post-initialization.
     */
    UnorderedSet<std::string> deferredWatchmanUpdates;
    /**
     * Set to true once the server is initialized.
     * TODO(jvilk): Use to raise server not initialized errors.
     */
    bool initialized = false;
    /**
     * If true, then LSP will send the client notifications at the start and end of slow operations.
     * We don't want to send these notifications to clients that don't know what to do with them,
     * so this boolean gets set when the client sends the `initialize` request with
     * `params.initializationOptions.supportsOperationNotifications` set to `true`.
     */
    bool enableOperationNotifications = false;
    /**
     * The time, in nanoseconds since the epoch, that LSP last sent metrics to statsd -- if `opts.statsdHost` was
     * specified.
     */
    long lastMetricUpdateTime;
    /** ID of the main thread, which actually processes LSP requests and performs typechecking. */
    std::thread::id mainThreadId;

    /* Send the following document to client */
    void sendRaw(std::string_view json);

    /* Send `data` as payload of notification 'meth' */
    void sendNotification(LSPMethod meth, const JSONBaseType &data);

    void sendNullResponse(const MessageId &id);
    void sendResponse(const MessageId &id, const JSONBaseType &result);
    void sendResponse(const MessageId &id, const std::vector<std::unique_ptr<JSONBaseType>> &result);
    void sendError(const MessageId &id, std::unique_ptr<ResponseError> error);
    void sendError(const MessageId &id, int errorCode, std::string_view errorMsg);

    std::unique_ptr<Location> loc2Location(const core::GlobalState &gs, core::Loc loc);
    void addLocIfExists(const core::GlobalState &gs, std::vector<std::unique_ptr<JSONBaseType>> &locs, core::Loc loc);

    /** Returns true if there is no need to continue processing this document as it is a reply to
     * already registered request*/
    bool handleReplies(const LSPMessage &d);

    core::FileRef updateFile(const std::shared_ptr<core::File> &file);
    /** Invalidate all currently cached trees and re-index them from file system.
     * This runs code that is not considered performance critical and this is expected to be slow */
    void reIndexFromFileSystem();
    struct TypecheckRun {
        std::vector<std::unique_ptr<core::Error>> errors;
        std::vector<core::FileRef> filesTypechecked;
        std::vector<std::unique_ptr<core::lsp::QueryResponse>> responses;
        // The global state, post-typechecking.
        std::unique_ptr<core::GlobalState> gs;
    };
    /** Conservatively rerun entire pipeline without caching any trees */
    TypecheckRun runSlowPath(const std::vector<std::shared_ptr<core::File>> &changedFiles);
    /** Apply conservative heuristics to see if we can run a fast path, if not, bail out and run slowPath */
    TypecheckRun tryFastPath(std::unique_ptr<core::GlobalState> gs,
                             std::vector<std::shared_ptr<core::File>> &changedFiles, bool allFiles = false);

    std::unique_ptr<core::GlobalState> pushDiagnostics(TypecheckRun filesTypechecked);

    std::vector<unsigned int> computeStateHashes(const std::vector<std::shared_ptr<core::File>> &files);
    bool ensureInitialized(LSPMethod forMethod, const LSPMessage &msg,
                           const std::unique_ptr<core::GlobalState> &currentGs);

    core::FileRef uri2FileRef(std::string_view uri);
    std::string fileRef2Uri(const core::GlobalState &gs, core::FileRef);
    std::string remoteName2Local(std::string_view uri);
    std::string localName2Remote(std::string_view uri);
    std::unique_ptr<core::Loc> lspPos2Loc(core::FileRef source, const Position &pos, const core::GlobalState &gs);

    /** Used to implement textDocument/documentSymbol
     * Returns `nullptr` if symbol kind is not supported by LSP
     * */
    std::unique_ptr<SymbolInformation> symbolRef2SymbolInformation(const core::GlobalState &gs, core::SymbolRef);
    TypecheckRun runLSPQuery(std::unique_ptr<core::GlobalState> gs, const core::lsp::Query &q,
                             std::vector<std::shared_ptr<core::File>> &changedFiles, bool allFiles = false);
    std::variant<LSPLoop::TypecheckRun, std::pair<std::unique_ptr<ResponseError>, std::unique_ptr<core::GlobalState>>>
    setupLSPQueryByLoc(std::unique_ptr<core::GlobalState> gs, std::string_view uri, const Position &pos,
                       const LSPMethod &forMethod, bool errorIfFileIsUntyped);
    TypecheckRun setupLSPQueryBySymbol(std::unique_ptr<core::GlobalState> gs, core::SymbolRef symbol,
                                       const LSPMethod &forMethod);
    std::unique_ptr<core::GlobalState> handleTextDocumentHover(std::unique_ptr<core::GlobalState> gs,
                                                               const MessageId &id,
                                                               const TextDocumentPositionParams &params);
    std::unique_ptr<core::GlobalState> handleTextDocumentDocumentSymbol(std::unique_ptr<core::GlobalState> gs,
                                                                        const MessageId &id,
                                                                        const DocumentSymbolParams &d);
    std::unique_ptr<core::GlobalState> handleWorkspaceSymbols(std::unique_ptr<core::GlobalState> gs,
                                                              const MessageId &id, const WorkspaceSymbolParams &params);
    std::unique_ptr<core::GlobalState> handleTextDocumentReferences(std::unique_ptr<core::GlobalState> gs,
                                                                    const MessageId &id, const ReferenceParams &params);
    std::unique_ptr<core::GlobalState> handleTextDocumentDefinition(std::unique_ptr<core::GlobalState> gs,
                                                                    const MessageId &id,
                                                                    const TextDocumentPositionParams &params);
    std::unique_ptr<core::GlobalState> handleTextDocumentCompletion(std::unique_ptr<core::GlobalState> gs,
                                                                    const MessageId &id,
                                                                    const CompletionParams &params);
    std::unique_ptr<CompletionItem> getCompletionItem(const core::GlobalState &gs, core::SymbolRef what,
                                                      core::TypePtr receiverType,
                                                      const std::shared_ptr<core::TypeConstraint> &constraint);
    void findSimilarConstantOrIdent(const core::GlobalState &gs, const core::TypePtr receiverType,
                                    std::vector<std::unique_ptr<CompletionItem>> &items);
    void sendShowMessageNotification(MessageType messageType, std::string_view message);
    std::unique_ptr<core::GlobalState> handleTextSignatureHelp(std::unique_ptr<core::GlobalState> gs,
                                                               const MessageId &id,
                                                               const TextDocumentPositionParams &params);
    /**
     * Merges all pending Watchman updates into a single update, and merges any consecutive textDocumentDidChange events
     * into a single update.
     */
    static void mergeFileChanges(rapidjson::MemoryPoolAllocator<> &alloc,
                                 std::deque<std::unique_ptr<LSPMessage>> &pendingRequests);
    /**
     * Performs pre-processing on the incoming LSP request and appends it to the queue.
     * Merges changes to the same document + Watchman filesystem updates, and processes pause/ignore requests.
     * If `collectThreadCounters` is `true`, it also merges in thread-local counters into the QueueState counters.
     */
    static void enqueueRequest(rapidjson::MemoryPoolAllocator<> &alloc, const std::shared_ptr<spd::logger> &logger,
                               LSPLoop::QueueState &queue, std::unique_ptr<LSPMessage> msg,
                               bool collectThreadCounters = false);

    static std::deque<std::unique_ptr<LSPMessage>>::iterator
    findRequestToBeCancelled(std::deque<std::unique_ptr<LSPMessage>> &pendingRequests,
                             const CancelParams &cancellationRequest);
    static std::deque<std::unique_ptr<LSPMessage>>::iterator
    findFirstPositionAfterLSPInitialization(std::deque<std::unique_ptr<LSPMessage>> &pendingRequests);
    std::unique_ptr<core::GlobalState> processRequestInternal(std::unique_ptr<core::GlobalState> gs,
                                                              const LSPMessage &msg);

    std::unique_ptr<core::GlobalState> handleWatchmanUpdates(std::unique_ptr<core::GlobalState> gs,
                                                             std::vector<std::string> changedFiles);

    /** Returns `true` if 5 minutes have elapsed since LSP last sent counters to statsd. */
    bool shouldSendCountersToStatsd(long long currentTime);
    /** Sends counters to statsd. */
    void sendCountersToStatsd(long long currentTime);

public:
    LSPLoop(std::unique_ptr<core::GlobalState> gs, const options::Options &opts,
            const std::shared_ptr<spd::logger> &logger, WorkerPool &workers, std::istream &input, std::ostream &output,
            bool skipConfigatron = false, bool disableFastPath = false);
    std::unique_ptr<core::GlobalState> runLSP();
    std::unique_ptr<core::GlobalState> processRequest(std::unique_ptr<core::GlobalState> gs, const LSPMessage &msg);
    std::unique_ptr<core::GlobalState> processRequest(std::unique_ptr<core::GlobalState> gs, const std::string &json);
    /**
     * Processes a batch of requests. Performs pre-processing to avoid unnecessary work.
     */
    std::unique_ptr<core::GlobalState> processRequests(std::unique_ptr<core::GlobalState> gs,
                                                       std::vector<std::unique_ptr<LSPMessage>> messages);
};

std::optional<std::string> findDocumentation(std::string_view sourceCode, int beginIndex);
bool hasSimilarName(const core::GlobalState &gs, core::NameRef name, std::string_view pattern);
bool hideSymbol(const core::GlobalState &gs, core::SymbolRef sym);
std::string methodDetail(const core::GlobalState &gs, core::SymbolRef method, core::TypePtr receiver,
                         core::TypePtr retType, std::shared_ptr<core::TypeConstraint> constraint);
core::TypePtr getResultType(const core::GlobalState &gs, core::SymbolRef ofWhat, core::TypePtr receiver,
                            std::shared_ptr<core::TypeConstraint> constr);
SymbolKind symbolRef2SymbolKind(const core::GlobalState &gs, core::SymbolRef);
std::unique_ptr<Range> loc2Range(const core::GlobalState &gs, core::Loc loc);

} // namespace sorbet::realmain::lsp
#endif // RUBY_TYPER_LSPLOOP_H