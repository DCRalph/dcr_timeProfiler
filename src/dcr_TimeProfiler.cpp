#include "dcr_TimeProfiler.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <set>
#include <dcr_Logger.h>
#include <dcr_taskManager/FreeRtosRaii.h>
#include <dcr_taskManager/MutexRegistry.h>
#include <dcr_TaskManager.h>
#include <dcr_FatalHandler.h>

#undef LOG_TAG
#define LOG_TAG "TPROF"

// Static task handle
static TaskHandle_t callCounterResetTaskHandle = NULL;

static FreeRtosRaii::Mutex &profilerMutex()
{
  static FreeRtosRaii::Mutex m{FreeRtosRaii::DeferredCreate};
  return m;
}

// Static task function for call counter resets
static void callCounterResetTask(void *parameter)
{
  auto *profiler = static_cast<TimeProfiler *>(parameter);
  FatalHandler::runGuardedTask("TimeProfilerCallReset", [profiler]()
                               {
    for (;;)
    {
      taskManager.noteHeartbeat();
      TickType_t delayTicks = portTICK_PERIOD_MS > 0 ? (1000 / portTICK_PERIOD_MS) : 1;
      if (delayTicks == 0)
        delayTicks = 1;
      vTaskDelay(delayTicks);
      profiler->resetCallCounters();
    } });
}

void TimeProfiler::begin(uint32_t stackSize, UBaseType_t priority, BaseType_t core)
{
  if (active)
    return;

  if (!profilerMutex().ensureCreated())
  {
    debugE("TimeProfiler: mutex create failed");
    return;
  }
  RtosUtils::registerMutex(profilerMutex(), "timeProfiler");

  if (!taskManager.createTaskPinnedToCore(callCounterResetTask,
                                          "TimeProfilerCallReset",
                                          stackSize,
                                          this,
                                          priority,
                                          &callCounterResetTaskHandle,
                                          core))
  {
    callCounterResetTaskHandle = NULL;
    debugE("Failed to create TimeProfilerCallReset task");
    return;
  }

  active = true;
}

void TimeProfiler::start(const char *key, TimeUnit unit)
{
  if (!active)
    return;

  if (auto lock = FreeRtosRaii::tryLock(profilerMutex(), portMAX_DELAY))
  {
    ProfilerData &data = profilerData[key];
    data.startUnit = unit;
    data.hasStartTime = true;
    if (unit == TimeUnit::MICROSECONDS)
    {
      data.startTime = micros();
    }
    else
    {
      data.startTime = millis();
    }
  }
}

void TimeProfiler::stop(const char *key)
{
  if (!active)
    return;

  if (auto lock = FreeRtosRaii::tryLock(profilerMutex(), portMAX_DELAY))
  {
    auto it = profilerData.find(key);
    if (it != profilerData.end() && it->second.hasStartTime)
    {
      ProfilerData &data = it->second;

      if (data.startUnit == TimeUnit::MICROSECONDS)
      {
        data.elapsed.time = micros() - data.startTime;
      }
      else
      {
        data.elapsed.time = millis() - data.startTime;
      }

      data.elapsed.unit = data.startUnit;
      data.hasElapsedTime = true;
      data.hasStartTime = false; // Clear start time
    }
  }
}

uint32_t TimeProfiler::getTime(const char *key)
{
  uint32_t result = 0;
  if (!active)
    return result;

  if (auto lock = FreeRtosRaii::tryLock(profilerMutex(), portMAX_DELAY))
  {
    auto it = profilerData.find(key);
    if (it != profilerData.end() && it->second.hasElapsedTime)
    {
      result = it->second.elapsed.time;
    }
  }
  return result;
}

TimeUnit TimeProfiler::getTimeUnit(const char *key)
{
  TimeUnit result = TimeUnit::MICROSECONDS; // Default fallback
  if (!active)
    return result;

  if (auto lock = FreeRtosRaii::tryLock(profilerMutex(), portMAX_DELAY))
  {
    auto it = profilerData.find(key);
    if (it != profilerData.end() && it->second.hasElapsedTime)
    {
      result = it->second.elapsed.unit;
    }
  }
  return result;
}

uint32_t TimeProfiler::getTimeUs(const char *key)
{
  uint32_t result = 0;
  if (!active)
    return result;

  if (auto lock = FreeRtosRaii::tryLock(profilerMutex(), portMAX_DELAY))
  {
    auto it = profilerData.find(key);
    if (it != profilerData.end() && it->second.hasElapsedTime)
    {
      if (it->second.elapsed.unit == TimeUnit::MICROSECONDS)
      {
        result = it->second.elapsed.time;
      }
      else
      {
        result = it->second.elapsed.time * 1000; // Convert ms to us
      }
    }
  }
  return result;
}

float TimeProfiler::getTimeMs(const char *key)
{
  float result = 0.0f;
  if (!active)
    return result;

  if (auto lock = FreeRtosRaii::tryLock(profilerMutex(), portMAX_DELAY))
  {
    auto it = profilerData.find(key);
    if (it != profilerData.end() && it->second.hasElapsedTime)
    {
      if (it->second.elapsed.unit == TimeUnit::MILLISECONDS)
      {
        result = (float)it->second.elapsed.time;
      }
      else
      {
        result = it->second.elapsed.time / 1000.0f; // Convert us to ms
      }
    }
  }
  return result;
}

// === CALL COUNTING SYSTEM METHODS ===
void TimeProfiler::increment(const char *key)
{
  if (!active)
    return;

  if (auto lock = FreeRtosRaii::tryLock(profilerMutex(), portMAX_DELAY))
  {
    profilerData[key].currentCount++;
  }
}

uint32_t TimeProfiler::getCallsPerSecond(const char *key)
{
  uint32_t result = 0;
  if (!active)
    return result;

  if (auto lock = FreeRtosRaii::tryLock(profilerMutex(), portMAX_DELAY))
  {
    auto it = profilerData.find(key);
    if (it != profilerData.end())
    {
      result = it->second.perSecond;
    }
  }
  return result;
}

uint32_t TimeProfiler::getCurrentCallCount(const char *key)
{
  uint32_t result = 0;
  if (!active)
    return result;

  if (auto lock = FreeRtosRaii::tryLock(profilerMutex(), portMAX_DELAY))
  {
    auto it = profilerData.find(key);
    if (it != profilerData.end())
    {
      result = it->second.currentCount;
    }
  }
  return result;
}

void TimeProfiler::resetCallCounters()
{
  if (!active)
    return;

  if (auto lock = FreeRtosRaii::tryLock(profilerMutex(), portMAX_DELAY))
  {
    // Copy current call counts to calls per second and reset current counts
    for (auto &pair : profilerData)
    {
      pair.second.perSecond = pair.second.currentCount;
      pair.second.currentCount = 0;
    }
  }
}

// === SHARED UTILITIES ===
bool TimeProfiler::hasKey(const char *key)
{
  bool result = false;
  if (!active)
    return result;

  if (auto lock = FreeRtosRaii::tryLock(profilerMutex(), portMAX_DELAY))
  {
    result = profilerData.find(key) != profilerData.end();
  }
  return result;
}

void TimeProfiler::clear()
{
  if (!active)
    return;

  if (auto lock = FreeRtosRaii::tryLock(profilerMutex(), portMAX_DELAY))
  {
    profilerData.clear();
  }
}

void TimeProfiler::remove(const char *key)
{
  if (!active)
    return;

  if (auto lock = FreeRtosRaii::tryLock(profilerMutex(), portMAX_DELAY))
  {
    profilerData.erase(key);
  }
}

String TimeProfiler::formatAll()
{
  if (!active)
    return String("=== TimeProfiler (disabled - call timeProfiler.begin()) ===\n");

  String buffer;
  buffer.reserve(2048); // Pre-allocate buffer to reduce reallocations

  buffer += "=== TimeProfiler Results ===\n";

  if (auto lock = FreeRtosRaii::tryLock(profilerMutex(), portMAX_DELAY))
  {
    for (const auto &pair : profilerData)
    {
      const char *key = pair.first;
      const ProfilerData &data = pair.second;

      buffer += key;
      buffer += ": ";

      // Print timing data if available
      if (data.hasElapsedTime)
      {
        buffer += String(data.elapsed.time);
        if (data.elapsed.unit == TimeUnit::MICROSECONDS)
        {
          buffer += " us (";
          buffer += String(data.elapsed.time / 1000.0f);
          buffer += " ms)";
        }
        else
        {
          buffer += " ms (";
          buffer += String(data.elapsed.time * 1000);
          buffer += " us)";
        }
      }
      else
      {
        buffer += "no timing data";
      }

      // Print call data if available
      if (data.perSecond > 0 || data.currentCount > 0)
      {
        buffer += " - ";
        buffer += String(data.perSecond);
        buffer += " calls/sec";
        if (data.currentCount > 0)
        {
          buffer += " (current: ";
          buffer += String(data.currentCount);
          buffer += ")";
        }
      }

      buffer += "\n";
    }
  }

  buffer += "=============================\n";

  return buffer;
}

void TimeProfiler::printAll()
{
  if (!active)
    return;

  debugI("%s", formatAll().c_str());
}

// Global instance
TimeProfiler timeProfiler;