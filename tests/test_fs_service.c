#include <moltest.h>

#include <molto/services/fs_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Reading one line out of a file.
 *
 * The interesting cases are all the ones where there is no line to read,
 * because that is what happens whenever a compiler names something that is not
 * a file on disk — and it names those often.
 */

/* A temporary file holding `content`. The path is written into `path`. */
static bool write_temp(char *path, size_t path_size, const char *content) {
    if(!moltest_temp_file("molto_fs", path, path_size))
        return false;
    return fs_write_file(path, content);
}

MOLTEST(a_line_is_read_without_its_ending) {
    char path[64];
    ASSERT_TRUE(write_temp(path, sizeof path, "first\nsecond\nthird\n"));

    char line[128] = "";
    EXPECT_TRUE(fs_read_line(path, 1, line, sizeof line));
    EXPECT_STREQ("first", line);
    EXPECT_TRUE(fs_read_line(path, 2, line, sizeof line));
    EXPECT_STREQ("second", line);
    EXPECT_TRUE(fs_read_line(path, 3, line, sizeof line));
    EXPECT_STREQ("third", line);

    (void)remove(path);
}

/* The last line of a file that does not end in a newline is still a line. */
MOLTEST(a_file_that_does_not_end_in_a_newline_still_has_a_last_line) {
    char path[64];
    ASSERT_TRUE(write_temp(path, sizeof path, "alpha\nomega"));

    char line[128] = "";
    EXPECT_TRUE(fs_read_line(path, 2, line, sizeof line));
    EXPECT_STREQ("omega", line);

    (void)remove(path);
}

/* Two characters end a line on another platform, and showing the first of them
   would put a stray glyph at the end of every excerpt taken from that file. */
MOLTEST(a_carriage_return_is_part_of_the_ending_and_not_of_the_line) {
    char path[64];
    ASSERT_TRUE(write_temp(path, sizeof path, "one\r\ntwo\r\n"));

    char line[128] = "";
    EXPECT_TRUE(fs_read_line(path, 2, line, sizeof line));
    EXPECT_STREQ("two", line);

    (void)remove(path);
}

MOLTEST(an_empty_line_is_a_line) {
    char path[64];
    ASSERT_TRUE(write_temp(path, sizeof path, "top\n\nbottom\n"));

    char line[128] = "sentinel";
    EXPECT_TRUE(fs_read_line(path, 2, line, sizeof line));
    EXPECT_STREQ("", line);

    (void)remove(path);
}

/* Past the end, at zero, and negative: none of these is a line, and none of
   them is a reason to leave the caller's buffer holding whatever it held. */
MOLTEST(a_line_that_does_not_exist_is_reported_rather_than_invented) {
    char path[64];
    ASSERT_TRUE(write_temp(path, sizeof path, "only\n"));

    char line[128] = "sentinel";
    EXPECT_FALSE(fs_read_line(path, 2, line, sizeof line));
    EXPECT_STREQ("", line);
    EXPECT_FALSE(fs_read_line(path, 0, line, sizeof line));
    EXPECT_FALSE(fs_read_line(path, -1, line, sizeof line));

    (void)remove(path);
}

/* The ordinary answer for `<command line>`, `<built-in>`, and any source that
   was generated and then removed. */
MOLTEST(a_file_that_cannot_be_opened_is_not_an_excerpt) {
    char line[128] = "sentinel";
    EXPECT_FALSE(fs_read_line("/tmp/molto_no_such_file_zzz", 1, line, sizeof line));
    EXPECT_STREQ("", line);
    EXPECT_FALSE(fs_read_line("<command line>", 1, line, sizeof line));
}

/* An excerpt is for reading, and part of one still reads. What must not happen
   is running on into the line after it. */
MOLTEST(a_line_longer_than_the_buffer_is_shortened_not_refused) {
    char path[64];
    ASSERT_TRUE(write_temp(path, sizeof path, "aaaaaaaaaaaaaaaaaaaa\nnext\n"));

    char line[8] = "";
    EXPECT_TRUE(fs_read_line(path, 1, line, sizeof line));
    EXPECT_EQ(sizeof line - 1, strlen(line));
    EXPECT_STREQ("aaaaaaa", line);

    /* The line after it is still where it was. */
    char after[64] = "";
    EXPECT_TRUE(fs_read_line(path, 2, after, sizeof after));
    EXPECT_STREQ("next", after);

    (void)remove(path);
}

MOLTEST(no_buffer_is_not_a_place_to_put_a_line) {
    char line[4] = "";
    EXPECT_FALSE(fs_read_line("/etc/hostname", 1, NULL, 8));
    EXPECT_FALSE(fs_read_line("/etc/hostname", 1, line, 0));
}

MOLTEST(fs_reports_the_directory_it_is_in) {
    char here[4096] = "";
    ASSERT_TRUE(fs_current_dir(here, sizeof here));
    EXPECT_TRUE(here[0] != '\0');
    EXPECT_TRUE(fs_is_dir(here));

    /* Absolute, because every caller joins something onto it and then compares
       the result against a root that came from the same place. What "absolute"
       looks like differs — a leading slash here, a drive letter on Windows —
       so the assertion is the one thing both share: it resolves to itself. */
    char resolved[4096] = "";
    ASSERT_TRUE(fs_real_path(here, resolved, sizeof resolved));
    EXPECT_STREQ(here, resolved);
}

MOLTEST(a_buffer_too_small_for_the_directory_is_refused) {
    char cramped[2] = "";
    EXPECT_FALSE(fs_current_dir(cramped, sizeof cramped));
}

MOLTEST(an_absolute_path_is_recognised_as_one) {
    EXPECT_TRUE(fs_path_is_absolute("/usr/include"));
    EXPECT_TRUE(fs_path_is_absolute("/"));

    EXPECT_FALSE(fs_path_is_absolute("src/main.c"));
    EXPECT_FALSE(fs_path_is_absolute("./here"));
    EXPECT_FALSE(fs_path_is_absolute(""));
    EXPECT_FALSE(fs_path_is_absolute(NULL));

    /* A drive letter names a place only on the platform that has drives, and
       there only with the slash: `D:x` is relative to whatever directory that
       drive happens to be on. Asserted from both sides so the answer here is
       written down rather than assumed — on POSIX these are ordinary relative
       names, and a file really can be called `D:`. */
#ifdef _WIN32
    EXPECT_TRUE(fs_path_is_absolute("D:/work"));
    EXPECT_TRUE(fs_path_is_absolute("c:/work"));
    EXPECT_FALSE(fs_path_is_absolute("D:work"));
#else
    EXPECT_FALSE(fs_path_is_absolute("D:/work"));
    EXPECT_FALSE(fs_path_is_absolute("D:work"));
#endif
}

/*
 * A name and a filename, and the conversion between them.
 *
 * These two are inverses, and the pair exists because on Windows they are not
 * the same string: there is no execute bit there, so `.exe` is what says a
 * file may be run at all. Everything that composes a name and then looks for
 * the file needs `fs_executable_file`; everything that reads a filename and
 * wants the name a person types needs `fs_executable_name`.
 */
MOLTEST(a_name_becomes_the_filename_the_platform_stores_it_in) {
    char file[64] = "";
    EXPECT_TRUE(fs_executable_file("molto-meson", file, sizeof file));
#ifdef _WIN32
    EXPECT_STREQ("molto-meson.exe", file);
#else
    EXPECT_STREQ("molto-meson", file);
#endif
}

MOLTEST(a_filename_that_does_not_fit_is_refused_rather_than_cut) {
    char file[4] = "";
    EXPECT_FALSE(fs_executable_file("molto-meson", file, sizeof file));
}

MOLTEST(a_filename_gives_back_the_name_it_is_run_by) {
    char name[64] = "";
#ifdef _WIN32
    EXPECT_TRUE(fs_executable_name("molto-meson.exe", name, sizeof name));
    EXPECT_STREQ("molto-meson", name);

    /* Case-insensitively: a filesystem that does not tell `.EXE` from `.exe`
       hands back either, so both are the same file and both are runnable. */
    EXPECT_TRUE(fs_executable_name("molto-meson.EXE", name, sizeof name));
    EXPECT_STREQ("molto-meson", name);

    /* And a file without it cannot be run, so it has no such name. */
    EXPECT_FALSE(fs_executable_name("molto-meson", name, sizeof name));
    EXPECT_FALSE(fs_executable_name(".exe", name, sizeof name));
#else
    /* Nothing is appended to run a file here, so the filename is the name. */
    EXPECT_TRUE(fs_executable_name("molto-meson", name, sizeof name));
    EXPECT_STREQ("molto-meson", name);
#endif
}

/* The round trip, which is the property the two are used for: a plugin is
   found by scanning filenames and started by composing one. */
MOLTEST(a_name_survives_the_trip_through_its_filename) {
    char file[64] = "";
    char back[64] = "";
    EXPECT_TRUE(fs_executable_file("molto-meson", file, sizeof file));
    EXPECT_TRUE(fs_executable_name(file, back, sizeof back));
    EXPECT_STREQ("molto-meson", back);
}
