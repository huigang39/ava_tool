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

// Entry point for the FFT worker thread. Computes scope spectra off the render
// thread so heavy fft_exec calls don't drag down the GUI frame rate. Runs until
// g_appRunning is set to false.
void fftThreadFunc(Gui *gui);

// Sampler run mode.  Written by GUI; sampler thread re-applies on next
// iteration after g_samplerRebind is set.
//   0 = Low      : normal priority, no affinity, always Sleep(1ms)
//   1 = Normal   : slightly elevated priority, no affinity, adaptive sleep
//   2 = CPUBound : realtime priority + core pinning + spin-loop
extern std::atomic<int>  g_samplerRunMode;   // default 1
extern std::atomic<bool> g_samplerRebind;

// After setupThread() runs:
//   >= 0 : sampler is pinned to this core (CPUBound mode)
//   -2   : running in Low/Normal mode (no core pinning)
//   -1   : not yet initialised (initial value — main.cpp waits on this)
extern std::atomic<int> g_samplerBoundCore;

#endif // !SAMPLER_HPP
