/* test_config.c - config file parser and inline overrides. */
#include <stdio.h>
#include <string.h>

#include "core/config.h"
#include "test.h"

#define TMP_CONF "test_config_tmp.conf"

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    DD_CHECK(f != NULL);
    DD_CHECK(fputs(content, f) >= 0);
    fclose(f);
}

static void test_defaults(void)
{
    ddup_config cfg;
    config_init(&cfg);
    DD_CHECK_EQ_INT(6379, cfg.port);
    DD_CHECK_STR("0.0.0.0", cfg.bind);
    DD_CHECK_EQ_INT(0, (long long)cfg.maxmemory);
    DD_CHECK_EQ_INT(DB_POLICY_ALLKEYS_LRU, cfg.maxmemory_policy);
    DD_CHECK_STR(".", cfg.dir);
    DD_CHECK_EQ_INT(0, cfg.appendonly);
    DD_CHECK_STR("appendonly.aof", cfg.appendfilename);
    DD_CHECK_STR("dump.ddr", cfg.dbfilename);
    DD_CHECK_EQ_INT(0, cfg.save_sec);
    DD_CHECK_EQ_INT(1, cfg.io_threads);
    DD_CHECK_EQ_INT(1024LL * 1024LL * 1024LL,
                    (long long)cfg.proto_max_request_bytes);
    DD_CHECK_EQ_INT(1024LL * 1024LL * 1024LL,
                    (long long)cfg.repl_max_snapshot_bytes);
}

static void test_requirepass(void)
{
    ddup_config cfg;
    config_init(&cfg);
    DD_CHECK(cfg.requirepass[0] == '\0');
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "requirepass", "s3cret"));
    DD_CHECK_STR("s3cret", cfg.requirepass);
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "REQUIREPASS", "other"));
    DD_CHECK_STR("other", cfg.requirepass);
}

static void test_io_backend_values(void)
{
    ddup_config cfg;

    config_init(&cfg);
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "io", "select"));
    DD_CHECK_STR("select", cfg.io);
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "io", "iocp"));
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "io", "iouring"));
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "io", "iouring-op"));
    DD_CHECK_STR("iouring-op", cfg.io);
    DD_CHECK_EQ_INT(-1, config_apply(&cfg, "io", "bogus"));
}

static void test_io_threads(void)
{
    ddup_config cfg;
    char err[256];

    config_init(&cfg);
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "io-threads", "4"));
    DD_CHECK_EQ_INT(4, cfg.io_threads);
    DD_CHECK_EQ_INT(-1, config_apply(&cfg, "io-threads", "0"));
    DD_CHECK_EQ_INT(-1, config_apply(&cfg, "io-threads", "abc"));

    /* persistence is supported in mt mode (per-worker files); cluster and
     * replication still conflict */
    config_init(&cfg);
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "io-threads", "2"));
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "appendonly", "yes"));
    DD_CHECK_EQ_INT(0, config_validate(&cfg, err, sizeof(err)));

    config_init(&cfg);
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "io-threads", "2"));
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "save", "60"));
    DD_CHECK_EQ_INT(0, config_validate(&cfg, err, sizeof(err)));

    /* TLS is supported in mt mode (per-worker contexts, acceptor-owned
     * TLS listener) */
    config_init(&cfg);
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "io-threads", "2"));
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "tls-port", "6381"));
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "tls-cert-file", TMP_CONF));
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "tls-key-file", TMP_CONF));
    write_file(TMP_CONF, "# x\n");
    DD_CHECK_EQ_INT(0, config_validate(&cfg, err, sizeof(err)));
    remove(TMP_CONF);

    config_init(&cfg);
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "io-threads", "2"));
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "cluster-enabled", "yes"));
    DD_CHECK_EQ_INT(-1, config_validate(&cfg, err, sizeof(err)));
    DD_CHECK(strstr(err, "io-threads") != NULL);
}

static void test_file_parse(void)
{
    ddup_config cfg;
    config_init(&cfg);
    write_file(TMP_CONF,
               "# comment line\n"
               "\n"
               "port 7777\n"
               "BIND 127.0.0.1\n"
               "maxmemory 1048576\n"
               "maxmemory-policy noeviction\n"
               "appendonly yes\n"
               "dir /tmp/ddup\n"
               "appendfilename custom.aof\n"
               "dbfilename custom.ddr\n"
               "save 60\n");
    DD_CHECK_EQ_INT(0, config_load_file(&cfg, TMP_CONF));
    DD_CHECK_EQ_INT(7777, cfg.port);
    DD_CHECK_STR("127.0.0.1", cfg.bind);
    DD_CHECK_EQ_INT(1048576, (long long)cfg.maxmemory);
    DD_CHECK_EQ_INT(DB_POLICY_NOEVICTION, cfg.maxmemory_policy);
    DD_CHECK_EQ_INT(1, cfg.appendonly);
    DD_CHECK_STR("/tmp/ddup", cfg.dir);
    DD_CHECK_STR("custom.aof", cfg.appendfilename);
    DD_CHECK_STR("custom.ddr", cfg.dbfilename);
    DD_CHECK_EQ_INT(60, cfg.save_sec);
    remove(TMP_CONF);
}

static void test_file_errors(void)
{
    ddup_config cfg;
    config_init(&cfg);
    DD_CHECK_EQ_INT(-1, config_load_file(&cfg, "no_such_file.conf"));

    write_file(TMP_CONF, "boguskey 1\n");
    DD_CHECK_EQ_INT(-1, config_load_file(&cfg, TMP_CONF));
    remove(TMP_CONF);

    write_file(TMP_CONF, "port 99999\n");
    DD_CHECK_EQ_INT(-1, config_load_file(&cfg, TMP_CONF));
    remove(TMP_CONF);

    write_file(TMP_CONF, "appendonly maybe\n");
    DD_CHECK_EQ_INT(-1, config_load_file(&cfg, TMP_CONF));
    remove(TMP_CONF);

    write_file(TMP_CONF, "port\n");
    DD_CHECK_EQ_INT(-1, config_load_file(&cfg, TMP_CONF));
    remove(TMP_CONF);
}

static void test_inline_overrides(void)
{
    ddup_config cfg;
    config_init(&cfg);
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "port", "6380"));
    DD_CHECK_EQ_INT(6380, cfg.port);
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "MAXMEMORY", "4096"));
    DD_CHECK_EQ_INT(4096, (long long)cfg.maxmemory);
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "appendonly", "yes"));
    DD_CHECK_EQ_INT(1, cfg.appendonly);
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "save", "30"));
    DD_CHECK_EQ_INT(30, cfg.save_sec);
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "replicaof", "127.0.0.1 6380"));
    DD_CHECK_STR("127.0.0.1", cfg.replicaof_host);
    DD_CHECK_EQ_INT(6380, cfg.replicaof_port);
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "repl-backlog-size", "2097152"));
    DD_CHECK_EQ_INT(2097152, (long long)cfg.repl_backlog_size);
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "proto-max-request-bytes", "128"));
    DD_CHECK_EQ_INT(128, (long long)cfg.proto_max_request_bytes);
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "repl-max-snapshot-bytes", "256"));
    DD_CHECK_EQ_INT(256, (long long)cfg.repl_max_snapshot_bytes);
    DD_CHECK_EQ_INT(-1, config_apply(&cfg, "proto-max-request-bytes",
                                     "18446744073709551616"));
    DD_CHECK_EQ_INT(-1, config_apply(&cfg, "repl-max-snapshot-bytes",
                                     "18446744073709551616"));
    DD_CHECK_EQ_INT(-1, config_apply(&cfg, "replicaof", "nohost"));
    DD_CHECK_EQ_INT(-1, config_apply(&cfg, "replicaof", "host 99999"));
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "tls-port", "6381"));
    DD_CHECK_EQ_INT(6381, cfg.tls_port);
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "tls-cert-file", "c.pem"));
    DD_CHECK_STR("c.pem", cfg.tls_cert_file);
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "tls-key-file", "k.pem"));
    DD_CHECK_STR("k.pem", cfg.tls_key_file);
    DD_CHECK_EQ_INT(-1, config_apply(&cfg, "nonsense", "1"));
    DD_CHECK_EQ_INT(-1, config_apply(&cfg, "port", "0"));
}

static void test_resource_limit_validation(void)
{
    ddup_config cfg;
    char err[256];
    config_init(&cfg);
    cfg.proto_max_request_bytes = 0;
    DD_CHECK_EQ_INT(-1, config_validate(&cfg, err, sizeof(err)));
    DD_CHECK(strstr(err, "proto-max-request-bytes") != NULL);
    config_init(&cfg);
    cfg.repl_max_snapshot_bytes = 0;
    DD_CHECK_EQ_INT(-1, config_validate(&cfg, err, sizeof(err)));
    DD_CHECK(strstr(err, "repl-max-snapshot-bytes") != NULL);
}

static void test_validate_tls(void)
{
    ddup_config cfg;
    char err[256];
    config_init(&cfg);
    /* off by default: always valid */
    DD_CHECK_EQ_INT(0, config_validate(&cfg, err, sizeof(err)));
    /* tls-port without cert/key files fails with a clear message */
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "tls-port", "6381"));
    DD_CHECK_EQ_INT(-1, config_validate(&cfg, err, sizeof(err)));
    DD_CHECK(strstr(err, "tls-cert-file") != NULL);
    /* point at readable files (this test file itself works as content) */
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "tls-cert-file", TMP_CONF));
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "tls-key-file", TMP_CONF));
    write_file(TMP_CONF, "# x\n");
    DD_CHECK_EQ_INT(0, config_validate(&cfg, err, sizeof(err)));
    remove(TMP_CONF);
}

static void test_repl_backlog_size(void)
{
    ddup_config cfg;
    char err[256];

    config_init(&cfg);
    DD_CHECK_EQ_INT(-1, config_apply(&cfg, "repl-backlog-size", "0"));
    DD_CHECK_EQ_INT(-1, config_apply(&cfg, "repl-backlog-size", "-1"));
    DD_CHECK_EQ_INT(-1, config_apply(&cfg, "repl-backlog-size",
                                     "18446744073709551616"));
    DD_CHECK_EQ_INT(1024 * 1024, (long long)cfg.repl_backlog_size);

    cfg.repl_backlog_size = 0;
    DD_CHECK_EQ_INT(-1, config_validate(&cfg, err, sizeof(err)));
    DD_CHECK(strstr(err, "repl-backlog-size") != NULL);
}

int main(void)
{
    DD_RUN(test_defaults);
    DD_RUN(test_file_parse);
    DD_RUN(test_file_errors);
    DD_RUN(test_inline_overrides);
    DD_RUN(test_requirepass);
    DD_RUN(test_io_backend_values);
    DD_RUN(test_io_threads);
    DD_RUN(test_validate_tls);
    DD_RUN(test_repl_backlog_size);
    DD_RUN(test_resource_limit_validation);
    return DD_TEST_SUMMARY();
}
