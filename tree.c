// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include "index.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';

        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];

        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1;

        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── FIXED PART ─────────────────────────────────────────────────────────────

// 🔥 Recursive helper
static int build_tree(IndexEntry *entries, int count, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    for (int i = 0; i < count; i++) {
        IndexEntry *entry = &entries[i];

        char *slash = strchr(entry->path, '/');

        if (!slash) {
            TreeEntry *t = &tree.entries[tree.count++];
            t->mode = entry->mode;
            strcpy(t->name, entry->path);
            t->hash = entry->hash;
        } else {
            char dirname[256];
            size_t len = slash - entry->path;
            strncpy(dirname, entry->path, len);
            dirname[len] = '\0';

            int exists = 0;
            for (int j = 0; j < tree.count; j++) {
                if (strcmp(tree.entries[j].name, dirname) == 0) {
                    exists = 1;
                    break;
                }
            }

            if (exists) continue;

            // 🔥 FIX: heap allocation instead of stack
            IndexEntry *sub_entries = malloc(sizeof(IndexEntry) * count);
            if (!sub_entries) return -1;

            int sub_count = 0;

            for (int k = 0; k < count; k++) {
                if (strncmp(entries[k].path, dirname, len) == 0 &&
                    entries[k].path[len] == '/') {

                    IndexEntry sub = entries[k];

                    // 🔥 FIXED memmove (safe)
                    size_t new_len = strlen(sub.path) - len - 1;
                    memmove(sub.path,
                            sub.path + len + 1,
                            new_len);
                    sub.path[new_len] = '\0';

                    sub_entries[sub_count++] = sub;
                }
            }

            ObjectID sub_id;
            if (build_tree(sub_entries, sub_count, &sub_id) != 0) {
                free(sub_entries);
                return -1;
            }

            free(sub_entries);

            TreeEntry *t = &tree.entries[tree.count++];
            t->mode = MODE_DIR;
            strcpy(t->name, dirname);
            t->hash = sub_id;
        }
    }

    void *data;
    size_t len;

    if (tree_serialize(&tree, &data, &len) != 0) return -1;

    if (object_write(OBJ_TREE, data, len, id_out) != 0) {
        free(data);
        return -1;
    }

    free(data);
    return 0;
}

// Main function
int tree_from_index(ObjectID *id_out) {
    Index index;

    if (index_load(&index) != 0) return -1;

    return build_tree(index.entries, index.count, id_out);
}