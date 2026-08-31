#include <molto/commands/login_command.h>

#include <molto/exit_code.h>
#include <molto/services/console_service.h>
#include <molto/services/credentials_service.h>
#include <molto/services/registry_service.h>

#include <stdio.h>
#include <string.h>

/* Name the token carries in the account page's list. */
#define TOKEN_NAME "molto login"

static void report(const char *message) { fprintf(stderr, "molto: %s\n", message); }

/* Reads a line, dropping the newline. False at end of input. */
static bool read_line(char *out, size_t size) {
    if(fgets(out, (int)size, stdin) == NULL)
        return false;
    out[strcspn(out, "\r\n")] = '\0';
    return true;
}

static bool prompt_line(const char *label, char *out, size_t size) {
    fputs(label, stderr);
    fflush(stderr);
    return read_line(out, size) && out[0] != '\0';
}

/* Reads a password without echoing it.

   Refuses when stdin is not a terminal rather than reading it anyway: a
   password arriving on a pipe is one that came from a script, where it would
   have been written down somewhere. `--token` is the way to log in without a
   terminal. */
static bool prompt_password(char *out, size_t size) {
    switch(console_read_secret("Password: ", out, size)) {
    case console_secret_ok:
        return true;
    case console_secret_not_a_terminal:
        report("a password can only be typed at a terminal; use --token instead");
        return false;
    case console_secret_no_control:
        report("could not turn off echo, so the password would be visible");
        return false;
    case console_secret_empty:
        return false;
    }
    return false;
}

/* Forgets a secret before its buffer goes out of scope. Not a guarantee — the
   compiler may still elide it — but the volatile write makes that unlikely and
   the intent explicit. */
static void wipe(char *text, size_t size) {
    volatile char *p = (volatile char *)text;
    for(size_t i = 0; i < size; i++)
        p[i] = '\0';
}

static bool store(const char *registry, const char *email, const char *token) {
    credentials creds = {0};
    snprintf(creds.registry, sizeof creds.registry, "%s", registry);
    snprintf(creds.email, sizeof creds.email, "%s", email);
    snprintf(creds.token, sizeof creds.token, "%s", token);

    char err[256] = "";
    if(!credentials_save(&creds, err, sizeof err)) {
        report(err);
        return false;
    }

    char path[768];
    if(credentials_path(path, sizeof path))
        fprintf(stderr, "Logged in to %s; the token is in %s\n", registry, path);
    return true;
}

/* --token: the person already made one on the account page, so there is no
   password to exchange and nothing to verify here — the first publish will. */
static int login_with_token(const char *registry, const char *email, const char *token) {
    return store(registry, email == NULL ? "" : email, token) ? exit_ok : exit_dependency_failure;
}

static int login_with_password(const char *registry, const char *email) {
    char address[CREDENTIALS_EMAIL_MAX];
    if(email != NULL)
        snprintf(address, sizeof address, "%s", email);
    else if(!prompt_line("Email: ", address, sizeof address)) {
        report("no email given");
        return exit_usage_error;
    }

    char password[256];
    if(!prompt_password(password, sizeof password)) {
        wipe(password, sizeof password);
        return exit_usage_error;
    }

    char token[CREDENTIALS_TOKEN_MAX];
    char err[512] = "";
    const bool issued = registry_create_token(registry, address, password, TOKEN_NAME, token,
                                              sizeof token, err, sizeof err);
    wipe(password, sizeof password);

    if(!issued) {
        report(err);
        return exit_dependency_failure;
    }
    return store(registry, address, token) ? exit_ok : exit_dependency_failure;
}

int login_command_run(const char *registry, const char *email, const char *token) {
    const char *url = registry != NULL && registry[0] != '\0' ? registry : REGISTRY_DEFAULT_URL;

    if(token != NULL && token[0] != '\0')
        return login_with_token(url, email, token);
    return login_with_password(url, email);
}
