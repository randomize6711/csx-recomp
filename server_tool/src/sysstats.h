// A process's CPU and memory use, measured by the operating system (Linux /proc, Windows process API). Nothing is
// estimated: a value that cannot be read stays unset and the UI shows "n/a".
#pragma once

#include <optional>

namespace tool {

class ProcessMeter {
public:
    // sample now; call about once a second
    void Sample(long long pid);
    std::optional<double> cpu_percent;  // of one core (100 = one core busy)
    std::optional<double> memory_mb;    // resident set
    std::optional<int> threads;

private:
    long long pid_ = 0;
    double last_cpu_s_ = -1, last_wall_s_ = 0;
};

}  // namespace tool
