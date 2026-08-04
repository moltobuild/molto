#include <molto/build/compile_flags.h>

#include <molto/services/fs_service.h>

/* Compiler command-line arguments composed here. */
#define STD_FLAG_FORMAT "-std=%s" /* language standard, e.g. -std=c23 */
#define DEFINE_FLAG_PREFIX "-D"   /* prepended to a preprocessor define */
#define INCLUDE_FLAG_PREFIX "-I"  /* prepended to an include directory */

/* Size of the stack buffer used to compose an include flag, which carries a
   whole path. */
#define FLAG_PATH_SIZE 4096

/* Size of the buffer holding the "-std=<name>" flag. */
#define STD_FLAG_SIZE 24

const char *compile_flags_driver(const resolved_toolchain *chain, bool is_cpp) {
    if(!is_cpp)
        return chain->cc;
    return chain->cxx[0] != '\0' ? chain->cxx : NULL;
}

bool compile_flags_push_prefixed(str_list *argv, const char *prefix, const char *value) {
    char buffer[PROJECT_OPT_LEN + 4];
    if(!fs_format_path(buffer, sizeof buffer, "%s%s", prefix, value))
        return fs_report_long_path(value);
    return str_list_push(argv, buffer);
}

bool compile_flags_push_include(str_list *argv, const char *root, const char *directory) {
    /* Composed here rather than through compile_flags_push_prefixed, whose buffer is sized
       for an option value and not for a path. */
    char flag[FLAG_PATH_SIZE];
    bool composed =
        directory[0] == '/'
            ? fs_format_path(flag, sizeof flag, INCLUDE_FLAG_PREFIX "%s", directory)
            : fs_format_path(flag, sizeof flag, INCLUDE_FLAG_PREFIX "%s/%s", root, directory);
    if(!composed)
        return fs_report_long_path(directory);
    return str_list_push(argv, flag);
}

bool compile_flags_push_options(str_list *argv, const char *root, const project_options *options) {
    bool ok = true;
    for(size_t i = 0; ok && i < options->define_count; i++)
        ok = compile_flags_push_prefixed(argv, DEFINE_FLAG_PREFIX, options->defines[i]);
    for(size_t i = 0; ok && i < options->include_count; i++)
        ok = compile_flags_push_include(argv, root, options->include[i]);
    for(size_t i = 0; ok && i < options->flag_count; i++)
        ok = str_list_push(argv, options->flags[i]);
    return ok;
}

bool compile_flags_push_std(str_list *argv, const project_target *target, bool is_cpp) {
    const char *value = is_cpp ? target->cpp_std : target->std;
    if(value[0] == '\0')
        return true; /* no standard declared for this language */
    char flag[STD_FLAG_SIZE];
    if(!fs_format_path(flag, sizeof flag, STD_FLAG_FORMAT, value))
        return fs_report_long_path(value);
    return str_list_push(argv, flag);
}
