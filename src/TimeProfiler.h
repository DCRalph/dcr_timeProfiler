#pragma once

#include <Arduino.h>
#include <logger/Logger.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <map>

namespace TimeProfilerInternal
{
  struct CStringLess
  {
    bool operator()(const char *a, const char *b) const
    {
      return std::strcmp(a, b) < 0;
    }
  };
}

enum class TimeUnit
{
  MICROSECONDS,
  MILLISECONDS
};

struct TimingData
{
  uint32_t time;
  TimeUnit unit;
};

struct ProfilerData
{
  // Start timing (only set when timing is active)
  uint32_t startTime;
  TimeUnit startUnit;
  bool hasStartTime;

  // Elapsed timing (only set when timing is complete)
  TimingData elapsed;
  bool hasElapsedTime;

  // Call counting
  uint32_t currentCount;
  uint32_t perSecond;

  ProfilerData() : startTime(0), startUnit(TimeUnit::MICROSECONDS),
                   hasStartTime(false), hasElapsedTime(false),
                   currentCount(0), perSecond(0) {}
};

class TimeProfiler
{
private:
  // Single unified map containing all profiling data (maximum memory efficiency)
  // Uses const char* keys to avoid String allocations
  std::map<const char *, ProfilerData, TimeProfilerInternal::CStringLess> profilerData;

  // Set true only after begin() successfully starts the background task
  bool active = false;

public:
  TimeProfiler() = default;

  void begin(uint32_t stackSize, UBaseType_t priority, BaseType_t core);
  void start(const char *key, TimeUnit unit = TimeUnit::MICROSECONDS);
  void stop(const char *key);
  uint32_t getTime(const char *key);
  TimeUnit getTimeUnit(const char *key);
  uint32_t getTimeUs(const char *key);
  float getTimeMs(const char *key);
  void increment(const char *key);
  uint32_t getCallsPerSecond(const char *key);
  uint32_t getCurrentCallCount(const char *key);
  bool hasKey(const char *key);
  void clear();
  void remove(const char *key);
  String formatAll();
  void printAll();
  void resetCallCounters();

};

extern TimeProfiler timeProfiler;