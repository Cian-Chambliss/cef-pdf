#include "RenderHandler.h"
#include "Job/Manager.h"

namespace cefpdf {

RenderHandler::RenderHandler(CefRefPtr<job::Manager> manager) : m_manager(manager)
{
    m_viewWidth = 128;
    m_viewHeight = 128;
}

void RenderHandler::SetViewWidth(int viewWidth)
{
    m_viewWidth = viewWidth;
    DLOG(INFO) << "View width: " << m_viewWidth;
}

void RenderHandler::SetViewHeight(int viewHeight)
{
    m_viewHeight = viewHeight;
    DLOG(INFO) << "View height: " << m_viewHeight;
}

// CefRenderHandler methods:
// -------------------------------------------------------------------------
void RenderHandler::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect)
{
    CefRefPtr<job::Job> job = m_manager->GetJob(browser);
    rect.x = 0;
    rect.y = 0;
    rect.width = job ? job->GetViewWidth() : m_viewWidth;
    rect.height = job ? job->GetViewHeight() : m_viewHeight;
}

void RenderHandler::OnPaint(
    CefRefPtr<CefBrowser> browser,
    CefRenderHandler::PaintElementType type,
    const CefRenderHandler::RectList& dirtyRects,
    const void* buffer, int width, int height
) {

}

} // namespace cefpdf
