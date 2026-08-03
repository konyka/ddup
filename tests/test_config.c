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
    DD_CHECK_EQ_INT(-1, config_apply(&cfg, "nonsense", "1"));
    DD_CHECK_EQ_INT(-1, config_apply(&cfg, "port", "0"));
}

int main(void)
{
    DD_RUN(test_defaults);
    DD_RUN(test_file_parse);
    DD_RUN(test_file_errors);
    DD_RUN(test_inline_overrides);
    return DD_TEST_SUMMARY();
}
