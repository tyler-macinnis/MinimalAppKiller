#define NOMINMAX
#include <Windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace
{
    std::atomic<bool> g_running{true};

    struct Config
    {
        std::vector<std::wstring> processNames;
        unsigned int intervalMs = 500;
        unsigned int noHitLogEveryLoops = 60;
    };

    std::wstring Trim(const std::wstring &value)
    {
        const auto isSpace = [](wchar_t ch)
        {
            return ::iswspace(ch) != 0;
        };

        size_t start = 0;
        while (start < value.size() && isSpace(value[start]))
        {
            ++start;
        }

        size_t end = value.size();
        while (end > start && isSpace(value[end - 1]))
        {
            --end;
        }

        return value.substr(start, end - start);
    }

    std::wstring ToLower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch)
                       { return static_cast<wchar_t>(::towlower(ch)); });
        return value;
    }

    void PrintUsage(const wchar_t *appName)
    {
        std::wcout << L"Usage: " << appName << L" [config-file]\n";
        std::wcout << L"Default config file: appkiller.conf\n";
    }

    bool ParseUnsigned(const std::wstring &text, unsigned int &outValue)
    {
        try
        {
            size_t consumed = 0;
            const unsigned long parsed = std::stoul(text, &consumed, 10);
            if (consumed != text.size() || parsed > UINT_MAX)
            {
                return false;
            }

            outValue = static_cast<unsigned int>(parsed);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    std::optional<Config> LoadConfig(const std::filesystem::path &configPath)
    {
        std::wifstream file(configPath);
        if (!file)
        {
            std::wcerr << L"Failed to open config: " << configPath.wstring() << L"\n";
            return std::nullopt;
        }

        Config config;
        std::unordered_set<std::wstring> seen;

        std::wstring line;
        unsigned int lineNumber = 0;
        while (std::getline(file, line))
        {
            ++lineNumber;
            std::wstring trimmed = Trim(line);
            if (trimmed.empty() || trimmed.starts_with(L'#'))
            {
                continue;
            }

            if (trimmed.starts_with(L"interval_ms="))
            {
                const std::wstring value = Trim(trimmed.substr(std::wstring_view(L"interval_ms=").size()));
                unsigned int parsed = 0;
                if (!ParseUnsigned(value, parsed) || parsed == 0)
                {
                    std::wcerr << L"Invalid interval_ms at line " << lineNumber << L"\n";
                    return std::nullopt;
                }

                config.intervalMs = parsed;
                continue;
            }

            if (trimmed.starts_with(L"no_hit_log_every_loops="))
            {
                const std::wstring value = Trim(trimmed.substr(std::wstring_view(L"no_hit_log_every_loops=").size()));
                unsigned int parsed = 0;
                if (!ParseUnsigned(value, parsed) || parsed == 0)
                {
                    std::wcerr << L"Invalid no_hit_log_every_loops at line " << lineNumber << L"\n";
                    return std::nullopt;
                }

                config.noHitLogEveryLoops = parsed;
                continue;
            }

            const std::wstring normalized = ToLower(trimmed);
            if (seen.insert(normalized).second)
            {
                config.processNames.push_back(trimmed);
            }
        }

        if (config.processNames.empty())
        {
            std::wcerr << L"Config has no process names. Add one exe name per line (for example: notepad.exe).\n";
            return std::nullopt;
        }

        return config;
    }

    std::vector<DWORD> FindTargetPids(const std::unordered_set<std::wstring> &targets)
    {
        std::vector<DWORD> pids;

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return pids;
        }

        PROCESSENTRY32W process{};
        process.dwSize = sizeof(process);

        if (!Process32FirstW(snapshot, &process))
        {
            CloseHandle(snapshot);
            return pids;
        }

        do
        {
            std::wstring exe = ToLower(process.szExeFile);
            if (targets.find(exe) != targets.end())
            {
                pids.push_back(process.th32ProcessID);
            }
        } while (Process32NextW(snapshot, &process));

        CloseHandle(snapshot);
        return pids;
    }

    bool KillProcessByPid(DWORD pid)
    {
        HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (process == nullptr)
        {
            return false;
        }

        const BOOL terminated = TerminateProcess(process, 1);
        CloseHandle(process);
        return terminated != FALSE;
    }

    BOOL WINAPI ConsoleCtrlHandler(DWORD signal)
    {
        switch (signal)
        {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_running = false;
            return TRUE;
        default:
            return FALSE;
        }
    }
} // namespace

int wmain(int argc, wchar_t *argv[])
{
    if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE))
    {
        std::wcerr << L"Failed to register console handler.\n";
        return 1;
    }

    std::filesystem::path configPath = L"appkiller.conf";
    if (argc >= 2)
    {
        const std::wstring arg = argv[1];
        if (arg == L"-h" || arg == L"--help" || arg == L"/?")
        {
            PrintUsage(argv[0]);
            return 0;
        }

        configPath = arg;
    }

    std::optional<Config> config = LoadConfig(configPath);
    if (!config)
    {
        return 1;
    }

    std::filesystem::file_time_type lastConfigWriteTime{};
    std::error_code fsError;
    if (std::filesystem::exists(configPath, fsError))
    {
        lastConfigWriteTime = std::filesystem::last_write_time(configPath, fsError);
    }

    std::wcout << L"MinimalAppKiller started. Press Ctrl+C to stop.\n";
    std::wcout << L"Config: " << configPath.wstring() << L"\n";
    std::wcout << L"Targets:";
    for (const auto &name : config->processNames)
    {
        std::wcout << L" " << name;
    }
    std::wcout << L"\n";

    auto buildTargets = [](const Config &cfg)
    {
        std::unordered_set<std::wstring> t;
        t.reserve(cfg.processNames.size());
        for (const auto &name : cfg.processNames)
        {
            t.insert(ToLower(name));
        }
        return t;
    };

    std::unordered_set<std::wstring> targets = buildTargets(*config);
    unsigned int idleLoops = 0;

    while (g_running)
    {
        fsError.clear();
        const auto currentWriteTime = std::filesystem::last_write_time(configPath, fsError);
        if (!fsError && currentWriteTime != lastConfigWriteTime)
        {
            if (const auto reloaded = LoadConfig(configPath))
            {
                config = reloaded;
                targets = buildTargets(*config);
                lastConfigWriteTime = currentWriteTime;
                std::wcout << L"Config reloaded. " << config->processNames.size() << L" targets.\n";
            }
            else
            {
                std::wcerr << L"Config reload failed. Keeping previous config.\n";
            }
        }

        const std::vector<DWORD> targetPids = FindTargetPids(targets);

        unsigned int killedCount = 0;
        for (const DWORD pid : targetPids)
        {
            if (KillProcessByPid(pid))
            {
                ++killedCount;
            }
        }

        if (killedCount > 0)
        {
            idleLoops = 0;
            std::wcout << L"Killed " << killedCount << L" process(es).\n";
        }
        else
        {
            ++idleLoops;
            if (idleLoops >= config->noHitLogEveryLoops)
            {
                std::wcout << L"No target process found.\n";
                idleLoops = 0;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(config->intervalMs));
    }

    std::wcout << L"Stopping MinimalAppKiller.\n";
    return 0;
}
