#ifndef SERVER_SESSION_MANAGER_H_
#define SERVER_SESSION_MANAGER_H_

#include "Session.h"

#include "include/cef_base.h"

#include <set>
#include <chrono>

namespace cefpdf {
namespace server {

class SessionManager : public CefBaseRefCounted
{

public:
    SessionManager();

    SessionManager(const SessionManager&) = delete;

    SessionManager& operator=(const SessionManager&) = delete;

    void Start(CefRefPtr<Session>);

    void Stop(CefRefPtr<Session>);

    void CloseAll();

    void StopAll();

    // Update the last activity timestamp to now. Call on Start/Stop
    void UpdateActivity();

    // Return whether there are no active sessions
    bool IsEmpty() const;

    // Return the last activity time point
    std::chrono::steady_clock::time_point LastActivity() const;

private:
    std::set<CefRefPtr<Session>> m_sessions;
    std::chrono::steady_clock::time_point m_lastActivity;

    // Include the default reference counting implementation.
    IMPLEMENT_REFCOUNTING(SessionManager);
};

} // namespace server
} // namespace cefpdf

#endif // SERVER_SESSION_MANAGER_H_
