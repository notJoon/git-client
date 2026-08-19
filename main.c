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
    // TODO: merge, branch, checkout 추가
    else { fprintf(stderr, "unknown command: %s\n", cmd); return 1; }
}
