#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "refs.h"

static void ref_path(const char *ref_name, char *path, size_t sz)
{
	snprintf(path, sz, ".git/%s", ref_name);
}

/*
 * 현재는 loose ref(.git/refs/...)만 읽는다.
 * .git/packed-refs는 아직 보지 않으므로, 외부 git이 pack-refs로 정리한 레포의
 * ref는 누락될 수 있음.
 *
 * TODO: 현재 구현체는 항상 loose ref만 쓰므로 문제없지만,
 *       merge 기능 구현 이후 packed-refs 읽기 지원 추가 여부 검토.
 */
int ref_read(const char *ref_name, char *hex_out)
{
	char path[256];
	ref_path(ref_name, path, sizeof(path));

	FILE *f = fopen(path, "r");
	if (!f) {
		return -1;
	}
	int r = fscanf(f, "%40s", hex_out);
	fclose(f);

	return (r == 1) ? 0 : -1;
}

int ref_write(const char *ref_name, const char *hex)
{
	char path[256];
	ref_path(ref_name, path, sizeof(path));

	FILE *f = fopen(path, "w");
	if (!f) {
		return -1;
	}
	int r = fprintf(f, "%s\n", hex);
	fclose(f);

	return (r > 0) ? 0 : -1;
}

int head_read(char *hex_out)
{
	FILE *f = fopen(".git/HEAD", "r");
	if (!f) {
		return -1;
	}

	char line[256];
	if (!fgets(line, sizeof(line), f)) {
		// 빈 HEAD 파일 읽기 실패 처리
		fclose(f);
		return -1;
	}
	fclose(f);

	line[strcspn(line, "\n")] = '\0'; // 줄 끝 제거

	if (strncmp(line, "ref: ", 5) == 0) {
		// 심볼릭 참조 추적
		return ref_read(line + 5, hex_out);
	}

	/* detached HEAD: 커밋 SHA 복사 + NUL 종료 보장 */
	strncpy(hex_out, line, 40);
	hex_out[40] = '\0'; // 총 41바이트(40 + NUL) 보장
	return 0;
}

int head_branch(char *branch_out, size_t branch_out_size)
{
	FILE *f = fopen(".git/HEAD", "r");
	if (!f) {
		return -1;
	}

	char line[256];
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return -1;
	}
	line[strcspn(line, "\n")] = '\0';

	if (strncmp(line, "ref: refs/heads/", 16) == 0) {
		// truncate 시에도 NUL 종료 보장
		strncpy(branch_out, line + 16, branch_out_size - 1);
		branch_out[branch_out_size - 1] = '\0';
		fclose(f);
		return 0;
	}
	fclose(f);
	return -1; // detached HEAD인 경우
}

int head_set_branch(const char *branch)
{
	FILE *f = fopen(".git/HEAD", "w");
	if (!f) {
		return -1;
	}
	fprintf(f, "ref: refs/heads/%s\n", branch);
	if (fclose(f) != 0) {
		return -1;
	}
	return 0;
}

/*
 * HEAD를 detached 상태로 설정
 * symbolic ref 없이 커밋 해시의 SHA를 직접 기록함.
 */
int head_set_detached(const char *hex)
{
	FILE *f = fopen(".git/HEAD", "w");
	if (!f) {
		return -1;
	}
	fprintf(f, "%s\n", hex);
	if (fclose(f) != 0) {
		return -1;
	}
	return 0;
}

int branch_create(const char *name, const char *start_hex)
{
	char ref_name[256];
	snprintf(ref_name, sizeof(ref_name), "refs/heads/%s", name);
	/*
	 * TODO: 중첩 브랜치(예: "topic/x") 생성은 아직 미지원.
	 * 현재는 ref_write가 부모 디렉터리를 만들지 않아 .git/refs/heads/topic이
	 * 없으면 fopen이 실패함.
	 */
	return ref_write(ref_name, start_hex);
}

/*
 * refs/heads 아래를 재귀적으로 탐색해 브랜치 이름을 수집.
 *
 * - dir: 실제 디스크 경로 (예: ".git/refs/heads/feature")
 * - prefix: refs/heads 기준 상대 경로 (예: "feature/"), 루트는 빈 문자열("")
 */
static int collect_branches(const char *dir, const char *prefix, char names[][128], int max_count,
                            int count)
{
	DIR *d = opendir(dir);
	if (!d) {
		return count;
	}

	struct dirent *ent;
	while (count < max_count && (ent = readdir(d)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
			continue;
		}

		char path[512], rel_name[128];
		snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
		snprintf(rel_name, sizeof(rel_name), "%s%s", prefix, ent->d_name);

		struct stat st;
		if (stat(path, &st) != 0) {
			continue;
		}

		if (S_ISDIR(st.st_mode)) {
			char subprefix[128];
			snprintf(subprefix, sizeof(subprefix), "%s/", rel_name);
			count = collect_branches(path, subprefix, names, max_count, count);
		} else if (S_ISREG(st.st_mode)) {
			strncpy(names[count], rel_name, 128);
			names[count][127] = '\0';
			count++;
		}
	}

	closedir(d);
	return count;
}

static int name_cmp(const void *a, const void *b)
{
	return strcmp((const char *)a, (const char *)b);
}

/*
 * 한계: .git/refs/heads/ 아래의 loose ref만 나열한다. .git/packed-refs에만
 *       존재하는 브랜치는 누락된다. (자체 생성 레포는 loose ref만 쓰므로 문제없음)
 * TODO: merge 기능 구현 이후 packed-refs 읽기 지원 추가 여부 검토.
 */
int branch_list(char names[][128], int max_count)
{
	int count = collect_branches(".git/refs/heads", "", names, max_count, 0);
	qsort(names, (size_t)count, 128, name_cmp);
	return count;
}
