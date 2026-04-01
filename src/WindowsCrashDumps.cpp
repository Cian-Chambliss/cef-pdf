#include "WindowsCrashDumps.h"

#if defined(OS_WIN)

#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string g_dumpFilePrefix;
int g_maxDumpFiles = 5;

bool isPathSeparator(char c)
{
    return c == '\\' || c == '/';
}

std::string joinPath(const std::string& directory, const std::string& fileName)
{
    if (directory.empty() || directory == ".") {
        return fileName;
    }

    if (isPathSeparator(directory[directory.size() - 1])) {
        return directory + fileName;
    }

    return directory + "\\" + fileName;
}

void splitDumpPrefix(const std::string& dumpFilePrefix, std::string& directory, std::string& filePrefix)
{
    const std::string::size_type separatorPos = dumpFilePrefix.find_last_of("\\/");
    if (separatorPos == std::string::npos) {
        directory = ".";
        filePrefix = dumpFilePrefix;
    } else {
        directory = dumpFilePrefix.substr(0, separatorPos);
        filePrefix = dumpFilePrefix.substr(separatorPos + 1);
    }

    if (directory.empty()) {
        directory = ".";
    }

    if (filePrefix.empty()) {
        filePrefix = "cef_pdf";
    }
}

bool ensureDirectoryExists(const std::string& directory)
{
    if (directory.empty() || directory == ".") {
        return true;
    }

    char fullPath[MAX_PATH];
    DWORD fullPathLength = GetFullPathNameA(directory.c_str(), MAX_PATH, fullPath, NULL);
    if (fullPathLength == 0 || fullPathLength >= MAX_PATH) {
        return false;
    }

    std::string normalized(fullPath);
    std::replace(normalized.begin(), normalized.end(), '/', '\\');

    size_t pos = 0;
    if (normalized.size() > 1 && normalized[1] == ':') {
        pos = 3;
    } else if (normalized.size() > 1 && normalized[0] == '\\' && normalized[1] == '\\') {
        pos = normalized.find('\\', 2);
        if (pos == std::string::npos) {
            return false;
        }
        pos = normalized.find('\\', pos + 1);
        if (pos == std::string::npos) {
            return false;
        }
        ++pos;
    }

    while (pos <= normalized.size()) {
        size_t next = normalized.find('\\', pos);
        const std::string current = normalized.substr(0, next);
        if (!current.empty()) {
            if (!CreateDirectoryA(current.c_str(), NULL)) {
                const DWORD createError = GetLastError();
                if (createError != ERROR_ALREADY_EXISTS) {
                    return false;
                }
            }
        }

        if (next == std::string::npos) {
            break;
        }

        pos = next + 1;
    }

    return true;
}

struct DumpFileInfo {
    std::string path;
    ULONGLONG lastWriteTime;
};

bool compareDumpFileInfo(const DumpFileInfo& lhs, const DumpFileInfo& rhs)
{
    return lhs.lastWriteTime < rhs.lastWriteTime;
}

void pruneDumpFiles(const std::string& directory, const std::string& filePrefix, size_t maxFiles)
{
    std::vector<DumpFileInfo> files;

    WIN32_FIND_DATAA findData;
    const std::string searchPattern = joinPath(directory, filePrefix + "*.dmp");
    HANDLE findHandle = FindFirstFileA(searchPattern.c_str(), &findData);
    if (findHandle == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            const ULONGLONG writeTime =
                (static_cast<ULONGLONG>(findData.ftLastWriteTime.dwHighDateTime) << 32ULL) |
                static_cast<ULONGLONG>(findData.ftLastWriteTime.dwLowDateTime);

            files.push_back({
                joinPath(directory, findData.cFileName),
                writeTime
            });
        }
    } while (FindNextFileA(findHandle, &findData) != 0);

    FindClose(findHandle);

    if (files.size() <= maxFiles) {
        return;
    }

    std::sort(files.begin(), files.end(), compareDumpFileInfo);

    const size_t filesToDelete = files.size() - maxFiles;
    for (size_t i = 0; i < filesToDelete; ++i) {
        DeleteFileA(files[i].path.c_str());
    }
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* exceptionPointers)
{
    if (g_dumpFilePrefix.empty()) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    std::string directory;
    std::string filePrefix;
    splitDumpPrefix(g_dumpFilePrefix, directory, filePrefix);

    if (!ensureDirectoryExists(directory)) {
        std::cerr << "dump: failed to create dump directory: " << directory << std::endl;
        return EXCEPTION_EXECUTE_HANDLER;
    }

    size_t maxDumpFiles = static_cast<size_t>(g_maxDumpFiles > 0 ? g_maxDumpFiles : 5);
    if (maxDumpFiles > 0) {
        pruneDumpFiles(directory, filePrefix, maxDumpFiles - 1);
    }

    SYSTEMTIME localTime;
    GetLocalTime(&localTime);

    char timestamp[48];
    sprintf_s(
        timestamp,
        "%04u%02u%02u_%02u%02u%02u_%03u_%lu",
        static_cast<unsigned int>(localTime.wYear),
        static_cast<unsigned int>(localTime.wMonth),
        static_cast<unsigned int>(localTime.wDay),
        static_cast<unsigned int>(localTime.wHour),
        static_cast<unsigned int>(localTime.wMinute),
        static_cast<unsigned int>(localTime.wSecond),
        static_cast<unsigned int>(localTime.wMilliseconds),
        static_cast<unsigned long>(GetCurrentProcessId())
    );

    const std::string dumpFileName = filePrefix + "_" + timestamp + ".dmp";
    const std::string dumpFilePath = joinPath(directory, dumpFileName);

    HANDLE dumpFile = CreateFileA(
        dumpFilePath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (dumpFile == INVALID_HANDLE_VALUE) {
        std::cerr << "dump: failed to create dump file: " << dumpFilePath << std::endl;
        return EXCEPTION_EXECUTE_HANDLER;
    }

    MINIDUMP_EXCEPTION_INFORMATION exceptionInfo;
    exceptionInfo.ThreadId = GetCurrentThreadId();
    exceptionInfo.ExceptionPointers = exceptionPointers;
    exceptionInfo.ClientPointers = FALSE;

    const BOOL writeOk = MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        dumpFile,
        MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory,
        exceptionPointers != NULL ? &exceptionInfo : NULL,
        NULL,
        NULL
    );

    CloseHandle(dumpFile);

    if (writeOk) {
        std::cerr << "dump: wrote unhandled exception dump: " << dumpFilePath << std::endl;
    } else {
        std::cerr << "dump: failed to write dump file: " << dumpFilePath << std::endl;
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

namespace cefpdf {

void ConfigureWindowsCrashDumps(const std::string& dumpFilePrefix, int maxDumpFiles)
{
    g_dumpFilePrefix = dumpFilePrefix;
    g_maxDumpFiles = maxDumpFiles > 0 ? maxDumpFiles : 5;

    SetUnhandledExceptionFilter(unhandledExceptionFilter);
}

} // namespace cefpdf

#else

namespace cefpdf {

void ConfigureWindowsCrashDumps(const std::string& dumpFilePrefix, int maxDumpFiles)
{
    (void)dumpFilePrefix;
    (void)maxDumpFiles;
}

} // namespace cefpdf

#endif // defined(OS_WIN)
