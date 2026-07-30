#ifndef MOLTO_TESTS_H
#define MOLTO_TESTS_H

/* Test suites, one per unit under test. */
void suite_manifest_service(void);
void suite_cli(void);
void suite_str_list(void);
void suite_str_map(void);
void suite_depfile(void);
void suite_scaffold(void);
void suite_workspace(void);
void suite_wsdb(void);
void suite_profile(void);
void suite_source_discovery(void);
void suite_process_service(void);
void suite_build_service(void);
void suite_run_command(void);
void suite_test_command(void);
void suite_task_pool(void);
void suite_toml(void);
void suite_project_ctx(void);

#endif /* MOLTO_TESTS_H */
