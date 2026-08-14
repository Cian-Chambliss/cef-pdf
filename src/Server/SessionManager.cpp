#include "SessionManager.h"
#include <chrono>

namespace cefpdf {
namespace server {

SessionManager::SessionManager()
    : m_lastActivity(std::chrono::steady_clock::now())
{}

void SessionManager::Start(CefRefPtr<Session> session)
{
    session->Start();
    m_sessions.insert(session);
    UpdateActivity();
}

void SessionManager::Stop(CefRefPtr<Session> session)
{
    session->Close();
    m_sessions.erase(session);
    UpdateActivity();
}

void SessionManager::CloseAll()
{
    for (auto c: m_sessions) {
        c->Close();
    }
}

void SessionManager::StopAll()
{
    CloseAll();
    m_sessions.clear();
    UpdateActivity();
}

void SessionManager::UpdateActivity()
{
    m_lastActivity = std::chrono::steady_clock::now();
}

bool SessionManager::IsEmpty() const
{
    return m_sessions.empty();
}

std::chrono::steady_clock::time_point SessionManager::LastActivity() const
{
    return m_lastActivity;
}

} // namespace server
} // namespace cefpdf
