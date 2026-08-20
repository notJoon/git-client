#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "object.h"
#include "index.h"
#include "refs.h"
#include "sha1.h"

/* ---- jg init ---- */
static int cmd_init(void) {
    mkdir(".git",              0755);
    mkdir(".git/objects",      0755);
    mkdir(".git/objects/info", 0755);
    mkdir(".git/objects/pack", 0755);
    mkdir(".git/refs",         0755);
    mkdir(".git/refs/heads",   0755);
    mkdir(".git/refs/tags",    0755);

    FILE *f = fopen(".git/HEAD", "w");
    fprintf(f, "ref: refs/heads/main\n");
    fclose(f);

    printf("Initialized empty jg repository in .git/\n");
    return 0;
}

/* ---- jg hash-object [-w] [--stdin | <file>] ---- */
static int cmd_hash_object(int argc, char **argv) {
    int write_flag = 0;
    const char *filepath = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0) write_flag = 1;
        else if (strcmp(argv[i], "--stdin") != 0) filepath = argv[i];
    }

    uint8_t *content = NULL;
    size_t   content_len = 0;

    if (filepath) {
        FILE *f = fopen(filepath, "rb");
        if (!f) { fprintf(stderr, "fatal: cannot open '%s'\n", filepath); return -1; }
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
        long sz = ftell(f);
        rewind(f);
        if (sz < 0) { fclose(f); return -1; }
        content_len = (size_t)sz;
        content = malloc(content_len ? content_len : 1);
        if (!content) { fclose(f); return -1; }
        if (fread(content, 1, content_len, f) != content_len) {
            free(content); fclose(f); return -1;
        }
        fclose(f);
    } else {
        /* --stdin: 버퍼를 동적으로 키우며 읽기 (64KB 고정 버퍼 오버플로 제거) */
        size_t cap = 65536;
        content = malloc(cap);
        if (!content) return -1;
        int c;
        while ((c = fgetc(stdin)) != EOF) {
            if (content_len == cap) {
                cap *= 2;
                uint8_t *tmp = realloc(content, cap);
                if (!tmp) { free(content); return -1; }
                content = tmp;
            }
            content[content_len++] = (uint8_t)c;
        }
    }

    uint8_t sha1[20];
    if (write_flag) {
        if (object_write(OBJ_BLOB, content, content_len, sha1) < 0) {
            free(content); return -1;
        }
    } else {
        /*
         * -w 없이도 실제 git과 동일한 해시를 내려면 "blob <len>\0" + content 를
         * 해시해야 함. (기존 코드의 "blob " 5바이트만으로는 size와 NUL이 빠져
         * git·object_write 어느 쪽과도 일치하지 않는 잘못된 해시가 나올 수 있음.)
         */
        char header[64];
        int  hlen = snprintf(header, sizeof(header), "blob %zu", content_len);
        if (hlen < 0 || (size_t)hlen >= sizeof(header)) { free(content); return -1; }
        sha1_compute2((uint8_t *)header, (size_t)hlen + 1,   /* NUL 포함 */
                      content, content_len, sha1);
    }

    char hex[41];
    sha1_to_hex(sha1, hex);
    printf("%s\n", hex);
    free(content);
    return 0;
}

/* ---- jg cat-file -p <hash> ---- */
static int cmd_cat_file(int argc, char **argv) {
    if (argc < 2) return -1;
    const char *hex = argv[1];

    char     type[8];
    uint8_t *data;
    size_t   data_len;

    if (object_read(hex, type, &data, &data_len) < 0) {
        fprintf(stderr, "fatal: Not a valid object: %s\n", hex);
        return -1;
    }
    fwrite(data, 1, data_len, stdout);
    free(data);
    return 0;
}

/* ---- jg add <file> ---- */
static int cmd_add(int argc, char **argv) {
    Index *idx = malloc(sizeof(*idx));
    if (!idx) return -1;
    index_read(idx);
    int result = 0;

    for (int i = 0; i < argc; i++) {
        if (index_add(idx, argv[i]) < 0) {
            fprintf(stderr, "error: cannot add '%s'\n", argv[i]);
            result = -1;
        }
    }

    if (index_write(idx) < 0) result = -1;
    free(idx);
    return result;
}

/*
 * 인덱스로부터 중첩 트리(nested tree)를 재귀적으로 구성.
 *
 * git의 트리는 디렉터리 한 단계만 표현함. 얘를 들어 `src/main.c`는 루트 트리의
 * `40000 src` 엔트리(하위 트리를 가리킴) 안에 `100644 main.c`로 들어가가는데,
 * 인덱스 엔트리의 전체 경로를 그대로 한 트리에 넣으면(평면 트리) 이름에 '/'가
 * 들어가고 서브트리가 없는 것을 처리되어 실제 git과 다른 트리 SHA가 나오므로
 * 반드시 재귀 처리해야 함.
 *
 * prefix: "" 또는 "src/" 처럼 끝에 '/'를 포함하는 디렉터리 경로.
 * out_sha: 생성된 트리의 raw 20바이트 SHA-1.
 */
int build_tree(const Index *idx, const char *prefix, uint8_t *out_sha) {
    size_t plen = strlen(prefix);
    Tree   t;
    t.count = 0;

    for (int i = 0; i < idx->count && t.count < MAX_TREE_ENTRIES; i++) {
        const IndexEntry *e = &idx->entries[i];
        if (e->stage != 0) continue;                       /* 충돌 엔트리 제외 */
        if (plen && strncmp(e->path, prefix, plen) != 0) continue;

        const char *rest = e->path + plen;                 /* prefix 이후 경로 */
        if (*rest == '\0') continue;
        const char *slash = strchr(rest, '/');

        if (!slash) {
            /* 이 디렉터리에 직접 속한 파일 → blob 엔트리 */
            TreeEntry *te = &t.entries[t.count++];
            te->mode = (FileMode)e->mode;
            snprintf(te->name, sizeof(te->name), "%s", rest);
            memcpy(te->sha1, e->sha1, 20);
        } else {
            /* 하위 디렉터리 → 이미 추가한 서브트리면 건너뜀(중복 방지) */
            size_t dlen = (size_t)(slash - rest);
            int    dup  = 0;
            for (int k = 0; k < t.count; k++)
                if (t.entries[k].mode == MODE_DIR &&
                    strlen(t.entries[k].name) == dlen &&
                    strncmp(t.entries[k].name, rest, dlen) == 0) { dup = 1; break; }
            if (dup) continue;

            char sub_prefix[4096];
            snprintf(sub_prefix, sizeof(sub_prefix), "%.*s%.*s/",
                     (int)plen, prefix, (int)dlen, rest);

            TreeEntry *te = &t.entries[t.count++];
            te->mode = MODE_DIR;
            snprintf(te->name, sizeof(te->name), "%.*s", (int)dlen, rest);
            if (build_tree(idx, sub_prefix, te->sha1) < 0) return -1;
        }
    }

    return tree_write(&t, out_sha);  /* tree_write 가 git 규칙으로 정렬·직렬화 */
}

/* ---- jg write-tree ---- */
static int cmd_write_tree(void) {
    Index *idx = malloc(sizeof(*idx));
    if (!idx) return -1;
    index_read(idx);

    uint8_t sha1[20];
    if (build_tree(idx, "", sha1) < 0) {
        free(idx);
        fprintf(stderr, "fatal: write-tree failed\n");
        return -1;
    }
    free(idx);
    char hex[41];
    sha1_to_hex(sha1, hex);
    printf("%s\n", hex);
    return 0;
}

/* ---- jg commit-tree <tree> [-p <parent>] -m <msg> ---- */
static int cmd_commit_tree(int argc, char **argv) {
    Commit c;
    memset(&c, 0, sizeof(c));

    if (argc < 1) return -1;
    strncpy(c.tree_hex, argv[0], 40);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i+1 < argc)
            strncpy(c.parent_hex[c.parent_count++], argv[++i], 40);
        else if (strcmp(argv[i], "-m") == 0 && i+1 < argc)
            strncpy(c.message, argv[++i], sizeof(c.message)-1);
    }

    /* 환경변수에서 사용자 정보 읽기 (실제 git 방식) */
    const char *gn = getenv("GIT_AUTHOR_NAME");   /* GNU `?:` 확장 대신 표준 C */
    const char *ge = getenv("GIT_AUTHOR_EMAIL");
    const char *name  = gn ? gn : "User";
    const char *email = ge ? ge : "user@example.com";
    strncpy(c.author.name,    name,  sizeof(c.author.name)-1);
    strncpy(c.author.email,   email, sizeof(c.author.email)-1);
    strncpy(c.committer.name, name,  sizeof(c.committer.name)-1);
    strncpy(c.committer.email,email, sizeof(c.committer.email)-1);
    c.author.time = c.committer.time = (long)time(NULL);
    strcpy(c.author.tz, "+0000");
    strcpy(c.committer.tz, "+0000");

    uint8_t sha1[20];
    char hex[41];
    commit_write(&c, sha1);
    sha1_to_hex(sha1, hex);
    printf("%s\n", hex);
    return 0;
}

/* ---- jg commit -m <msg> ---- */
static int cmd_commit(int argc, char **argv) {
    const char *msg = "no message";
    for (int i = 0; i < argc-1; i++)
        if (strcmp(argv[i], "-m") == 0) msg = argv[i+1];

    /* write-tree (중첩 트리) */
    Index *idx = malloc(sizeof(*idx));
    if (!idx) return -1;
    index_read(idx);
    uint8_t tree_sha1[20];
    if (build_tree(idx, "", tree_sha1) < 0) {
        free(idx);
        fprintf(stderr, "fatal: write-tree failed\n");
        return -1;
    }
    free(idx);
    char tree_hex[41];
    sha1_to_hex(tree_sha1, tree_hex);

    /* 커밋 오브젝트 */
    Commit c; memset(&c, 0, sizeof(c));
    strncpy(c.tree_hex, tree_hex, 40);

    char parent_hex[41] = {0};
    if (head_read(parent_hex) == 0)
        strncpy(c.parent_hex[c.parent_count++], parent_hex, 40);

    const char *gn = getenv("GIT_AUTHOR_NAME");   /* GNU `?:` 확장 대신 표준 C */
    const char *ge = getenv("GIT_AUTHOR_EMAIL");
    const char *name  = gn ? gn : "User";
    const char *email = ge ? ge : "user@example.com";
    strncpy(c.author.name,    name,  127);
    strncpy(c.author.email,   email, 127);
    strncpy(c.committer.name, name,  127);
    strncpy(c.committer.email,email, 127);
    c.author.time = c.committer.time = (long)time(NULL);
    strcpy(c.author.tz, "+0000");
    strcpy(c.committer.tz, "+0000");
    strncpy(c.message, msg, 4095);

    uint8_t commit_sha1[20];
    commit_write(&c, commit_sha1);
    char commit_hex[41];
    sha1_to_hex(commit_sha1, commit_hex);

    /* HEAD(branch) 업데이트 */
    char branch[128];
    if (head_branch(branch, sizeof(branch)) == 0) {
        char refname[256];
        snprintf(refname, sizeof(refname), "refs/heads/%s", branch);
        ref_write(refname, commit_hex);
    } else {
        head_set_detached(commit_hex);
    }

    printf("[%s] %s\n", commit_hex, msg);
    return 0;
}

static int cmd_branch(int argc, char **argv) {
    if (argc == 0) {
        // 브랜치 목록 출력
        char names[64][128];
        int  n = branch_list(names, 64);
        char current[128] = {0};

        head_branch(current, sizeof(current));
        for (int i = 0; i < n; i++) {
            printf("%s %s\n",
                strcmp(names[i], current) == 0 ? "*" : " ",
                names[i]);
        }
        return 0;
    }

    // 새 브랜치 생성
    char start_hex[41];
    if (head_read(start_hex) < 0) {
        fprintf(stderr, "fatal: no commit yet\n");
        return -1;
    }
    return branch_create(argv[0], start_hex);
}

static int cmd_checkout(int argc, char **argv) {
    if (argc < 1) return -1;
    const char *branch = argv[0];

    // 브랜치가 존재하는지 검사
    char refname[256];
    snprintf(refname, sizeof(refname), "refs/heads/%s", branch);
    char hex[41];
    if (ref_read(refname, hex) < 0) {
        fprintf(stderr, "error: branch '%s' not found\n", branch);
        return -1;
    }

    // working tree 업데이트
    //
    // FIXME: 지금은 커밋의 tree를 현재 디렉토리에 적용하는 식으로 단순하게 처리함.
    // 이후 필요하다면 스펙 확인 후 고도화 고려.
    Commit c;
    if (commit_read(hex, &c) < 0) return -1;
    checkout_tree(c.tree_hex, ".");

    // Index를 브랜치 tree 기준으로 재구성
    Index *idx = malloc(sizeof(*idx));
    if (!idx) return -1;
    idx->count = 0;
    if (index_from_tree(idx, c.tree_hex, "") < 0 || index_write(idx) < 0) {
        free(idx);
        return -1;
    }
    free(idx);

    // HEAD 업데이트
    if (head_set_branch(branch) < 0) return -1;

    printf("Switched to branch %s\n", branch);
    return 0;
}

/*
 * 트리 오브젝트의 내용을 실제 파일 시스템에 기록
 * prefix: 현재 상태 경로
 */
void checkout_tree(const char *tree_hex, const char *prefix) {
    Tree t;
    if (tree_read(tree_hex, &t) < 0) return;

    for (int i = 0; i < t.count; i++) {
        TreeEntry *e = &t.entries[i];
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", prefix, e->name);

        char sha1_hex[41];
        sha1_to_hex(e->sha1, sha1_hex);

        if (e->mode == MODE_DIR) {
            mkdir(full_path, 0755);
            checkout_tree(sha1_hex, full_path);
        } else {
            char type[8]; uint8_t *data = NULL; size_t data_len = 0;
            if (object_read(sha1_hex, type, &data, &data_len) < 0) continue;

            FILE *f = fopen(full_path, "wb");
            if (f) {
                fwrite(data, 1, data_len, f);
                fclose(f);
                if (e->mode == MODE_EXEC) chmod(full_path, 0755);
            }
            free(data);
        }
    }
}

/*
 * 트리 오브젝트를 재귀적으로 읽은 다음 인덱스를 재구성함. 특히,
 * checkout이나 merge 시에 사용함.
 *
 * prefix: "" 또는 "src/" 처럼 끝에 "/"를 포함해야 함.
 * stat 정보는 0으로 두고, 이후 add나 status 시 채워 짐.
 */
int index_from_tree(Index *idx, const char *tree_hex, const char *prefix) {
    Tree t;
    if (tree_read(tree_hex, &t) < 0) return -1;

    for (int i = 0; i < t.count; i++) {
        TreeEntry *e = &t.entries[i];
        char sha1_hex[41];
        sha1_to_hex(e->sha1, sha1_hex);

        if (e->mode == MODE_DIR) {
            char sub[4096];
            snprintf(sub, sizeof(sub), "%s%s/", prefix, e->name);
            if (index_from_tree(idx, sha1_hex, sub) < 0) return -1;
        } else {
            if (idx->count >= INDEX_MAX_ENTRIES) return -1;
            IndexEntry *ie = &idx->entries[idx->count++];
            memset(ie, 0, sizeof(*ie));
            ie->mode = (uint32_t)e->mode;
            ie->stage = 0;
            memcpy(ie->sha1, e->sha1, 20);
            snprintf(ie->path, sizeof(ie->path), "%s%s", prefix, e->name);
        }
    }
    return 0;
}

/*
 * TODO: checkout_tree는 대상 트리에 존재하는 파일만 기록할 뿐, working tree에는
 * 있지만 대상 트리에 없는 파일을 삭제하지는 않는다. 실제 git 처럼 브랜치 전환 시 사라진
 * 파일을 지우려면, 전환 전의 인덱스와 트리하고 비교해 제거하는 단계를 별도로 구현해야 함.
 */

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: jg <command>\n"); return 1; }

    const char *cmd = argv[1];
    if      (strcmp(cmd, "init")         == 0) return cmd_init();
    else if (strcmp(cmd, "hash-object")  == 0) return cmd_hash_object(argc-2, argv+2);
    else if (strcmp(cmd, "cat-file")     == 0) return cmd_cat_file(argc-2, argv+2);
    else if (strcmp(cmd, "add")          == 0) return cmd_add(argc-2, argv+2);
    else if (strcmp(cmd, "write-tree")   == 0) return cmd_write_tree();
    else if (strcmp(cmd, "commit-tree")  == 0) return cmd_commit_tree(argc-2, argv+2);
    else if (strcmp(cmd, "commit")       == 0) return cmd_commit(argc-2, argv+2);
    else if (strcmp(cmd, "branch")       == 0) return cmd_branch(argc-2, argv+2);
    else if (strcmp(cmd, "checkout")     == 0) return cmd_checkout(argc-2, argv+2);
    // TODO: merge 추가
    else { fprintf(stderr, "unknown command: %s\n", cmd); return 1; }
}
