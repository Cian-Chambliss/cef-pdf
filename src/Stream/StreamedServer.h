#ifndef STREAM_STREAMEDSERVER_H_
#define STREAM_STREAMEDSERVER_H_

#include "../Job/Job.h"
#include "include/cef_values.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace cefpdf {
class Client;

namespace stream {

#if defined(OS_WIN)
typedef void* ProtocolHandle;
#else
typedef int ProtocolHandle;
#endif

bool PrepareProtocolStdout(ProtocolHandle& handle, std::string& error);
void CloseProtocolHandle(ProtocolHandle handle);

class StreamedServer {
public:
    StreamedServer(CefRefPtr<Client> client, ProtocolHandle output);
    ~StreamedServer();

    void Start();
    void Join();
    int GetExitCode() const;

private:
    struct WriteItem {
        std::string json;
        bool stopAfterWrite;
    };

    void ReadLoop();
    void WriteLoop();
    void HandlePacket(const std::string& packet);
    void OnJobComplete(CefRefPtr<job::Job> job, CefRefPtr<CefValue> id,
        const std::string& idKey, const std::vector<std::string>& paths);
    void ReleaseRequest(const std::string& idKey, const std::vector<std::string>& paths);
    void FinishInput(CefRefPtr<CefValue> quitId);
    void FinishIfDrained();
    void FramingError(const std::string& message);
    void Enqueue(CefRefPtr<CefValue> value, bool stopAfterWrite = false);
    void EnqueueRaw(const std::string& json, bool stopAfterWrite);

    CefRefPtr<Client> m_client;
    ProtocolHandle m_output;
    std::thread m_reader;
    std::thread m_writer;
    mutable std::mutex m_mutex;
    std::condition_variable m_writeReady;
    std::condition_variable m_writeSpace;
    std::queue<WriteItem> m_writes;
    std::set<std::string> m_activeIds;
    std::set<std::string> m_activePaths;
    std::size_t m_pending;
    bool m_accepting;
    bool m_finishing;
    bool m_finalQueued;
    CefRefPtr<CefValue> m_quitId;
    int m_exitCode;
};

} // namespace stream
} // namespace cefpdf

#endif // STREAM_STREAMEDSERVER_H_
