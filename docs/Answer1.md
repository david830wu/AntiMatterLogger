
00. Our error handling philosophy is if a error can be detected, it must be reported as early as it can.

A. Decisions that shape the architecture

D1. keep both modes. async is default and global switch is acceptable.

D2. one worker thread is OK.

D3. no formatter objects at all. EVENT must then be a string literal

D4. just drop `_st` at all, focus on `_mt`

D5. auto-create for unlisted modules, and loggers live until shutdown

D6. use (b) hand-rolled strict subset parser

B. Semantics to pin down

Q7. enable_trader_id in configs can be droped. there is no real bound for trading system.

Q8. Payload isn't JSON. They are typo in example use and they have been fixed.

Q9. Empty payload. the line end right aftert {}

Q10. Timezone. utc_offset config key defaulting to +8

Q11. Maximum line length. max_message_size (default 2 KB) with an explicit truncation marker;

Q12. Lossless or lossy? keep default to block mode

Q13. CRITICAL vs crash. (a) as default plus (b) as opt-in.

Q14. $TODAY. Expanded once at file-open

C. Config and operations

Q15. Config gaps to settle. all OK

Q16. Bad or missing config file. Fail fast, according to our error hanlding philosophy

Q17. Runtime control. out of scope for v1, keep it as config file assigned.

D. Project baseline

Q18. Toolchain floor. Focus on Linux-only, x86-64, gcc-13. MacOS support is required as well.

Q19. Make "very fast" a number. In production ~ 10k messages/sec, ~10 producer threads. The performance can be comparable to spdlog is OK.

Q20. Build and QA. OK 

Q21. Naming. The repo is AntiMatterLogger, but is is integrated into a AntiMatterCommon repo, this is where "AMC" comes from. So the code says amc_/AMC_LOGGER_/AmcLogger.h. keep amc_. amc_logger_close_logger → amc_logger_shutdown is great. keep the naming clear and meaningful.

E. Improvements 

- Cache the rendered YYYY-MM-DD HH:MM:SS. seconds prefix and re-render only the microseconds — removes most timestamp-formatting cost.
- Worker drains the queue in batches with one buffered write per batch, and uses a condvar wait timeout subsumes flush_every() with no extra thread, resolving §3's "flush_every requires _mt" note.
- AMC_LOGGER_ACTIVE_LEVEL compile-time strip, so DEBUG calls vanish entirely from release builds.
- Drop/overflow counters plus a periodic "N messages dropped" summary line (essential honesty for lossytiny amc_logger_stats().
- Shutdown = drain queue, join worker, flush and close sinks; idempotent; post-shutdown calls are no-ops via the same OFF-level trick. Note "flush" throughout means fflush (userspace → kernel), not fsync — confirm that's the intent.
- Documented contracts: init is called before spawning threads (not itself thread-safe); logging from sted.
- Focus on a spdlog comparable performance, design a stable and safe logger system in this stage. 

F. Doc errata (fix when you found)
