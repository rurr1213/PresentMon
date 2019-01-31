/*
Copyright 2017-2019 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "PresentMon.hpp"

#include <algorithm>
#include <shlwapi.h>
#include <thread>

static std::thread gThread;
static bool gQuit = false;

// When we collect realtime ETW events, we don't receive the events in real
// time but rather sometime after they occur.  Since the user might be toggling
// recording based on realtime cues (e.g., watching the target application) we
// maintain a history of realtime record toggle events from the user.  When we
// consider recording an event, we can look back and see what the recording
// state was at the time the event actually occurred.
//
// gRecordingToggleHistory is a vector of QueryPerformanceCounter() values at
// times when the recording state changed, and gIsRecording is the recording
// state at the current time.
//
// CRITICAL_SECTION used as this is expected to have low contention (e.g., *no*
// contention when capturing from ETL).

static CRITICAL_SECTION gRecordingToggleCS;
static std::vector<uint64_t> gRecordingToggleHistory;
static bool gIsRecording = false;

void SetOutputRecordingState(bool record)
{
    auto const& args = GetCommandLineArgs();

    if (gIsRecording == record) {
        return;
    }

    // When capturing from an ETL file, just use the current recording state.
    // It's not clear how best to map realtime to ETL QPC time, and there
    // aren't any realtime cues in this case.
    if (args.mEtlFileName != nullptr) {
        EnterCriticalSection(&gRecordingToggleCS);
        gIsRecording = record;
        LeaveCriticalSection(&gRecordingToggleCS);
        return;
    }

    uint64_t qpc = 0;
    QueryPerformanceCounter((LARGE_INTEGER*) &qpc);

    EnterCriticalSection(&gRecordingToggleCS);
    gRecordingToggleHistory.emplace_back(qpc);
    gIsRecording = record;
    LeaveCriticalSection(&gRecordingToggleCS);
}

static bool CopyRecordingToggleHistory(std::vector<uint64_t>* recordingToggleHistory)
{
    EnterCriticalSection(&gRecordingToggleCS);
    recordingToggleHistory->assign(gRecordingToggleHistory.begin(), gRecordingToggleHistory.end());
    auto isRecording = gIsRecording;
    LeaveCriticalSection(&gRecordingToggleCS);

    auto recording = recordingToggleHistory->size() + (isRecording ? 1 : 0);
    return (recording & 1) == 1;
}

// Remove recording toggle events that we've processed.
static void UpdateRecordingToggles(size_t nextIndex)
{
    if (nextIndex > 0) {
        EnterCriticalSection(&gRecordingToggleCS);
        gRecordingToggleHistory.erase(gRecordingToggleHistory.begin(), gRecordingToggleHistory.begin() + nextIndex);
        LeaveCriticalSection(&gRecordingToggleCS);
    }
}

// Processes are handled differently when running in realtime collection vs.
// ETL collection.  When reading an ETL, we receive NT_PROCESS events whenever
// a process is created or exits which we use to update the active processes.
//
// When collecting events in realtime, we update the active processes whenever
// we notice an event with a new process id.  If it's a target process, we
// obtain a handle to the process, and periodically check it to see if it has
// exited.

static std::unordered_map<uint32_t, ProcessInfo> gProcesses;
static uint32_t gTargetProcessCount = 0;

static bool IsTargetProcess(uint32_t processId, std::string const& processName)
{
    auto const& args = GetCommandLineArgs();

    // -exclude
    for (auto excludeProcessName : args.mExcludeProcessNames) {
        if (_stricmp(excludeProcessName, processName.c_str()) == 0) {
            return false;
        }
    }

    // -capture_all
    if (args.mTargetPid == 0 && args.mTargetProcessNames.empty()) {
        return true;
    }

    // -process_id
    if (args.mTargetPid != 0 && args.mTargetPid == processId) {
        return true;
    }

    // -process_name
    for (auto targetProcessName : args.mTargetProcessNames) {
        if (_stricmp(targetProcessName, processName.c_str()) == 0) {
            return true;
        }
    }

    return false;
}

static void InitProcessInfo(PresentMonData& pm, ProcessInfo* processInfo, uint32_t processId, HANDLE handle, std::string const& processName)
{
    auto target = IsTargetProcess(processId, processName);

    processInfo->mHandle        = handle;
    processInfo->mModuleName    = processName;
    processInfo->mOutputFile    = nullptr;
    processInfo->mLsrOutputFile = nullptr;
    processInfo->mTargetProcess = target;

    if (target) {
        // Create any CSV files that need process info to be created
        CreateProcessCSVs(pm, processInfo, processName);

        // Include process in -terminate_on_proc_exit count
        gTargetProcessCount += 1;
    }
}

static ProcessInfo* GetProcessInfo(PresentMonData& pm, uint32_t processId)
{
    auto result = gProcesses.emplace(processId, ProcessInfo());
    auto processInfo = &result.first->second;
    auto newProcess = result.second;

    if (newProcess) {
        // In ETL capture, we should have gotten an NTProcessEvent for this
        // process updated via UpdateNTProcesses(), so this path should only
        // happen in realtime capture.
        char path[MAX_PATH];
        DWORD numChars = sizeof(path);
        auto h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        auto processName = QueryFullProcessImageNameA(h, 0, path, &numChars) ? PathFindFileNameA(path) : "<error>";

        InitProcessInfo(pm, processInfo, processId, h, processName);
    }

    return processInfo;
}

// Check if any realtime processes terminated and add them to the terminated
// list.
//
// We assume that the process terminated now, which is wrong but conservative
// and functionally ok because no other process should start with the same PID
// as long as we're still holding a handle to it.
static void CheckForTerminatedRealtimeProcesses(std::vector<std::pair<uint32_t, uint64_t>>* terminatedProcesses)
{
    for (auto& pair : gProcesses) {
        auto processId = pair.first;
        auto processInfo = &pair.second;

        DWORD exitCode = 0;
        if (processInfo->mHandle != NULL && GetExitCodeProcess(processInfo->mHandle, &exitCode) && exitCode != STILL_ACTIVE) {
            uint64_t qpc = 0;
            QueryPerformanceCounter((LARGE_INTEGER*) &qpc);
            terminatedProcesses->emplace_back(processId, qpc);
            CloseHandle(processInfo->mHandle);
            processInfo->mHandle = NULL;
        }
    }
}

static void HandleTerminatedProcess(PresentMonData& pm, uint32_t processId)
{
    auto const& args = GetCommandLineArgs();

    auto iter = gProcesses.find(processId);
    if (iter == gProcesses.end()) {
        return; // shouldn't happen.
    }

    auto processInfo = &iter->second;
    if (processInfo->mTargetProcess) {
        // Save the output files in case the process is re-started.
        if (args.mMultiCsv) {
            pm.mProcessOutputFiles.emplace(processInfo->mModuleName, std::make_pair(processInfo->mOutputFile, processInfo->mLsrOutputFile));
        }

        // Quit if this is the last process tracked for -terminate_on_proc_exit.
        gTargetProcessCount -= 1;
        if (args.mTerminateOnProcExit && gTargetProcessCount == 0) {
            ExitMainThread();
        }
    }

    gProcesses.erase(iter);
}

static void UpdateNTProcesses(PresentMonData& pmData, std::vector<NTProcessEvent> const& ntProcessEvents, std::vector<std::pair<uint32_t, uint64_t>>* terminatedProcesses)
{
    for (auto const& ntProcessEvent : ntProcessEvents) {
        // An empty ImageFileName indicates that the event is a process
        // termination; record the termination in terminatedProcess to be
        // handled once the present event stream catches up to the termination
        // time.
        if (ntProcessEvent.ImageFileName.empty()) {
            terminatedProcesses->emplace_back(ntProcessEvent.ProcessId, ntProcessEvent.QpcTime);
            continue;
        }

        // This event is a new process starting, the pid should not already be
        // in gProcesses.
        auto result = gProcesses.emplace(ntProcessEvent.ProcessId, ProcessInfo());
        auto processInfo = &result.first->second;
        auto newProcess = result.second;
        if (newProcess) {
            InitProcessInfo(pmData, processInfo, ntProcessEvent.ProcessId, NULL, ntProcessEvent.ImageFileName);
        }
    }
}

static void AddPresents(PresentMonData& pm, std::vector<std::shared_ptr<PresentEvent>> const& presentEvents, size_t* presentEventIndex,
                        bool recording, bool checkStopQpc, uint64_t stopQpc, bool* hitStopQpc)
{
    auto i = *presentEventIndex;
    for (auto n = presentEvents.size(); i < n; ++i) {
        auto presentEvent = presentEvents[i];

        // Stop processing events if we hit the next stop time.
        if (checkStopQpc && presentEvent->QpcTime >= stopQpc) {
            *hitStopQpc = true;
            break;
        }

        // Look up the swapchain this present belongs to.
        auto processInfo = GetProcessInfo(pm, presentEvent->ProcessId);
        if (!processInfo->mTargetProcess) {
            continue;
        }

        auto& chain = processInfo->mChainMap[presentEvent->SwapChainAddress];
        chain.AddPresentToSwapChain(*presentEvent);

        // Output CSV row if recording (need to do this before updating chain).
        if (recording) {
            UpdateCSV(pm, *processInfo, chain, *presentEvent);
        }

        // Add the present to the swapchain history.
        chain.UpdateSwapChainInfo(*presentEvent);
    }

    *presentEventIndex = i;
}

static void AddPresents(PresentMonData& pm, LateStageReprojectionData* lsrData,
                        std::vector<std::shared_ptr<LateStageReprojectionEvent>> const& presentEvents, size_t* presentEventIndex,
                        bool recording, bool checkStopQpc, uint64_t stopQpc, bool* hitStopQpc)
{
    auto const& args = GetCommandLineArgs();

    auto i = *presentEventIndex;
    for (auto n = presentEvents.size(); i < n; ++i) {
        auto presentEvent = presentEvents[i];

        // Stop processing events if we hit the next stop time.
        if (checkStopQpc && presentEvent->QpcTime >= stopQpc) {
            *hitStopQpc = true;
            break;
        }

        const uint32_t appProcessId = presentEvent->GetAppProcessId();
        auto processInfo = GetProcessInfo(pm, appProcessId);
        if (!processInfo->mTargetProcess) {
            continue;
        }

        if ((args.mVerbosity > Verbosity::Simple) && (appProcessId == 0)) {
            continue; // Incomplete event data
        }

        lsrData->AddLateStageReprojection(*presentEvent);

        if (recording) {
            UpdateLSRCSV(pm, *lsrData, processInfo, *presentEvent);
        }

        lsrData->UpdateLateStageReprojectionInfo();
    }

    *presentEventIndex = i;
}

static void ProcessEvents(
    PresentMonData& pmData,
    LateStageReprojectionData* lsrData,
    std::vector<NTProcessEvent>* ntProcessEvents,
    std::vector<std::shared_ptr<PresentEvent>>* presentEvents,
    std::vector<std::shared_ptr<LateStageReprojectionEvent>>* lsrEvents,
    std::vector<uint64_t>* recordingToggleHistory,
    std::vector<std::pair<uint32_t, uint64_t>>* terminatedProcesses)
{
    // Copy any analyzed information from ConsumerThread.
    DequeueAnalyzedInfo(ntProcessEvents, presentEvents, lsrEvents);

    // Copy the record range history form the MainThread.
    auto recording = CopyRecordingToggleHistory(recordingToggleHistory);

    // Process NTProcess events; created processes are added to gProcesses and
    // termianted processes are added to termiantedProcesses.
    //
    // Handling of terminated processes need to be deferred until we observe
    // present event that started after the termination time.  This is because
    // while a present must start before termination, it can complete after
    // termination.
    //
    // We don't have to worry about the recording toggles here because
    // NTProcess events are only captured when parsing ETL files and we don't
    // use recording toggle history for ETL files.
    UpdateNTProcesses(pmData, *ntProcessEvents, terminatedProcesses);

    // Next, iterate through the recording toggles (if any)...
    size_t presentEventIndex = 0;
    size_t lsrEventIndex = 0;
    size_t recordingToggleIndex = 0;
    size_t terminatedProcessIndex = 0;
    for (;;) {
        auto checkRecordingToggle   = recordingToggleIndex < recordingToggleHistory->size();
        auto nextRecordingToggleQpc = checkRecordingToggle ? (*recordingToggleHistory)[recordingToggleIndex] : 0ull;
        auto hitNextRecordingToggle = false;

        // First iterate through the terminated process history up until the
        // next recording toggle.  If we hit a present that started after the
        // termination, we can handle the process termination and continue.
        // Otherwise, we're done handling all the presents and any outstanding
        // terminations will have to wait for the next batch of events.
        for (; terminatedProcessIndex < terminatedProcesses->size(); ++terminatedProcessIndex) {
            auto const& pair = (*terminatedProcesses)[terminatedProcessIndex];
            auto terminatedProcessId = pair.first;
            auto terminatedProcessQpc = pair.second;

            if (checkRecordingToggle && nextRecordingToggleQpc < terminatedProcessQpc) {
                break;
            }

            auto hitTerminatedProcess = false;
            AddPresents(pmData, *presentEvents, &presentEventIndex, recording, true, terminatedProcessQpc, &hitTerminatedProcess);
            AddPresents(pmData, lsrData, *lsrEvents, &lsrEventIndex, recording, true, terminatedProcessQpc, &hitTerminatedProcess);
            if (!hitTerminatedProcess) {
                goto done;
            }
            HandleTerminatedProcess(pmData, terminatedProcessId);
        }

        // Process present events up until the next recording toggle.  If we
        // reached the toggle, handle it and continue.  Otherwise, we're done
        // handling all the presents and any outstanding toggles will have to
        // wait for next batch of events.
        AddPresents(pmData, *presentEvents, &presentEventIndex, recording, checkRecordingToggle, nextRecordingToggleQpc, &hitNextRecordingToggle);
        AddPresents(pmData, lsrData, *lsrEvents, &lsrEventIndex, recording, checkRecordingToggle, nextRecordingToggleQpc, &hitNextRecordingToggle);
        if (!hitNextRecordingToggle) {
            break;
        }

        // Toggle recording.
        recordingToggleIndex += 1;
        recording = !recording;
    }

done:

    // Clear events processed.
    ntProcessEvents->clear();
    presentEvents->clear();
    lsrEvents->clear();
    recordingToggleHistory->clear();

    // Finished processing all events.  Erase the recording toggles and
    // terminated processes that we also handled now.
    UpdateRecordingToggles(recordingToggleIndex);
    if (terminatedProcessIndex > 0) {
        terminatedProcesses->erase(terminatedProcesses->begin(), terminatedProcesses->begin() + terminatedProcessIndex);
    }
}

void Output()
{
    auto const& args = GetCommandLineArgs();

    // Structures to track processes and statistics from recorded events.
    PresentMonData pmData;
    LateStageReprojectionData lsrData;

    // Create any CSV files that don't need process info to be created
    CreateNonProcessCSVs(pmData);

    // Enter loop to consume collected events.
    std::vector<NTProcessEvent> ntProcessEvents;
    std::vector<std::shared_ptr<PresentEvent>> presentEvents;
    std::vector<std::shared_ptr<LateStageReprojectionEvent>> lsrEvents;
    std::vector<uint64_t> recordingToggleHistory;
    std::vector<std::pair<uint32_t, uint64_t>> terminatedProcesses;
    ntProcessEvents.reserve(128);
    presentEvents.reserve(4096);
    lsrEvents.reserve(4096);
    recordingToggleHistory.reserve(16);
    terminatedProcesses.reserve(16);

    for (;;) {
        // Read gQuit here, but then check it after processing queued events.
        // This ensures that we call DequeueAnalyzedInfo() at least once after
        // events have stopped being collected so that all events are included.
        auto quit = gQuit;

        // Copy and process all the collected events, and update the various
        // tracking and statistics data structures.
        ProcessEvents(pmData, &lsrData, &ntProcessEvents, &presentEvents, &lsrEvents, &recordingToggleHistory, &terminatedProcesses);

        // Display information to console if requested.  If debug build and
        // simple console, print a heartbeat if recording.
        //
        // gIsRecording is the real timeline recording state.  Because we're
        // just reading it without correlation to gRecordingToggleHistory, we
        // don't need the critical section.
        auto realtimeRecording = gIsRecording;
        if (!args.mSimpleConsole) {
            std::string display;
            for (auto const& pair : gProcesses) {
                UpdateConsole(pair.first, pair.second, &display);
            }
            UpdateConsole(gProcesses, lsrData, &display);
            SetConsoleText(display.c_str());

            if (realtimeRecording) {
                printf("** RECORDING **\n");
            }
        }
#if _DEBUG
        else if (realtimeRecording) {
            printf(".");
        }
#endif

        // Everything is processed and output out at this point, so if we're
        // quiting we don't need to update the rest.
        if (quit) {
            break;
        }

        // Update tracking information.
        CheckForTerminatedRealtimeProcesses(&terminatedProcesses);

        // Sleep to reduce overhead.
        Sleep(100);
    }

    if (args.mSimpleConsole == false) {
        SetConsoleText("");
    }

    // Shut down output.
    uint32_t eventsLost = 0;
    uint32_t buffersLost = 0;
    CheckLostReports(&eventsLost, &buffersLost);

    CloseCSVs(pmData, &gProcesses, eventsLost, buffersLost);

    for (auto& pair : gProcesses) {
        auto processInfo = &pair.second;
        if (processInfo->mHandle != NULL) {
            CloseHandle(processInfo->mHandle);
        }
    }
    gProcesses.clear();
}

void StartOutputThread()
{
    InitializeCriticalSection(&gRecordingToggleCS);

    gThread = std::thread(Output);
}

void StopOutputThread()
{
    if (gThread.joinable()) {
        gQuit = true;
        gThread.join();

        DeleteCriticalSection(&gRecordingToggleCS);
    }
}

