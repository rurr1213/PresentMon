#pragma once

#include <time.h>

// This files allows PresentMon to call and external app, that desires GPU info, by
// registering a callback.

#define GPUINFO 1

class GPUInfoConsoleData {
public:
    std::string processName;
    int processId;
    std::string runTime;
    int address;
    int syncInterval;
    int flags;
    double framemSecs;
    double fps;

    time_t timeStamp;

    GPUInfoConsoleData& operator= (GPUInfoConsoleData&);
};

class GPUInfoCsvData {
public:
    std::string processName;
    int processId;
    std::string runTime;
    int syncInterval;
    int flags;

    int supportsTearing;
    std::string presentMode;
    int wasBatched;
    int dwmNotified;

    std::string finalState;
    double timeInSeconds;
    double msBetweenPresents;
    double msBetweenDisplayChange;
    double msInPresentApi;
    double msUntilRenderComplete;
    double msUntilDisplayed;

    long long outputQpcTime;

    time_t timeStamp;

    GPUInfoCsvData& operator= (GPUInfoCsvData&);
};


class IGPUInfoCallback {
public:
    virtual void notifyHostConsoleData(GPUInfoConsoleData&)=0;
    virtual void notifyHostCsvData(GPUInfoCsvData&)=0;
};

class GPUInfo
{
public:

    IGPUInfoCallback* m_pgpuInfoCallback;
    bool  m_running;
    GPUInfo();
    ~GPUInfo();
    static bool start(int argc, char** argv, IGPUInfoCallback* pgpuInfoCallback);
    static bool stop(void);
};





