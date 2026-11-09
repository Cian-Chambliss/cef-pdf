#include "RenderProcessHandler.h"
#include "Common.h"

#include "include/cef_v8.h"

namespace cefpdf {

namespace {

const char kSignalObjectName[] = "cefpdf";
const char kSignalMethodName[] = "signalReady";

class SignalHandler : public CefV8Handler
{
public:
    explicit SignalHandler(CefRefPtr<CefFrame> frame) : m_frame(frame) {}

    bool Execute(
        const CefString& name,
        CefRefPtr<CefV8Value> object,
        const CefV8ValueList& arguments,
        CefRefPtr<CefV8Value>& retval,
        CefString& exception
    ) override {
        if (!m_frame) {
            return true;
        }

        CefRefPtr<CefProcessMessage> message =
            CefProcessMessage::Create(constants::waitSignalMessage);
        m_frame->SendProcessMessage(PID_BROWSER, message);
        return true;
    }

private:
    CefRefPtr<CefFrame> m_frame;

    IMPLEMENT_REFCOUNTING(SignalHandler);
};

} // namespace

RenderProcessHandler::RenderProcessHandler() {}

// CefRenderProcessHandler methods:
// -----------------------------------------------------------------------------
void RenderProcessHandler::OnContextCreated(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefV8Context> context
) {
    DLOG(INFO) << "RenderProcessHandler::OnContextCreated";
    CEF_REQUIRE_RENDERER_THREAD();

    if (!frame->IsMain()) {
        return;
    }

    CefRefPtr<CefV8Value> global = context->GetGlobal();
    CefRefPtr<CefV8Value> cefpdfObject = global->GetValue(kSignalObjectName);
    if (!cefpdfObject || !cefpdfObject->IsObject()) {
        cefpdfObject = CefV8Value::CreateObject(nullptr, nullptr);
        global->SetValue(kSignalObjectName, cefpdfObject, V8_PROPERTY_ATTRIBUTE_NONE);
    }

    CefRefPtr<CefV8Value> signalFunction =
        CefV8Value::CreateFunction(kSignalMethodName, new SignalHandler(frame));
    cefpdfObject->SetValue(kSignalMethodName, signalFunction, V8_PROPERTY_ATTRIBUTE_NONE);

    //m_messageRouterRendererSide->OnContextCreated(browser, frame, context);
}

void RenderProcessHandler::OnContextReleased(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefV8Context> context
) {
    DLOG(INFO) << "RenderProcessHandler::OnContextReleased";
    CEF_REQUIRE_RENDERER_THREAD();

    //m_messageRouterRendererSide->OnContextReleased(browser, frame, context);
}

bool RenderProcessHandler::OnProcessMessageReceived(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefProcessId source_process,
    CefRefPtr<CefProcessMessage> message
) {
    DLOG(INFO) << "RenderProcessHandler::OnProcessMessageReceived";
    CEF_REQUIRE_RENDERER_THREAD();

    if (message->GetName() == constants::requestMetadataMessage) {
        CefRefPtr<CefFrame> targetFrame = frame;
        if (!targetFrame && browser) {
            targetFrame = browser->GetMainFrame();
        }

        if (!targetFrame || !targetFrame->IsMain()) {
            DLOG(INFO) << "metadata: skipped JavaScript metadata - no main frame";
            return true;
        }

        std::string expression;
        auto requestArgs = message->GetArgumentList();
        if (requestArgs && requestArgs->GetSize() > 0 && requestArgs->GetType(0) == VTYPE_STRING) {
            expression = requestArgs->GetString(0).ToString();
        }

        std::string metadata;
        std::string error;
        if (expression.empty()) {
            error = "metadata expression is empty";
        } else {
            CefRefPtr<CefV8Context> context = targetFrame->GetV8Context();
            if (!context) {
                error = "main frame has no V8 context";
            } else if (context->Enter()) {
                CefRefPtr<CefV8Value> global = context->GetGlobal();
                CefRefPtr<CefV8Value> evalFunction =
                    global ? global->GetValue("eval") : nullptr;
                if (!evalFunction || !evalFunction->IsFunction()) {
                    error = "eval is not available";
                } else {
                    CefV8ValueList evalArgs;
                    evalArgs.push_back(CefV8Value::CreateString(expression));
                    CefRefPtr<CefV8Value> result =
                        evalFunction->ExecuteFunction(global, evalArgs);
                    if (evalFunction->HasException()) {
                        CefRefPtr<CefV8Exception> exception = evalFunction->GetException();
                        error = "metadata expression threw: ";
                        error += exception
                            ? exception->GetMessage().ToString()
                            : std::string("unknown exception");
                        evalFunction->ClearException();
                    } else if (!result) {
                        error = "metadata expression returned no value";
                    } else if (!result->IsString()) {
                        error = "metadata expression must return a string";
                    } else {
                        metadata = result->GetStringValue().ToString();
                    }
                }
                context->Exit();
            } else {
                error = "failed to enter V8 context";
            }
        }

        DLOG(INFO) << "metadata: renderer captured bytes=" << metadata.size();

        CefRefPtr<CefProcessMessage> response =
            CefProcessMessage::Create(constants::metadataMessage);
        response->GetArgumentList()->SetString(0, metadata);
        response->GetArgumentList()->SetString(1, error);
        targetFrame->SendProcessMessage(PID_BROWSER, response);
        return true;
    }

    if (message->GetName() == constants::requestDomHtmlMessage) {
        CefRefPtr<CefFrame> targetFrame = frame;
        if (!targetFrame && browser) {
            targetFrame = browser->GetMainFrame();
        }

        if (!targetFrame || !targetFrame->IsMain()) {
            DLOG(INFO) << "savehtml: skipped DOM snapshot - no main frame";
            return true;
        }

        std::string html;
        std::string error;
        CefRefPtr<CefV8Context> context = targetFrame->GetV8Context();
        if (!context) {
            error = "main frame has no V8 context";
        } else if (context->Enter()) {
            CefRefPtr<CefV8Value> global = context->GetGlobal();
            CefRefPtr<CefV8Value> document = global ? global->GetValue("document") : nullptr;
            CefRefPtr<CefV8Value> documentElement =
                (document && document->IsObject()) ? document->GetValue("documentElement") : nullptr;
            CefRefPtr<CefV8Value> outerHtml =
                (documentElement && documentElement->IsObject()) ? documentElement->GetValue("outerHTML") : nullptr;

            if (outerHtml && outerHtml->IsString()) {
                html = outerHtml->GetStringValue().ToString();
            } else if (!document || !document->IsObject()) {
                error = "document object is not available";
            } else if (!documentElement || !documentElement->IsObject()) {
                error = "document.documentElement is not available";
            } else {
                error = "document.documentElement.outerHTML is not a string";
            }

            context->Exit();
        } else {
            error = "failed to enter V8 context";
        }

        DLOG(INFO) << "savehtml: renderer captured DOM bytes=" << html.size();

        CefRefPtr<CefProcessMessage> response =
            CefProcessMessage::Create(constants::domHtmlMessage);
        response->GetArgumentList()->SetString(0, html);
        response->GetArgumentList()->SetString(1, error);
        targetFrame->SendProcessMessage(PID_BROWSER, response);
        return true;
    }

    //m_messageRouterRendererSide->OnProcessMessageReceived(browser, source_process, message);
    return true;
}

void RenderProcessHandler::OnWebKitInitialized()
{
    DLOG(INFO) << "RenderProcessHandler::OnWebKitInitialized";
    CEF_REQUIRE_RENDERER_THREAD();

    // CefMessageRouterConfig config;
    // config.js_query_function = constants::jsQueryFunction;
    // config.js_cancel_function = constants::jsCancelFunction;

    // m_messageRouterRendererSide = CefMessageRouterRendererSide::Create(config);
}

} // namespace cefpdf

