#include "Job.h"

namespace cefpdf {
namespace job {

const char* GetOutputExtension(Job::OutputFormat format)
{
    switch (format) {
        case Job::OutputFormat::PNG: return "png";
        case Job::OutputFormat::JPEG: return "jpg";
        case Job::OutputFormat::BMP: return "bmp";
        default: return "pdf";
    }
}

const char* GetOutputMimeType(Job::OutputFormat format)
{
    switch (format) {
        case Job::OutputFormat::PNG: return "image/png";
        case Job::OutputFormat::JPEG: return "image/jpeg";
        case Job::OutputFormat::BMP: return "image/bmp";
        default: return "application/pdf";
    }
}

Job::Job() :
    m_outputPath(),
    m_outputFormat(OutputFormat::PDF),
    m_captureMode(CaptureMode::FULL),
    m_viewWidth(128),
    m_viewHeight(128),
    m_imageQuality(90),
    m_imageBackground({255, 255, 255, 255}),
    m_inputMediaType("text/html"),
    m_delay(0),
    m_waitForSignal(false),
    m_waitSignalTimeout(0),
    m_saveHtmlPath(),
    m_saveHtmlStaticOnly(false),
    m_pageSize(),
    m_pageOrientation(PageOrientation::PORTRAIT),
    m_pageMargin(),
    m_backgrounds(false),
    m_status(Job::Status::PENDING),
    m_callback(),
    m_scale(100),
    m_headerFooterEnabled(false),
    m_headerFooterTitle(),
    m_headerFooterUrl()
{
    SetPageSize(cefpdf::constants::pageSize);
    SetPageMargin("default");
}

void Job::SetPageSize(const CefString& pageSize)
{
    m_pageSize = getPageSize(pageSize);
}

void Job::SetLandscape(bool flag)
{
    m_pageOrientation = (flag ? PageOrientation::LANDSCAPE : PageOrientation::PORTRAIT);
}

void Job::SetPageMargin(const CefString& pageMargin)
{
    m_pageMargin = getPageMargin(pageMargin);
}

void Job::SetBackgrounds(bool flag)
{
    m_backgrounds = flag;
}

void Job::SetScale(int scale)
{
    DLOG(INFO) << "Scale factor: " << scale;
    m_scale = scale;
}

CefPdfPrintSettings Job::GetCefPdfPrintSettings() const
{
    CefPdfPrintSettings pdfSettings;

    pdfSettings.scale = (double) m_scale / 100.0;
    pdfSettings.print_background = m_backgrounds;
    pdfSettings.landscape = (m_pageOrientation == PageOrientation::LANDSCAPE);

    pdfSettings.paper_width  = (double)m_pageSize.width / 25.4;
    pdfSettings.paper_height = (double)m_pageSize.height / 25.4;

    pdfSettings.margin_type = m_pageMargin.type;
    pdfSettings.margin_top = (double)m_pageMargin.top / 25.4;
    pdfSettings.margin_right = (double)m_pageMargin.right / 25.4;
    pdfSettings.margin_bottom = (double)m_pageMargin.bottom / 25.4;
    pdfSettings.margin_left = (double)m_pageMargin.left / 25.4;
    if( m_headerFooterEnabled) 
    {
        pdfSettings.display_header_footer = true;
		CefString(&pdfSettings.header_template).FromString(m_headerFooterTitle);
		CefString(&pdfSettings.footer_template).FromString(m_headerFooterUrl);
    }
    return pdfSettings;
}

} // namespace job
} // namespace cefpdf
