#ifndef AMC_TEST_SUPPORT_H_
#define AMC_TEST_SUPPORT_H_

/*
 * TestSupport.h — shared helpers for the AmcLogger documentation test suite.
 *
 * The chapters (Test01..Test09) are written to be read in order: together they
 * are the user manual, in executable form. Helpers here keep each chapter
 * focused on behavior instead of plumbing:
 *
 *   - capture_stdout()/release_stdout(): temporarily redirect a standard
 *     stream to a file so tests can assert on what a user would see.
 *     Discipline: always release before asserting, so Unity failure output
 *     stays visible on the console.
 *   - amc_assert_log_line(): asserts one full log line — it validates the
 *     timestamp *shape* ([YYYY-MM-DD HH:MM:SS.ssssss]) and exact-matches
 *     every other field. This helper doubles as the format specification.
 *   - amc_internal_test_reset(): test-only hook (library built with
 *     AMC_LOGGER_TESTING) that tears the logger down to its pre-init state,
 *     letting one test binary simulate many independent process lifetimes.
 *     In production, init/shutdown happen once per process.
 */

#include "AmcLogger.h"
#include "unity.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void amc_internal_test_reset(void);   /* provided by the AMC_LOGGER_TESTING build */

/* ---------- file helpers ---------- */

static inline char *amc_read_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (buf && sz > 0 && fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        buf = NULL;
    }
    if (buf)
        buf[sz] = '\0';
    fclose(fp);
    return buf;
}

static inline void amc_write_file(const char *path, const char *content)
{
    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL_MESSAGE(fp, "cannot create test file");
    fputs(content, fp);
    fclose(fp);
}

static inline int amc_count_lines(const char *text)
{
    int n = 0;
    for (const char *p = text; *p; p++)
        if (*p == '\n')
            n++;
    return n;
}

/* copy the idx-th line (0-based, without '\n') into out; NULL if absent */
static inline char *amc_get_line(const char *text, int idx, char *out, size_t outsz)
{
    const char *p = text;
    while (idx > 0 && p) {
        p = strchr(p, '\n');
        if (p)
            p++;
        idx--;
    }
    if (!p || !*p)
        return NULL;
    const char *end = strchr(p, '\n');
    size_t n = end ? (size_t)(end - p) : strlen(p);
    if (n >= outsz)
        n = outsz - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return out;
}

static inline int amc_count_lines_containing(const char *text, const char *needle)
{
    int n = 0;
    const char *p = text;
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p++;
    }
    return n;
}

/* application lines only: total minus the logger's own MessageLoss summaries */
static inline int amc_count_app_lines(const char *text)
{
    return amc_count_lines(text) - amc_count_lines_containing(text, "[AmcLogger]");
}

/* ---------- stream capture ---------- */

static int amc_saved_stdout_fd_ = -1;
static int amc_saved_stderr_fd_ = -1;

static inline void amc_capture_(FILE *stream, const char *path, int *saved)
{
    fflush(stream);
    *saved = dup(fileno(stream));
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    dup2(fd, fileno(stream));
    close(fd);
}

static inline void amc_release_(FILE *stream, int *saved)
{
    if (*saved < 0)
        return;
    fflush(stream);
    dup2(*saved, fileno(stream));
    close(*saved);
    *saved = -1;
}

static inline void capture_stdout(void)
{
    amc_capture_(stdout, "cap_stdout.txt", &amc_saved_stdout_fd_);
}

/* returns the captured text (malloc'd; free() it) */
static inline char *release_stdout(void)
{
    amc_release_(stdout, &amc_saved_stdout_fd_);
    return amc_read_file("cap_stdout.txt");
}

static inline void capture_stderr(void)
{
    amc_capture_(stderr, "cap_stderr.txt", &amc_saved_stderr_fd_);
}

static inline char *release_stderr(void)
{
    amc_release_(stderr, &amc_saved_stderr_fd_);
    return amc_read_file("cap_stderr.txt");
}

/* safety net for tests that fail while a stream is redirected */
static inline void amc_release_all_streams(void)
{
    amc_release_(stdout, &amc_saved_stdout_fd_);
    amc_release_(stderr, &amc_saved_stderr_fd_);
}

/* ---------- the line-format assertion (the format spec, executable) ----------
 *
 *   [YYYY-MM-DD HH:MM:SS.ssssss][LEVEL][MODULE][FUNCTION][EVENT][TRADER_ID]PAYLOAD
 *
 * No spaces between brackets, no space before the payload, microseconds are
 * always 6 digits, TRADER_ID is `-` when the non-_ID macro was used.
 */
static inline void amc_assert_log_line(const char *line,
                                       const char *level, const char *module,
                                       const char *function, const char *event,
                                       const char *id, const char *payload)
{
    TEST_ASSERT_NOT_NULL_MESSAGE(line, "expected a log line");
    TEST_ASSERT_EQUAL_CHAR('[', line[0]);

    static const char shape[] = "dddd-dd-dd dd:dd:dd.dddddd"; /* 26 chars */
    for (int i = 0; i < 26; i++) {
        char c = line[1 + i];
        if (shape[i] == 'd')
            TEST_ASSERT_TRUE_MESSAGE(isdigit((unsigned char)c),
                                     "timestamp digit expected");
        else
            TEST_ASSERT_EQUAL_CHAR(shape[i], c);
    }
    TEST_ASSERT_EQUAL_CHAR(']', line[27]);

    char expect[1024];
    snprintf(expect, sizeof(expect), "[%s][%s][%s][%s][%s]%s",
             level, module, function, event, id, payload);
    TEST_ASSERT_EQUAL_STRING(expect, line + 28);
}

#endif /* AMC_TEST_SUPPORT_H_ */
