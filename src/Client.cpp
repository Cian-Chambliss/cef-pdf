#include "Client.h"
#include "Common.h"
#include "SchemeHandlerFactory.h"
#include "PrintHandler.h"
#include "RenderHandler.h"
#include "RenderProcessHandler.h"

#include "include/base/cef_logging.h"
#include "include/wrapper/cef_helpers.h"
#include "include/base/cef_bind.h"
#include "include/base/cef_callback.h"
#include "include/cef_values.h"
#include "include/wrapper/cef_closure_task.h"

#include <iostream>
#include <thread>

namespace cefpdf {

Client::Client() :
    m_jobManager(new job::Manager()),
    m_pendingBrowsersCount(0),
    m_browsersCount(0),
    m_initialized(false),
    m_contextInitialized(false),
    m_running(false),
    m_stopAfterLastJob(false),
    m_printHandler(new PrintHandler),
    m_renderHandler(new RenderHandler),
    m_renderProcessHandler(new RenderProcessHandler)
{
    m_settings.no_sandbox = true;
    m_settings.windowless_rendering_enabled = true;
    m_settings.command_line_args_disabled = true;


    m_windowInfo.windowless_rendering_enabled = true;

    m_browserSettings.windowless_frame_rate = 1;
    CefString(&m_browserSettings.default_encoding).FromString(constants::encoding);
    //m_browserSettings.plugins = STATE_DISABLED;
    m_browserSettings.javascript_close_windows = STATE_DISABLED;
    m_delay = 0;
    m_waitForSignal = false;
    m_waitSignalTimeout = 0;
    m_saveHtmlPath.clear();
    m_saveHtmlStaticOnly = false;
}

int Client::ExecuteSubProcess(const CefMainArgs& mainArgs)
{
    return CefExecuteProcess(mainArgs, this, NULL);
}

void Client::Initialize(const CefMainArgs& mainArgs,CefRefPtr<CefCommandLine> commandLine)
{
    DCHECK(!m_initialized);

    m_initialized = true;

    std::string profile;
    if (commandLine->HasSwitch("profile")) {
        profile = commandLine->GetSwitchValue("profile").ToString();
    }
    if( profile.empty() )
    {
        char _cachePath[1024+100];
        GetTempPathA( sizeof(_cachePath)-100, _cachePath );
        char *endOfPath = _cachePath+strlen(_cachePath);
        sprintf( endOfPath , "CEF_PDF" );
        CefString(&m_settings.cache_path).FromASCII(_cachePath);
    }
    else
    {
         CefString(&m_settings.cache_path).FromString(profile);
    }
    CefInitialize(mainArgs, m_settings, this, NULL);
}

void Client::Shutdown()
{
    DCHECK(m_initialized);

    CefShutdown();
    m_contextInitialized = false;
    m_initialized = false;
}

void Client::Run()
{
    DCHECK(m_initialized);
    DCHECK(!m_running);

    m_running = true;
    CefRunMessageLoop();
    m_running = false;
    Shutdown();
}

void Client::Stop()
{
    DCHECK(m_initialized);

    if (m_running) {
        m_pendingBrowsersCount = 0;
        m_jobManager->StopAll();
        CefQuitMessageLoop();
    }
}

void Client::AddJob(CefRefPtr<job::Job> job)
{
    m_jobManager->Queue(job);
    CreateBrowsers(1);
}

void Client::CreateBrowsers(unsigned int browserCount)
{
    m_pendingBrowsersCount += browserCount;

    if (!m_contextInitialized) {
        return;
    }

    while (m_pendingBrowsersCount > 0 && m_browsersCount <= constants::maxProcesses) {
        --m_pendingBrowsersCount;
        ++m_browsersCount;
        CefBrowserHost::CreateBrowser(m_windowInfo, this, "", m_browserSettings, nullptr,nullptr);
    }
}

// CefApp methods:
// -----------------------------------------------------------------------------
CefRefPtr<CefBrowserProcessHandler> Client::GetBrowserProcessHandler()
{
    return this;
}

void Client::OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar)
{
    registrar->AddCustomScheme(constants::scheme, CEF_SCHEME_OPTION_STANDARD);
}

void Client::OnBeforeCommandLineProcessing(const CefString& process_type, CefRefPtr<CefCommandLine> command_line)
{
    DLOG(INFO)
        << "Client::OnBeforeCommandLineProcessing"
        << " with process_type: " << process_type.ToString()
        << ", command_line: " << command_line->GetCommandLineString().ToString();

    //command_line->AppendSwitch("disable-gpu");
    //command_line->AppendSwitch("disable-gpu-compositing");
    command_line->AppendSwitch("disable-extensions");
    command_line->AppendSwitch("disable-pinch");
    command_line->AppendSwitch("do-not-de-elevate");
};

CefRefPtr<CefRenderProcessHandler> Client::GetRenderProcessHandler()
{
    return m_renderProcessHandler;
}

// CefBrowserProcessHandler methods:
// -----------------------------------------------------------------------------
CefRefPtr<CefPrintHandler> Client::GetPrintHandler()
{
    return m_printHandler;
}

void Client::OnBeforeChildProcessLaunch(CefRefPtr<CefCommandLine> command_line)
{
    DLOG(INFO)
        << "Client::OnBeforeChildProcessLaunch"
        << " with command_line: " << command_line->GetCommandLineString().ToString();
}

void Client::OnContextInitialized()
{
    DLOG(INFO) << "Client::OnContextInitialized";

    CEF_REQUIRE_UI_THREAD();

    m_contextInitialized = true;

    CefRegisterSchemeHandlerFactory(constants::scheme, "", new SchemeHandlerFactory(m_jobManager));

    CreateBrowsers();
}

// CefClient methods:
// -----------------------------------------------------------------------------
CefRefPtr<CefLifeSpanHandler> Client::GetLifeSpanHandler()
{
    return this;
}

CefRefPtr<CefDisplayHandler> Client::GetDisplayHandler()
{
    return this;
}

CefRefPtr<CefLoadHandler> Client::GetLoadHandler()
{
    return this;
}

CefRefPtr<CefRenderHandler> Client::GetRenderHandler()
{
    return m_renderHandler;
}

CefRefPtr<CefRequestHandler> Client::GetRequestHandler()
{
    return this;
}

bool Client::OnProcessMessageReceived(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefProcessId source_process,
    CefRefPtr<CefProcessMessage> message
) {
    DLOG(INFO) << "Client::OnProcessMessageReceived";

    CEF_REQUIRE_UI_THREAD();

    if (message->GetName() == constants::waitSignalMessage) {
        if (m_waitForSignal && frame->IsMain()) {
            ProcessOnce(browser);
        }

        return true;
    }

    if (message->GetName() == constants::domHtmlMessage) {
        if (frame->IsMain()) {
            std::string html;
            std::string snapshotError;
            auto args = message->GetArgumentList();
            if (args && args->GetSize() > 0 && args->GetType(0) == VTYPE_STRING) {
                html = args->GetString(0).ToString();
            }
            if (args && args->GetSize() > 1 && args->GetType(1) == VTYPE_STRING) {
                snapshotError = args->GetString(1).ToString();
            }

            std::clog
                << "savehtml: DOM snapshot received"
                << " (bytes=" << html.size() << ")"
                << std::endl;

            if (!snapshotError.empty()) {
                std::cerr << "savehtml: renderer warning: " << snapshotError << std::endl;
            }

            if (!m_saveHtmlPath.empty()) {
                if (m_saveHtmlStaticOnly) {
                    const std::size_t originalSize = html.size();
                    html = stripScriptsFromHtml(html);
                    std::clog
                        << "savehtml: stripped scripts"
                        << " (bytes=" << originalSize
                        << " -> " << html.size() << ")"
                        << std::endl;
                }

                if (!writeTextFile(m_saveHtmlPath, html)) {
                    DLOG(INFO) << "Client::OnProcessMessageReceived - failed to save HTML to " << m_saveHtmlPath;
                    std::cerr << "savehtml: failed to write file: " << m_saveHtmlPath << std::endl;
                } else {
                    std::clog << "savehtml: wrote file: " << m_saveHtmlPath << std::endl;
                }
            }

            if (html.empty()) {
                std::cerr << "savehtml: warning - DOM snapshot is empty" << std::endl;
            }

            m_jobManager->Process(browser, 200);
        }

        return true;
    }

    return true;
}

bool Client::OnConsoleMessage(
    CefRefPtr<CefBrowser> browser,
    cef_log_severity_t level,
    const CefString& message,
    const CefString& source,
    int line
)
{
    const std::string prefix = "js-console";
    const std::string text = message.ToString();
    const std::string file = source.ToString();

    if (level == LOGSEVERITY_ERROR || level == LOGSEVERITY_FATAL) {
        std::cerr << prefix << ": error: " << text << " (" << file << ":" << line << ")" << std::endl;
    } else if (level == LOGSEVERITY_WARNING) {
        std::cerr << prefix << ": warn: " << text << " (" << file << ":" << line << ")" << std::endl;
    } else {
        std::clog << prefix << ": " << text << " (" << file << ":" << line << ")" << std::endl;
    }

    return false;
}


// CefLifeSpanHandler methods:
// -----------------------------------------------------------------------------
void Client::OnAfterCreated(CefRefPtr<CefBrowser> browser)
{
    DLOG(INFO) << "Client::OnAfterCreated";

    CEF_REQUIRE_UI_THREAD();

    // Assign this browser to the next job. JobsManager will
    // check if there is any queued job
    m_jobManager->Assign(browser);
}

bool Client::DoClose(CefRefPtr<CefBrowser> browser)
{
    DLOG(INFO) << "Client::DoClose";

    CEF_REQUIRE_UI_THREAD();

    // Allow the close. For windowed browsers this will result in the OS close
    // event being sent.
    return false;
}

void Client::OnBeforeClose(CefRefPtr<CefBrowser> browser)
{
    DLOG(INFO) << "Client::OnBeforeClose";

    CEF_REQUIRE_UI_THREAD();

    m_signalBrowsers.erase(browser->GetIdentifier());
    m_saveHtmlBrowsers.erase(browser->GetIdentifier());

    --m_browsersCount;

    if (0 == m_browsersCount && m_stopAfterLastJob) {
        CefPostDelayedTask(TID_UI, base::BindOnce(&Client::Stop, this), 50);
    } else {
        CreateBrowsers();
    }
}

// CefLoadHandler methods:
// -----------------------------------------------------------------------------
void Client::OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, TransitionType transition_type)
{
    DLOG(INFO)
        << "Client::OnLoadStart"
        << " with url: " << frame->GetURL().ToString();

    CEF_REQUIRE_UI_THREAD();

    if (m_waitForSignal && frame->IsMain()) {
        m_signalBrowsers.erase(browser->GetIdentifier());
    }

    if (frame->IsMain()) {
        m_saveHtmlBrowsers.erase(browser->GetIdentifier());
    }
}

void Client::Process(CefRefPtr<CefBrowser> browser)
{
    DLOG(INFO) << "Client::Process - generating PDF";

    if (!m_saveHtmlPath.empty()) {
        const int browserId = browser->GetIdentifier();
        if (m_saveHtmlBrowsers.find(browserId) == m_saveHtmlBrowsers.end()) {
            std::clog << "savehtml: requesting DOM snapshot" << std::endl;
            m_saveHtmlBrowsers.insert(browserId);
            CefRefPtr<CefProcessMessage> message =
                CefProcessMessage::Create(constants::requestDomHtmlMessage);
            browser->GetMainFrame()->SendProcessMessage(PID_RENDERER, message);
            return;
        }
    }

    m_jobManager->Process(browser, 200);
}

void Client::ProcessOnce(CefRefPtr<CefBrowser> browser)
{
    int browserId = browser->GetIdentifier();
    if (m_signalBrowsers.find(browserId) != m_signalBrowsers.end()) {
        return;
    }

    m_signalBrowsers.insert(browserId);
    Process(browser);
}

void Client::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode)
{
    DLOG(INFO)
        << "Client::OnLoadEnd"
        << " with url: " << frame->GetURL().ToString()
        << ", httpStatusCode: " << httpStatusCode;

    CEF_REQUIRE_UI_THREAD();

    if (frame->IsMain()) {
        if (httpStatusCode == 200 && m_waitForSignal) {
            DLOG(INFO) << "Client::OnLoadEnd - waiting for JavaScript signal before generating PDF";
            if (m_waitSignalTimeout > 0) {
                DLOG(INFO) << "Client::OnLoadEnd - wait-signal timeout set to " << m_waitSignalTimeout << "ms";
                CefPostDelayedTask(TID_UI, base::BindOnce(&Client::ProcessOnce, this, browser), m_waitSignalTimeout);
            }
            return;
        }

        if (httpStatusCode == 200 && m_delay > 0) {
            DLOG(INFO) << "Client::OnLoadEnd - waiting for " << m_delay << "ms before generating PDF";
            CefPostDelayedTask(TID_UI, base::BindOnce(&Client::Process, this, browser), m_delay);
        }
        else
            m_jobManager->Process(browser, httpStatusCode);
    }
}

void Client::OnLoadError(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    ErrorCode errorCode,
    const CefString& errorText,
    const CefString& failedUrl
) {
    DLOG(INFO)
        << "Client::OnLoadError"
        << " with errorCode: " << errorCode
        << ", failedUrl: " << failedUrl.ToString();

    CEF_REQUIRE_UI_THREAD();

    if (frame->IsMain()) {
        m_jobManager->Abort(browser, errorCode);
    }
}

// CefRequestHandler methods:
// -------------------------------------------------------------------------
bool Client::OnBeforeBrowse(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request,
    bool user_gesture,
    bool is_redirect
) {
    DLOG(INFO) << "Client::OnBeforeBrowse";

    if (m_schemes.empty()) {
        return false;
    }

    std::string url = request->GetURL().ToString();

    for (auto s = m_schemes.cbegin(); s != m_schemes.cend(); ++s) {
        if (matchScheme(url, *s)) {
            return false;
        }
    }

    return true;
}

void Client::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
    TerminationStatus status,
    int error_code,
    const CefString& error_string)
{
    DLOG(INFO) << "Client::OnRenderProcessTerminated";
}

void Client::SetViewWidth(int viewWidth)
{
   m_renderHandler->SetViewWidth(viewWidth);
}

void Client::SetViewHeight(int viewHeight)
{
   m_renderHandler->SetViewHeight(viewHeight);
}

} // namespace cefpdf
