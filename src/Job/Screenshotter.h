#ifndef JOB_SCREENSHOTTER_H_
#define JOB_SCREENSHOTTER_H_

#include "Job.h"

#include "include/cef_browser.h"
#include "include/cef_devtools_message_observer.h"

namespace cefpdf {
namespace job {

class Manager;

class Screenshotter : public CefDevToolsMessageObserver
{
public:
    Screenshotter(CefRefPtr<Manager> manager, CefRefPtr<CefBrowser> browser,
        CefRefPtr<Job> job);

    void Start();

    void OnDevToolsMethodResult(CefRefPtr<CefBrowser> browser, int message_id,
        bool success, const void* result, size_t result_size) override;
    void OnDevToolsAgentDetached(CefRefPtr<CefBrowser> browser) override;

private:
    void RequestLayoutMetrics();
    void RequestBackground(double width, double height);
    void RequestCapture(double width, double height);
    void Complete(bool ok);
    bool SaveResult(const CefString& encoded);
    bool SaveBmp(const void* pngData, size_t pngSize);
    void OnTimeout();
    void Finalize(bool ok);

    CefRefPtr<Manager> m_manager;
    CefRefPtr<CefBrowser> m_browser;
    CefRefPtr<Job> m_job;
    CefRefPtr<CefRegistration> m_registration;
    int m_layoutRequest;
    int m_backgroundRequest;
    int m_captureRequest;
    double m_captureWidth;
    double m_captureHeight;
    bool m_completed;

    IMPLEMENT_REFCOUNTING(Screenshotter);
};

} // namespace job
} // namespace cefpdf

#endif // JOB_SCREENSHOTTER_H_
