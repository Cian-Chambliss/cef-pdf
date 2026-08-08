#ifndef JOB_JOB_H_
#define JOB_JOB_H_

#include "../Common.h"
#include "Visitor.h"

#include "include/cef_base.h"

#include <string>
#include <functional>
#include <cstdint>

namespace cefpdf {
namespace job {

class Job : public CefBaseRefCounted
{

public:
    typedef std::function<void(CefRefPtr<Job>)> Callback;

    enum struct Status {
        PENDING,
        LOADING,
        RENDERING,
        SUCCESS,
        HTTP_ERROR,
        ABORTED,
        LOAD_ERROR,
        OUTPUT_ERROR
    };

    enum struct OutputFormat { PDF, PNG, JPEG, BMP };
    enum struct CaptureMode { FULL, VIEWPORT };

    struct ImageBackground {
        uint8_t red, green, blue, alpha;
    };

    Job();

    void SetCallback(Callback callback) {
        m_callback = callback;
    }

    void ExecuteCallback() {
        if (m_callback != nullptr) {
            m_callback(this);
        }
    }

    virtual void accept(CefRefPtr<Visitor> visitor) = 0;

    const CefString& GetOutputPath() const {
        return m_outputPath;
    }

    void SetOutputPath(const CefString& outputPath) {
        m_outputPath = outputPath;
    }

    OutputFormat GetOutputFormat() const { return m_outputFormat; }
    void SetOutputFormat(OutputFormat format) { m_outputFormat = format; }
    CaptureMode GetCaptureMode() const { return m_captureMode; }
    void SetCaptureMode(CaptureMode mode) { m_captureMode = mode; }
    int GetViewWidth() const { return m_viewWidth; }
    int GetViewHeight() const { return m_viewHeight; }
    void SetViewWidth(int width) { m_viewWidth = width; }
    void SetViewHeight(int height) { m_viewHeight = height; }
    int GetImageQuality() const { return m_imageQuality; }
    void SetImageQuality(int quality) { m_imageQuality = quality; }
    ImageBackground GetImageBackground() const { return m_imageBackground; }
    void SetImageBackground(ImageBackground color) { m_imageBackground = color; }
    const CefString& GetInputMediaType() const { return m_inputMediaType; }
    void SetInputMediaType(const CefString& type) { m_inputMediaType = type; }

    int GetDelay() const { return m_delay; }
    void SetDelay(int delay) { m_delay = delay; }
    bool GetWaitForSignal() const { return m_waitForSignal; }
    void SetWaitForSignal(bool flag) { m_waitForSignal = flag; }
    int GetWaitSignalTimeout() const { return m_waitSignalTimeout; }
    void SetWaitSignalTimeout(int timeout) { m_waitSignalTimeout = timeout; }
    const std::string& GetSaveHtmlPath() const { return m_saveHtmlPath; }
    void SetSaveHtmlPath(const std::string& path) { m_saveHtmlPath = path; }
    bool GetSaveHtmlStaticOnly() const { return m_saveHtmlStaticOnly; }
    void SetSaveHtmlStaticOnly(bool flag) { m_saveHtmlStaticOnly = flag; }

    void SetPageSize(const CefString& pageSize);

    void SetLandscape(bool flag = true);

    void SetPageMargin(const CefString& pageMargin);

    void SetBackgrounds(bool flag = true);

    void SetScale(int scale);

    void SetHeaderFooterEnabled(bool flag = true)
    {
        m_headerFooterEnabled = flag;
    }
    
    void SetHeaderFooterTitle(const CefString& title) 
    {
        m_headerFooterEnabled = true;
        m_headerFooterTitle = title;
    }

    void SetHeaderFooterUrl(const CefString& url) 
    {
        m_headerFooterEnabled = true;
        m_headerFooterUrl = url;
    }

    // Get prepared PDF setting for CEF
    CefPdfPrintSettings GetCefPdfPrintSettings() const;

    Status GetStatus() {
        return m_status;
    }

    void SetStatus(Status status) {
        m_status = status;
    }

private:
    CefString m_outputPath;
    OutputFormat m_outputFormat;
    CaptureMode m_captureMode;
    int m_viewWidth;
    int m_viewHeight;
    int m_imageQuality;
    ImageBackground m_imageBackground;
    CefString m_inputMediaType;
    int m_delay;
    bool m_waitForSignal;
    int m_waitSignalTimeout;
    std::string m_saveHtmlPath;
    bool m_saveHtmlStaticOnly;
    PageSize m_pageSize;
    PageOrientation m_pageOrientation;
    PageMargin m_pageMargin;
    bool m_backgrounds;
    Status m_status;
    Callback m_callback;
    int m_scale;
    bool m_headerFooterEnabled;
    CefString m_headerFooterTitle;
    CefString m_headerFooterUrl;
    // Include the default reference counting implementation.
    IMPLEMENT_REFCOUNTING(Job);
};

const char* GetOutputExtension(Job::OutputFormat format);
const char* GetOutputMimeType(Job::OutputFormat format);

} // namespace job
} // namespace cefpdf

#endif // JOB_JOB_H_
