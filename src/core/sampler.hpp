/**
 * @file  sampler.hpp
 * @brief Sampler thread — drives HSS/POLL/SHM reading and wave generation.
 *
 * The sampler runs as a dedicated background thread, reading telemetry data
 * from J-Link HSS, polling, or shared memory and pushing it into
 * MonitorChannel buffers for GUI consumption.
 */
#ifndef SAMPLER_HPP
#define SAMPLER_HPP

#include <atomic>

class Gui;

// Entry point for the sampler thread. Runs until g_appRunning is set to false.
void threadFunc(Gui *gui);

// CPU core the sampler thread should bind to. -1 = auto (highest available).
// Written by GUI, read by sampler thread on the next iteration after
// g_samplerCpuRebind is set.
extern std::atomic<int>  g_samplerCpuCore;
extern std::atomic<bool> g_samplerCpuRebind;

// After setupRealtimeThread() runs, this holds the actual core the sampler
// bound to.  main.cpp reads it to exclude that core from all other threads.
// -1 means not yet bound.
extern std::atomic<int> g_samplerBoundCore;

#endif // !SAMPLER_HPP
