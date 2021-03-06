#include "PresentMon.hpp"
#include "GPUInfoCallback.h"


GPUInfo g_gpuInfo;

// -------------------------------------------------------------------------------------------

GPUInfoConsoleData& GPUInfoConsoleData::operator= (const GPUInfoConsoleData& gid) {
    processName = gid.processName;
    processId   = gid.processId;
    runTime     = gid.runTime;
    address     = gid.address;
    syncInterval = gid.syncInterval;
    flags       = gid.flags;
    framemSecs  = gid.framemSecs;
    fps         = gid.fps;

    time        = gid.time;

    return *this;
}

GPUInfoCsvData& GPUInfoCsvData::operator= (const GPUInfoCsvData& gid) {
    processName = gid.processName;
    processId   = gid.processId;
    runTime     = gid.runTime;
    syncInterval = gid.syncInterval;
    flags       = gid.flags;

    supportsTearing = gid.supportsTearing;
    presentMode = gid.presentMode;
    wasBatched = gid.wasBatched;
    dwmNotified = gid.dwmNotified;

    finalState = gid.finalState;
    timeInSeconds = gid.timeInSeconds;
    msBetweenPresents = gid.msBetweenPresents;
    msBetweenDisplayChange = gid.msBetweenDisplayChange;
    msInPresentApi = gid.msInPresentApi;
    msUntilRenderComplete = gid.msUntilRenderComplete;
    msUntilDisplayed = gid.msUntilDisplayed;

    outputQpcTime = gid.outputQpcTime;

    time = gid.time;

    return *this;
}

// -------------------------------------------------------------------------------------------

GPUInfo::GPUInfo() {
    m_running = false;
    m_pgpuInfoCallback = 0;
}

GPUInfo::~GPUInfo() {
    m_pgpuInfoCallback = 0;
}

bool GPUInfo::start(int argc, char** argv, IGPUInfoCallback* pgpuInfoCallback)
{
    g_gpuInfo.m_pgpuInfoCallback = pgpuInfoCallback;

    // Parse command line arguments.
    if (!ParseCommandLine(argc, argv)) {
        printf("Invalid args\n");
        return false;
    }
//    auto const& args = GetCommandLineArgs();

    // Attempt to elevate process privilege if necessary.
    //
    // If a new process needs to be started, this will wait for the elevated
    // process to complete in order to report stderr and exit code, and then
    // abort from within ElevatePrivilege() (i.e., the rest of this function
    // won't run in this process).
    ElevatePrivilege(argc, argv);

    // Start the ETW trace session (including consumer and output threads).
    if (!StartTraceSession()) {
        printf("Start Trace failed\n");
        return false;
    }

    g_gpuInfo.m_running = true;

    return true;
}

bool GPUInfo::stop(void)
{
    if (g_gpuInfo.m_running) {
        g_gpuInfo.m_running = false;
        StopTraceSession();
    }
    return true;
}

// -------------------------------------------------------------------------------------------------


bool GPUInfoCallbackIsRunning(void)
{
    return g_gpuInfo.m_running;
}

// This is a copy of UpdateConsole in OutputThread.cpp. This is intended to provide the
// same output info as UpdateConsole to GPUInfo. So, code is the same, so that the
// fps number reported is the same as the original function, and can be compared by others.
void GPUInfoCallback_UpdateConsole(uint32_t processId, ProcessInfo const& processInfo)
{
//    auto const& args = GetCommandLineArgs();
/*
    // Don't display non-target or empty processes
    if (!processInfo.mTargetProcess ||
        processInfo.mModuleName.empty() ||
        processInfo.mSwapChain.empty()) {
        return;
    }
*/
    GPUInfoConsoleData gpuInfoConsoleData;

    gpuInfoConsoleData.processId = NULL;

    auto empty = true;

    for (auto const& pair : processInfo.mSwapChain) {
        auto address = pair.first;
        auto const& chain = pair.second;

        // Only show swapchain data if there at least two presents in the
        // history.
        if (chain.mPresentHistoryCount < 2) {
            continue;
        }

        if (empty) {
            empty = false;
//            ConsolePrintLn("%s[%d]:", processInfo.mModuleName.c_str(), processId);
            gpuInfoConsoleData.processName = processInfo.mModuleName;
            gpuInfoConsoleData.processId = processId;
        }

        if (gpuInfoConsoleData.processId == NULL) {
            continue;
        }

        auto const& present0 = *chain.mPresentHistory[(chain.mNextPresentIndex - chain.mPresentHistoryCount) % SwapChainData::PRESENT_HISTORY_MAX_COUNT];
        auto const& presentN = *chain.mPresentHistory[(chain.mNextPresentIndex - 1) % SwapChainData::PRESENT_HISTORY_MAX_COUNT];
        auto cpuAvg = QpcDeltaToSeconds(presentN.QpcTime - present0.QpcTime) / (chain.mPresentHistoryCount - 1);

        gpuInfoConsoleData.address = (int)address;
        gpuInfoConsoleData.runTime = RuntimeToString(presentN.Runtime);
        gpuInfoConsoleData.syncInterval = presentN.SyncInterval;
        gpuInfoConsoleData.flags = presentN.PresentFlags;
        gpuInfoConsoleData.framemSecs = 1000.0 * cpuAvg;
        gpuInfoConsoleData.fps = 1.0 / cpuAvg;

        /*
        ConsolePrint("    %016llX (%s): SyncInterval=%d Flags=%d %.2lf ms/frame (%.1lf fps",
            address,
            RuntimeToString(presentN.Runtime),
            presentN.SyncInterval,
            presentN.PresentFlags,
            1000.0 * cpuAvg,
            1.0 / cpuAvg);
        */
            /*
        size_t displayCount = 0;
        uint64_t latencySum = 0;
        uint64_t display0ScreenTime = 0;
        PresentEvent* displayN = nullptr;
        if (args.mVerbosity > Verbosity::Simple) {
            for (uint32_t i = 0; i < chain.mPresentHistoryCount; ++i) {
                auto const& p = chain.mPresentHistory[(chain.mNextPresentIndex - chain.mPresentHistoryCount + i) % SwapChainData::PRESENT_HISTORY_MAX_COUNT];
                if (p->FinalState == PresentResult::Presented) {
                    if (displayCount == 0) {
                        display0ScreenTime = p->ScreenTime;
                    }
                    displayN = p.get();
                    latencySum += p->ScreenTime - p->QpcTime;
                    displayCount += 1;
                }
            }
        }

        if (displayCount >= 2) {
            ConsolePrint(", %.1lf fps displayed", (double)(displayCount - 1) / QpcDeltaToSeconds(displayN->ScreenTime - display0ScreenTime));
        }

        if (displayCount >= 1) {
            ConsolePrint(", %.2lf ms latency", 1000.0 * QpcDeltaToSeconds(latencySum) / displayCount);
        }

        ConsolePrint(")");

        if (displayCount > 0) {
            ConsolePrint(" %s", PresentModeToString(displayN->PresentMode));
        }

        ConsolePrintLn("");
        */
    }
    /*
    if (!empty) {
        ConsolePrintLn("");
    } */

    time(&gpuInfoConsoleData.time);

    if (!empty) {
        if (g_gpuInfo.m_pgpuInfoCallback != 0) {
            g_gpuInfo.m_pgpuInfoCallback->notifyHostConsoleData(gpuInfoConsoleData);
        }
    }
}

// This is a copy of UpdateCsv in OutputThread.cpp. This is intended to provide the
// same output info as UpdateCsv to GPUInfo. So, code is the same, so that the
// fps number reported is the same as the original function, and can be compared by others.
void GPUInfoCallback_UpdateCsv(ProcessInfo* processInfo, SwapChainData const& chain, PresentEvent const& p)
{
    auto const& args = GetCommandLineArgs();

    // Don't output dropped frames (if requested).
    auto presented = p.FinalState == PresentResult::Presented;
    if (args.mExcludeDropped && !presented) {
        return;
    }
    /*
    // Early return if not outputing to CSV.
    auto fp = GetOutputCsv(processInfo).mFile;
    if (fp == nullptr) {
        return;
    }
    */
    // Look up the last present event in the swapchain's history.  We need at
    // least two presents to compute frame statistics.
    if (chain.mPresentHistoryCount == 0) {
        return;
    }

    auto lastPresented = chain.mPresentHistory[(chain.mNextPresentIndex - 1) % SwapChainData::PRESENT_HISTORY_MAX_COUNT].get();

    // Compute frame statistics.
    double timeInSeconds = QpcToSeconds(p.QpcTime);
    double msBetweenPresents = 1000.0 * QpcDeltaToSeconds(p.QpcTime - lastPresented->QpcTime);
    double msInPresentApi = 1000.0 * QpcDeltaToSeconds(p.TimeTaken);
    double msUntilRenderComplete = 0.0;
    double msUntilDisplayed = 0.0;
    double msBetweenDisplayChange = 0.0;

    if (args.mVerbosity > Verbosity::Simple) {
        if (p.ReadyTime > 0) {
            msUntilRenderComplete = 1000.0 * QpcDeltaToSeconds(p.ReadyTime - p.QpcTime);
        }
        if (presented) {
            msUntilDisplayed = 1000.0 * QpcDeltaToSeconds(p.ScreenTime - p.QpcTime);

            if (chain.mLastDisplayedPresentIndex > 0) {
                auto lastDisplayed = chain.mPresentHistory[chain.mLastDisplayedPresentIndex % SwapChainData::PRESENT_HISTORY_MAX_COUNT].get();
                msBetweenDisplayChange = 1000.0 * QpcDeltaToSeconds(p.ScreenTime - lastDisplayed->ScreenTime);
            }
        }
    }

    GPUInfoCsvData gpuInfoCsvData;

    gpuInfoCsvData.processName = processInfo->mModuleName.c_str(),
    gpuInfoCsvData.processId = p.ProcessId, p.SwapChainAddress,
    gpuInfoCsvData.runTime = RuntimeToString(p.Runtime),
    gpuInfoCsvData.syncInterval = p.SyncInterval,
    gpuInfoCsvData.flags = p.PresentFlags;

    // Output in CSV format
    gpuInfoCsvData.supportsTearing = p.SupportsTearing;
    gpuInfoCsvData.presentMode = PresentModeToString(p.PresentMode);
    gpuInfoCsvData.wasBatched = p.WasBatched;
    gpuInfoCsvData.dwmNotified = p.DwmNotified;

    gpuInfoCsvData.finalState = FinalStateToDroppedString(p.FinalState);
    gpuInfoCsvData.timeInSeconds = timeInSeconds;
    gpuInfoCsvData.msBetweenPresents = msBetweenPresents;
    gpuInfoCsvData.msBetweenDisplayChange = msBetweenDisplayChange;

    gpuInfoCsvData.msInPresentApi = msInPresentApi;

    gpuInfoCsvData.msUntilRenderComplete = msUntilRenderComplete,
    gpuInfoCsvData.msUntilDisplayed =  msUntilDisplayed;

    gpuInfoCsvData.outputQpcTime = p.QpcTime;

    time(&gpuInfoCsvData.time);

    if (g_gpuInfo.m_pgpuInfoCallback != 0) {
        g_gpuInfo.m_pgpuInfoCallback->notifyHostCsvData(gpuInfoCsvData);
    }

}