#include <molto/build/diff.h>

#include <stdlib.h>
#include <string.h>

/* Above this many differing lines on either side, the quadratic table below
   would cost more than the diff is worth, and the answer is reported as one
   wholesale replacement instead. A formatter rewriting a file that large has
   nothing useful to say line by line anyway. */
#define DIFF_MAX_LINES 5000

/* Growth policy for the line and operation arrays. */
#define DIFF_INITIAL_CAPACITY 64
#define DIFF_GROWTH_FACTOR 2

/* One line, pointing into the text it came from rather than copying it. */
typedef struct {
    const char *start;
    size_t length;
} line;

typedef struct {
    line *items;
    size_t count;
    size_t capacity;
} line_list;

typedef enum { op_keep, op_delete, op_add } op_kind;

typedef struct {
    op_kind kind;
    line text;
} diff_op;

typedef struct {
    diff_op *items;
    size_t count;
    size_t capacity;
} op_list;

static void line_list_init(line_list *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static bool line_list_push(line_list *list, line item) {
    if(list->count == list->capacity) {
        size_t capacity =
            list->capacity == 0 ? DIFF_INITIAL_CAPACITY : list->capacity * DIFF_GROWTH_FACTOR;
        line *grown = realloc(list->items, capacity * sizeof *grown);
        if(grown == NULL)
            return false;
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count++] = item;
    return true;
}

static void op_list_init(op_list *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static bool op_list_push(op_list *list, op_kind kind, line text) {
    if(list->count == list->capacity) {
        size_t capacity =
            list->capacity == 0 ? DIFF_INITIAL_CAPACITY : list->capacity * DIFF_GROWTH_FACTOR;
        diff_op *grown = realloc(list->items, capacity * sizeof *grown);
        if(grown == NULL)
            return false;
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count].kind = kind;
    list->items[list->count].text = text;
    list->count++;
    return true;
}

/* Split `text` into lines, without the newline. A trailing newline does not
   produce a final empty line, so "a\n" is one line and not two. */
static bool split_lines(const char *text, line_list *out) {
    const char *cursor = text;
    while(*cursor != '\0') {
        const char *end = strchr(cursor, '\n');
        line item = {
            .start = cursor,
            .length = end != NULL ? (size_t)(end - cursor) : strlen(cursor),
        };
        if(!line_list_push(out, item))
            return false;
        if(end == NULL)
            break;
        cursor = end + 1;
    }
    return true;
}

static bool lines_equal(line left, line right) {
    return left.length == right.length && memcmp(left.start, right.start, left.length) == 0;
}

/* Emit `count` operations of one kind, from `at`. */
static bool push_range(op_list *ops, op_kind kind, const line *items, size_t at, size_t count) {
    for(size_t i = 0; i < count; i++) {
        if(!op_list_push(ops, kind, items[at + i]))
            return false;
    }
    return true;
}

/* The longest common subsequence of two line ranges, as the operations that
   turn the first into the second. Quadratic in time and memory, which is why
   the common prefix and suffix are stripped before it is reached. */
static bool lcs_ops(const line *old_lines, size_t old_count, const line *new_lines,
                    size_t new_count, op_list *ops) {
    size_t width = new_count + 1;
    size_t *table = calloc((old_count + 1) * width, sizeof *table);
    if(table == NULL)
        return false;

    for(size_t i = old_count; i-- > 0;) {
        for(size_t j = new_count; j-- > 0;) {
            size_t here = i * width + j;
            table[here] = lines_equal(old_lines[i], new_lines[j])
                              ? table[(i + 1) * width + (j + 1)] + 1
                              : (table[(i + 1) * width + j] > table[i * width + (j + 1)]
                                     ? table[(i + 1) * width + j]
                                     : table[i * width + (j + 1)]);
        }
    }

    bool ok = true;
    size_t i = 0;
    size_t j = 0;
    while(ok && i < old_count && j < new_count) {
        if(lines_equal(old_lines[i], new_lines[j])) {
            ok = op_list_push(ops, op_keep, old_lines[i]);
            i++;
            j++;
        } else if(table[(i + 1) * width + j] >= table[i * width + (j + 1)]) {
            ok = op_list_push(ops, op_delete, old_lines[i]);
            i++;
        } else {
            ok = op_list_push(ops, op_add, new_lines[j]);
            j++;
        }
    }
    ok = ok && push_range(ops, op_delete, old_lines, i, old_count - i) &&
         push_range(ops, op_add, new_lines, j, new_count - j);

    free(table);
    return ok;
}

/* Too large to diff line by line: report it as replacing the whole range. */
static bool wholesale_ops(const line *old_lines, size_t old_count, const line *new_lines,
                          size_t new_count, op_list *ops) {
    return push_range(ops, op_delete, old_lines, 0, old_count) &&
           push_range(ops, op_add, new_lines, 0, new_count);
}

/* Build the operation list for two whole files, stripping what they share at
   each end first. For two versions of one file that is nearly all of it. */
static bool build_ops(const line_list *old_lines, const line_list *new_lines, op_list *ops) {
    size_t prefix = 0;
    while(prefix < old_lines->count && prefix < new_lines->count &&
          lines_equal(old_lines->items[prefix], new_lines->items[prefix]))
        prefix++;

    size_t suffix = 0;
    while(suffix < old_lines->count - prefix && suffix < new_lines->count - prefix &&
          lines_equal(old_lines->items[old_lines->count - 1 - suffix],
                      new_lines->items[new_lines->count - 1 - suffix]))
        suffix++;

    size_t old_middle = old_lines->count - prefix - suffix;
    size_t new_middle = new_lines->count - prefix - suffix;

    if(!push_range(ops, op_keep, old_lines->items, 0, prefix))
        return false;

    bool ok = old_middle > DIFF_MAX_LINES || new_middle > DIFF_MAX_LINES
                  ? wholesale_ops(old_lines->items + prefix, old_middle, new_lines->items + prefix,
                                  new_middle, ops)
                  : lcs_ops(old_lines->items + prefix, old_middle, new_lines->items + prefix,
                            new_middle, ops);

    return ok && push_range(ops, op_keep, old_lines->items, old_lines->count - suffix, suffix);
}

static char op_marker(op_kind kind) {
    switch(kind) {
    case op_delete:
        return '-';
    case op_add:
        return '+';
    case op_keep:
    default:
        return ' ';
    }
}

static bool is_change(op_kind kind) { return kind != op_keep; }

/* Write one hunk, from `first` to `last` inclusive, and the line numbers it
   spans on each side. */
static void write_hunk(FILE *stream, const op_list *ops, size_t first, size_t last,
                       size_t old_start, size_t new_start) {
    size_t old_count = 0;
    size_t new_count = 0;
    for(size_t i = first; i <= last; i++) {
        if(ops->items[i].kind != op_add)
            old_count++;
        if(ops->items[i].kind != op_delete)
            new_count++;
    }

    fprintf(stream, "@@ -%zu,%zu +%zu,%zu @@\n", old_count > 0 ? old_start : 0, old_count,
            new_count > 0 ? new_start : 0, new_count);
    for(size_t i = first; i <= last; i++) {
        const diff_op *op = &ops->items[i];
        fprintf(stream, "%c%.*s\n", op_marker(op->kind), (int)op->text.length, op->text.start);
    }
}

/* Group the operations into hunks and write them, each surrounded by `context`
   unchanged lines. Runs of changes closer than twice the context are one hunk:
   splitting them would repeat the lines between. */
static void write_hunks(FILE *stream, const op_list *ops, size_t context) {
    /* Line numbers, 1-based, as a diff reader expects. */
    size_t old_number = 1;
    size_t new_number = 1;
    size_t at = 0;

    while(at < ops->count) {
        if(!is_change(ops->items[at].kind)) {
            old_number++;
            new_number++;
            at++;
            continue;
        }

        /* Walk back over the leading context. */
        size_t first = at;
        size_t back = 0;
        while(first > 0 && back < context && !is_change(ops->items[first - 1].kind)) {
            first--;
            back++;
        }

        /* Walk forward to the last change, absorbing runs separated by less
           than twice the context. */
        size_t last = at;
        size_t gap = 0;
        for(size_t i = at; i < ops->count; i++) {
            if(is_change(ops->items[i].kind)) {
                last = i;
                gap = 0;
            } else if(++gap > 2 * context) {
                break;
            }
        }

        size_t trailing = 0;
        while(last + 1 < ops->count && trailing < context &&
              !is_change(ops->items[last + 1].kind)) {
            last++;
            trailing++;
        }

        write_hunk(stream, ops, first, last, old_number - back, new_number - back);

        for(size_t i = at; i <= last; i++) {
            if(ops->items[i].kind != op_add)
                old_number++;
            if(ops->items[i].kind != op_delete)
                new_number++;
        }
        at = last + 1;
    }
}

bool diff_unified(const char *original, const char *formatted, const char *path, size_t context,
                  FILE *stream, bool *changed) {
    if(changed != NULL)
        *changed = false;
    if(strcmp(original, formatted) == 0)
        return true;
    if(changed != NULL)
        *changed = true;

    line_list old_lines;
    line_list new_lines;
    op_list ops;
    line_list_init(&old_lines);
    line_list_init(&new_lines);
    op_list_init(&ops);

    bool ok = split_lines(original, &old_lines) && split_lines(formatted, &new_lines) &&
              build_ops(&old_lines, &new_lines, &ops);
    if(ok) {
        fprintf(stream, "--- a/%s\n+++ b/%s\n", path, path);
        write_hunks(stream, &ops, context);
    }

    free(old_lines.items);
    free(new_lines.items);
    free(ops.items);
    return ok;
}
