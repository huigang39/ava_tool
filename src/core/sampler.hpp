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

class Gui;

// Entry point for the sampler thread. Runs until g_appRunning is set to false.
void threadFunc(Gui *gui);

#endif // !SAMPLER_HPP
