#ifndef MOLTO_LOGIN_COMMAND_H
#define MOLTO_LOGIN_COMMAND_H

/* Execute `molto login [--registry <url>] [--email <address>] [--token <t>]`.

   Stores a bearer token in ~/.molto/credentials.toml, either by exchanging an
   email and password for one or by taking a token created on the registry's
   account page. What is stored is never the password: the registry keeps only
   the token's digest, so a leaked credentials file can be revoked.

   `email` and `token` may be NULL, in which case the missing one is prompted
   for. Returns a molto_exit_code. */
[[nodiscard]] int login_command_run(const char *registry, const char *email, const char *token);

#endif /* MOLTO_LOGIN_COMMAND_H */
