#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "refs.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
static char original_cwd[PATH_MAX];
static char tmpdir[] = "/tmp/git-client-refs-test.XXXXXX";

static const char SHA_A[] = "1111111111111111111111111111111111111111";
static const char SHA_B[] = "2222222222222222222222222222222222222222";

static void expect_int(const char *name, int got, int want)
{
	if (got == want)
		return;

	fprintf(stderr, "FAIL %s\n  got:  %d\n  want: %d\n", name, got, want);
	failures++;
}

static void expect_string(const char *name, const char *got, const char *want)
{
	if (strcmp(got, want) == 0)
		return;

	fprintf(stderr, "FAIL %s\n  got:  %s\n  want: %s\n", name, got, want);
	failures++;
}

static int mkdir_if_missing(const char *path)
{
	if (mkdir(path, 0755) == 0 || errno == EEXIST)
		return 0;
	return -1;
}

static void setup_repo(void)
{
	if (!mkdtemp(tmpdir)) {
		perror("mkdtemp");
		exit(EXIT_FAILURE);
	}

	if (!getcwd(original_cwd, sizeof(original_cwd))) {
		perror("getcwd");
		exit(EXIT_FAILURE);
	}

	if (chdir(tmpdir) != 0 || mkdir_if_missing(".git") != 0 ||
	    mkdir_if_missing(".git/refs") != 0 || mkdir_if_missing(".git/refs/heads") != 0) {
		perror("setup repo");
		exit(EXIT_FAILURE);
	}
}

static void cleanup_repo(void)
{
	if (original_cwd[0] != '\0' && chdir(original_cwd) != 0)
		perror("chdir original cwd");
}

static void write_head(const char *contents)
{
	FILE *f = fopen(".git/HEAD", "w");

	if (!f) {
		perror("write_head");
		exit(EXIT_FAILURE);
	}
	fputs(contents, f);
	fclose(f);
}

/* ref_write then ref_read returns the same SHA. */
static void test_ref_write_read_round_trip(void)
{
	char hex[41] = {0};

	expect_int("ref_write stores a branch ref", ref_write("refs/heads/main", SHA_A), 0);
	expect_int("ref_read reads the stored ref", ref_read("refs/heads/main", hex), 0);
	expect_string("ref_read returns the written SHA", hex, SHA_A);
}

/* ref_read on a nonexistent ref fails rather than returning stale data. */
static void test_ref_read_rejects_missing(void)
{
	char hex[41] = {0};

	expect_int("ref_read rejects a missing ref", ref_read("refs/heads/nope", hex), -1);
}

/* head_set_branch writes a symbolic ref that head_branch reads back. */
static void test_head_set_branch_and_branch_name(void)
{
	char branch[128] = {0};

	expect_int("head_set_branch points HEAD at a branch", head_set_branch("main"), 0);
	expect_int("head_branch reads the branch name", head_branch(branch, sizeof(branch)), 0);
	expect_string("head_branch strips refs/heads/ prefix", branch, "main");
}

/* head_read follows HEAD -> refs/heads/main -> commit SHA. */
static void test_head_read_follows_symbolic_ref(void)
{
	char hex[41] = {0};

	expect_int("ref_write seeds the branch tip", ref_write("refs/heads/main", SHA_A), 0);
	expect_int("head_set_branch points HEAD at the branch", head_set_branch("main"), 0);
	expect_int("head_read resolves through the symbolic ref", head_read(hex), 0);
	expect_string("head_read returns the branch tip SHA", hex, SHA_A);
}

/* A detached HEAD stores a raw SHA: head_read returns it, head_branch fails. */
static void test_head_detached(void)
{
	char hex[41] = {0};
	char branch[128] = {0};

	expect_int("head_set_detached writes a raw SHA", head_set_detached(SHA_B), 0);
	expect_int("head_read reads the detached SHA", head_read(hex), 0);
	expect_string("head_read returns the detached SHA", hex, SHA_B);
	expect_int("head_branch reports no branch when detached",
	           head_branch(branch, sizeof(branch)), -1);
}

/* branch_create writes refs/heads/<name> with the start SHA. */
static void test_branch_create(void)
{
	char hex[41] = {0};

	expect_int("branch_create makes a new branch", branch_create("feature", SHA_B), 0);
	expect_int("ref_read finds the created branch", ref_read("refs/heads/feature", hex), 0);
	expect_string("branch_create stores the start SHA", hex, SHA_B);
}

/*
 * branch_list returns every head, including nested ones, sorted by name.
 * branch_create only writes a single ref file (it does not create parent
 * directories), so the nested ref is laid out directly to exercise the
 * recursive listing.
 */
static void test_branch_list_sorted_and_nested(void)
{
	char names[16][128];
	int n;

	expect_int("seed branch main", branch_create("main", SHA_A), 0);
	expect_int("seed branch feature", branch_create("feature", SHA_B), 0);
	if (mkdir_if_missing(".git/refs/heads/topic") != 0) {
		perror("mkdir nested ref dir");
		exit(EXIT_FAILURE);
	}
	expect_int("seed nested branch", ref_write("refs/heads/topic/x", SHA_A), 0);

	n = branch_list(names, 16);
	expect_int("branch_list counts every head", n, 3);
	expect_string("branch_list sorts first by name", names[0], "feature");
	expect_string("branch_list sorts second by name", names[1], "main");
	expect_string("branch_list includes nested heads", names[2], "topic/x");
}

/* head_read fails when HEAD points at a branch that has no ref file yet. */
static void test_head_read_unborn_branch(void)
{
	char hex[41] = {0};

	write_head("ref: refs/heads/unborn\n");
	expect_int("head_read fails before the first commit", head_read(hex), -1);
}

int main(void)
{
	setup_repo();

	test_ref_write_read_round_trip();
	test_ref_read_rejects_missing();
	test_head_set_branch_and_branch_name();
	test_head_read_follows_symbolic_ref();
	test_head_detached();
	test_branch_create();
	test_branch_list_sorted_and_nested();
	test_head_read_unborn_branch();

	cleanup_repo();

	if (failures != 0) {
		fprintf(stderr, "%d refs test(s) failed\n", failures);
		return EXIT_FAILURE;
	}

	puts("refs tests passed");
	return EXIT_SUCCESS;
}
