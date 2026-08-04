/* test_cluster_id.c - cluster node identity (generation + persistence). */
#include <stdio.h>
#include <string.h>

#include "core/cluster.h"
#include "core/config.h"
#include "test.h"

#define TMP_NODES "test_cluster_nodes.conf"

static int is_hex40(const char *s)
{
    int i;
    for (i = 0; i < 40; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return 0;
    }
    return s[40] == '\0' || s[40] == ' ';
}

static void test_config_defaults(void)
{
    ddup_config cfg;
    config_init(&cfg);
    DD_CHECK_EQ_INT(0, cfg.cluster_enabled);
    DD_CHECK_STR("nodes.conf", cfg.cluster_config_file);
}

static void test_config_parse(void)
{
    ddup_config cfg;
    config_init(&cfg);
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "cluster-enabled", "yes"));
    DD_CHECK_EQ_INT(1, cfg.cluster_enabled);
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "cluster-enabled", "no"));
    DD_CHECK_EQ_INT(0, cfg.cluster_enabled);
    DD_CHECK_EQ_INT(-1, config_apply(&cfg, "cluster-enabled", "maybe"));
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "cluster-config-file", "n.conf"));
    DD_CHECK_STR("n.conf", cfg.cluster_config_file);
}

static void test_id_generate_and_persist(void)
{
    char id1[41], id2[41];
    FILE *f;
    char line[128];

    remove(TMP_NODES);
    DD_CHECK_EQ_INT(0, cluster_node_id_load_or_create(TMP_NODES, id1));
    DD_CHECK(is_hex40(id1));

    /* file was written, Redis-style minimal */
    f = fopen(TMP_NODES, "rb");
    DD_CHECK(f != NULL);
    DD_CHECK(fgets(line, sizeof(line), f) != NULL);
    fclose(f);
    DD_CHECK(strstr(line, "myself,master") != NULL);
    DD_CHECK(strstr(line, "connected 0-16383") != NULL);

    /* simulated reboot: same id comes back */
    DD_CHECK_EQ_INT(0, cluster_node_id_load_or_create(TMP_NODES, id2));
    DD_CHECK_STR(id1, id2);

    remove(TMP_NODES);
}

static void test_id_bad_file(void)
{
    FILE *f = fopen(TMP_NODES, "wb");
    DD_CHECK(f != NULL);
    fputs("garbage-not-an-id\n", f);
    fclose(f);
    {
        char id[41];
        DD_CHECK_EQ_INT(-1, cluster_node_id_load_or_create(TMP_NODES, id));
    }
    remove(TMP_NODES);
}

int main(void)
{
    DD_RUN(test_config_defaults);
    DD_RUN(test_config_parse);
    DD_RUN(test_id_generate_and_persist);
    DD_RUN(test_id_bad_file);
    return DD_TEST_SUMMARY();
}
