#include "Job.h"

namespace cefpdf {
namespace job {

Job::Job() :
    m_outputPath(),
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
    pdfSettings.margin_top = m_pageMargin.top;
    pdfSettings.margin_right = m_pageMargin.right;
    pdfSettings.margin_bottom = m_pageMargin.bottom;
    pdfSettings.margin_left = m_pageMargin.left;
    if( m_headerFooterEnabled) 
    {
        pdfSettings.display_header_footer = true;
		CefString(&pdfSettings.header_template).FromString(m_headerFooterTitle);
		CefString(&pdfSettings.header_template).FromString(m_headerFooterUrl);
    }
    return pdfSettings;
}

} // namespace job
} // namespace cefpdf
