"""Probe patch (never landed): the turn loop waits, after a turn that released writers, until as many
writers have opened records as it released, or STRATA_COHORT_US microseconds pass. Usage:
python3 cohort.py include/strata/core/turn_seal.hh"""

import sys

p = sys.argv[1]
s = open(p).read()


def sub(old: str, new: str) -> None:
    global s
    if old not in s:
        sys.exit(f"cohort.py: anchor not found: {old[:60]!r}")
    s = s.replace(old, new, 1)


sub("#include <condition_variable>", "#include <chrono>\n#include <cstdlib>\n#include <condition_variable>")
# wake(): count arrivals
sub(
    """		turns_.wake();
		const std::lock_guard lock(mu_);
		woken_ = true;
		cv_.notify_all();
	}""",
    """		turns_.wake();
		const std::lock_guard lock(mu_);
		woken_ = true;
		++arrivals_;
		cv_.notify_all();
	}""",
)
# turn(): record the cohort it releases
sub(
    """		const std::lock_guard lock(mu_);
		for (const auto& [group, e] : groups) {
			if (auto it = groups_.find(group); it != groups_.end() && it->second.done == e.done) { it->second.durable = e.durable; }
		}
		cv_.notify_all();
		return true;""",
    """		const std::lock_guard lock(mu_);
		for (const auto& [group, e] : groups) {
			if (auto it = groups_.find(group); it != groups_.end() && it->second.done == e.done) { it->second.durable = e.durable; }
		}
		cohort_ = waiting_;
		arrivals_ = 0;
		cv_.notify_all();
		return true;""",
)
sub(
    """		for (;;) {
			while (turn()) {}
			std::unique_lock lock(mu_);""",
    """		static const long cohort_us = std::getenv("STRATA_COHORT_US") ? std::atol(std::getenv("STRATA_COHORT_US")) : 0;
		for (;;) {
			while (turn()) {
				if (!cohort_us) { continue; }
				std::unique_lock lock(mu_);
				cv_.wait_for(lock, std::chrono::microseconds(cohort_us), [&] { return stopping_ || arrivals_ >= cohort_; });
			}
			std::unique_lock lock(mu_);""",
)
sub("	std::size_t waiting_ = 0;", "	std::size_t waiting_ = 0, cohort_ = 0, arrivals_ = 0;")
open(p, "w").write(s)
print("cohort.py: patched", p)
