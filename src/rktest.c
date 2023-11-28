/*
------------------------------------------------------------------------------
Public Domain (www.unlicense.org)
This is free and unencumbered software released into the public domain.
Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.
In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain. We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors. We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
------------------------------------------------------------------------------
*/
#include "rktest/rktest.h"

#include <memory.h>
#include <stdio.h>
#include <string.h>

#ifdef WIN32
#include <windows.h>
#endif

/* RK Test runner internals ------------------------------------------------- */
#define RKTEST_MAX_NUM_TESTS (RKTEST_MAX_NUM_TEST_SUITES * RKTEST_MAX_NUM_TESTS_PER_SUITE)

#define foreach(type_ptr, iter, array, array_len) \
	for (type_ptr iter = &array[0]; iter != &array[array_len]; iter++)

typedef enum {
	RKTEST_RESULT_OK,
	RKTEST_RESULT_ERROR,
} rktest_result_t;

typedef struct {
	const char* name;
	rktest_test_t tests[RKTEST_MAX_NUM_TESTS_PER_SUITE];
	size_t num_tests;
} rktest_suite_t;

typedef struct {
	rktest_suite_t test_suites[RKTEST_MAX_NUM_TEST_SUITES];
	size_t num_test_suites;
	size_t total_num_tests;
} rktest_environment_t;

typedef struct {
	size_t num_passed_tests;
	rktest_test_t failed_tests[RKTEST_MAX_NUM_TESTS];
	size_t num_failed_tests;
} rktest_report_t;

// Unit test case storage
//
// An `rktest` data section is added to the binary, and is used to store meta
// data about all unit test cases to be run. Three fragments are added: `begin`,
// `data`, and `end`. The names utilize that fragments are sorted and stored
// alphabetically in the binary.
//
// All unit test files adds local test data to the `rktest$data` fragment of the
// section with the TEST() macro, and the linker will then collect them into a
// single `rktest$data` fragment in the binary.
//
// This trick is based on the following article by Raymond Chen: "Using linker
// segments and __declspec(allocate(…)) to arrange data in a specific order"
// https://devblogs.microsoft.com/oldnewthing/20181107-00/?p=100155
//
// Example layout:
// +--------------+-----------------+-----------+
// | rktest$begin | test_data_begin | rktest.o |
// +--------------+-----------------+----------+
// |              | test_1_case_1   | test1.o  |
// | rktest$data  | test_1_case_2   | test1.o  |
// |              | test_2_case_1   | test2.o  |
// +--------------+-----------------+----------+
// | rktest$end   | test_data_end   | rktest.o |
// +--------------+-----------------+----------+
#pragma section("rktest$begin", read)
#pragma section("rktest$data", read)
#pragma section("rktest$end", read)

// Add `rktest_test_t` pointers to mark the begining and the end of the
// `rktest` memory section. Test cases are added to `rktest$data` using the
// TEST() macro.
__declspec(allocate("rktest$begin")) const rktest_test_t* const test_data_begin = NULL;
__declspec(allocate("rktest$end")) const rktest_test_t* const test_data_end = NULL;

static bool g_colors_enabled = false;
static bool g_current_test_failed = false;

bool rktest_colors_enabled(void) {
	return g_colors_enabled;
}

void rktest_fail_current_test(void) {
	g_current_test_failed = true;
}

#ifdef WIN32
static rktest_result_t enable_windows_virtual_terminal(void) {
	// Set output mode to handle virtual terminal sequences
	HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
	if (hOut == INVALID_HANDLE_VALUE) {
		return RKTEST_RESULT_ERROR;
	}

	DWORD dwMode = 0;
	if (!GetConsoleMode(hOut, &dwMode)) {
		return RKTEST_RESULT_ERROR;
	}

	dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
	if (!SetConsoleMode(hOut, dwMode)) {
		return RKTEST_RESULT_ERROR;
	}

	return RKTEST_RESULT_OK;
}
#endif // WIN32

static void initialize(void) {
	g_colors_enabled = true;
#ifdef WIN32
	if (enable_windows_virtual_terminal() != RKTEST_RESULT_OK) {
		fprintf(stderr, "Error: could not initialize color output\n");
		g_colors_enabled = false;
	}
#endif // WIN32
}

static const rktest_test_t* const* skip_until_next_test(const rktest_test_t* const* it) {
	do {
		it++;
	} while (it != &test_data_end && *it == NULL);
	return it;
}

static rktest_suite_t* find_suite_with_name(rktest_suite_t* suites, size_t num_suites, const char* suite_name) {
	foreach (rktest_suite_t*, suite, suites, num_suites) {
		if (strcmp(suite->name, suite_name) == 0) {
			return suite;
		}
	}
	return NULL;
}

static rktest_suite_t* add_new_suite(rktest_environment_t* env, const char* suite_name) {
	rktest_suite_t* suite = &env->test_suites[env->num_test_suites++];
	*suite = (rktest_suite_t) {
		.name = suite_name,
		.num_tests = 0,
		.tests = { 0 },
	};
	return suite;
}

static rktest_suite_t* find_or_add_suite(rktest_environment_t* env, const char* suite_name) {
	rktest_suite_t* suite = find_suite_with_name(env->test_suites, env->num_test_suites, suite_name);
	suite = suite ? suite : add_new_suite(env, suite_name);
	return suite;
}

// Loop through the entirety of the `rkdata` memory section, including padding.
// If the iterator `it` points to null, it's padding and we skip it.
// If it's non-null, we have a test and push it into `tests`.
static rktest_environment_t* setup_test_env(void) {
	rktest_environment_t* env = malloc(sizeof(rktest_environment_t));
	*env = (rktest_environment_t) {
		.test_suites = { 0 },
		.num_test_suites = 0,
		.total_num_tests = 0,
	};

	for (const rktest_test_t* const* it = skip_until_next_test(&test_data_begin + 1); it != &test_data_end; it = skip_until_next_test(it)) {
		const rktest_test_t* const test = *it;

		if (env->num_test_suites == RKTEST_MAX_NUM_TEST_SUITES) {
			fprintf(stderr, "Error: number of test suites is greater than RKTEST_MAX_NUM_TEST_SUITES (%zu)."
							"See the `Config variables` section of rktest.h\n",
					RKTEST_MAX_NUM_TEST_SUITES);
			exit(1);
		}

		rktest_suite_t* suite = find_or_add_suite(env, test->suite_name);

		if (suite->num_tests == RKTEST_MAX_NUM_TESTS_PER_SUITE) {
			fprintf(stderr, "Error: number of tests in suite %s is greater than RKTEST_MAX_NUM_TESTS_PER_SUITE (%zu)."
							"See the `Config variables` section of rktest.h\n",
					suite->name,
					RKTEST_MAX_NUM_TESTS_PER_SUITE);
			exit(1);
		}

		// add test to suite
		env->total_num_tests++;
		suite->tests[suite->num_tests++] = *test;
	}

	// return env;
	return env;
}

static bool run_test(const rktest_test_t* test) {
	rktest_log_info("[ RUN      ] ", "%s.%s \n", test->suite_name, test->test_name);
	if (test->func) {
		test->func();
	}

	const bool test_passed = !g_current_test_failed;
	g_current_test_failed = false;

	if (test_passed) {
		rktest_log_info("[       OK ] ", "%s.%s (xx ms)\n", test->suite_name, test->test_name);
	} else {
		rktest_log_error("[  FAILED  ] ", "%s.%s (xx ms)\n", test->suite_name, test->test_name);
	}

	return test_passed;
}

static rktest_report_t* run_all_tests(rktest_environment_t* env) {
	rktest_report_t* report = malloc(sizeof(rktest_report_t));
	*report = (rktest_report_t) {
		.failed_tests = { 0 },
		.num_failed_tests = 0,
		.num_passed_tests = 0,
	};

	foreach (rktest_suite_t*, suite, env->test_suites, env->num_test_suites) {
		rktest_log_info("[----------] ", "%zu tests from %s\n", suite->num_tests, suite->name);
		foreach (rktest_test_t*, test, suite->tests, suite->num_tests) {
			const bool test_passed = run_test(test);
			if (test_passed) {
				report->num_passed_tests++;
			} else {
				report->failed_tests[report->num_failed_tests] = *test;
				report->num_failed_tests++;
			}
		}
		rktest_log_info("[----------] ", "%zu tests from %s (xx ms total)\n", suite->num_tests, suite->name);
		printf("\n");
	}

	return report;
}

static void print_failed_tests(rktest_report_t* report) {
	rktest_log_error("[  FAILED  ] ", "%zu tests, listed below:\n", report->num_failed_tests);
	foreach (const rktest_test_t*, failed_test, report->failed_tests, report->num_failed_tests) {
		rktest_log_error("[  FAILED  ] ", "%s.%s\n", failed_test->suite_name, failed_test->test_name);
	}
	printf("\n");
	printf(" %zu FAILED TESTS\n", report->num_failed_tests);
}

int rktest_main(void) {
	initialize();

	rktest_environment_t* env = setup_test_env();

	rktest_log_info("[==========] ", "Running %zu tests from %zu test suites.\n", env->total_num_tests, env->num_test_suites);
	rktest_log_info("[----------] ", "Global test environment set-up.\n");

	rktest_report_t* report = run_all_tests(env);

	rktest_log_info("[----------] ", "Global test environment tear-down.\n");
	rktest_log_info("[==========] ", "%zu tests from %zu test suites ran. (xx ms total)\n", env->total_num_tests, env->num_test_suites);
	rktest_log_info("[  PASSED  ] ", "%zu tests.\n", report->num_passed_tests);

	const bool tests_passed = report->num_failed_tests == 0;
	if (!tests_passed) {
		print_failed_tests(report);
	}

	free(report);
	free(env);

	return tests_passed;
}
