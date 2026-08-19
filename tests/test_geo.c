/* test_geo.c - geospatial command tests, written before the implementation. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/command.h"
#include "test.h"

#define T0 1000000ULL

static void exec_cmd(db *d, resp_buf *out, int argc, ...)
{
    resp_value argv[16];
    va_list ap;
    int i;
    va_start(ap, argc);
    for (i = 0; i < argc; i++) {
        const char *s = va_arg(ap, const char *);
        memset(&argv[i], 0, sizeof(argv[i]));
        argv[i].type = RESP_BULK_STRING;
        argv[i].str = s;
        argv[i].len = strlen(s);
    }
    va_end(ap);
    out->len = 0;
    command_execute_at(d, argv, (size_t)argc, out, T0);
}

#define EXPECT(out, s) DD_CHECK_MEM((s), strlen(s), (out).data, (out).len)
#define EXPECT_PTR(out, s) DD_CHECK_MEM((s), strlen(s), (out)->data, (out)->len)

static void add_sicily(db *d, resp_buf *out)
{
    exec_cmd(d, out, 8, "GEOADD", "Sicily", "13.361389", "38.115556",
             "Palermo", "15.087269", "37.502669", "Catania");
    EXPECT_PTR(out, ":2\r\n");
}

static void test_geoadd_geopos_geodist(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    add_sicily(&d, &out);

    exec_cmd(&d, &out, 2, "GEOPOS", "Sicily");
    EXPECT(out, "*0\r\n");

    exec_cmd(&d, &out, 4, "GEOPOS", "Sicily", "Palermo", "Catania");
    EXPECT(out,
           "*2\r\n"
           "*2\r\n$20\r\n13.36138933897018433\r\n$20\r\n38.11555639549629859\r\n"
           "*2\r\n$20\r\n15.08726745843887329\r\n$20\r\n37.50266842333162032\r\n");
    exec_cmd(&d, &out, 3, "GEOPOS", "Sicily", "missing");
    EXPECT(out, "*1\r\n*-1\r\n");

    exec_cmd(&d, &out, 4, "GEODIST", "Sicily", "Palermo", "Catania");
    EXPECT(out, "$11\r\n166274.1516\r\n");
    exec_cmd(&d, &out, 5, "GEODIST", "Sicily", "Palermo", "Catania", "km");
    EXPECT(out, "$8\r\n166.2742\r\n");
    exec_cmd(&d, &out, 4, "GEODIST", "Sicily", "missing", "Catania");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, &out, 5, "GEODIST", "Sicily", "Palermo", "Catania", "mi");
    EXPECT(out, "$8\r\n103.3182\r\n");
    exec_cmd(&d, &out, 5, "GEODIST", "Sicily", "Palermo", "Catania", "ft");
    EXPECT(out, "$11\r\n545518.8700\r\n");

    /* NX/XX and CH */
    exec_cmd(&d, &out, 6, "GEOADD", "Sicily", "NX", "12.5", "41.9", "Rome");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, &out, 6, "GEOADD", "Sicily", "NX", "12.5", "41.9", "Rome");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, &out, 7, "GEOADD", "Sicily", "XX", "CH", "12.6", "41.9", "Rome");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, &out, 6, "GEOADD", "Sicily", "XX", "12.6", "41.9", "Rome");
    EXPECT(out, ":0\r\n");

    /* invalid coordinate */
    exec_cmd(&d, &out, 5, "GEOADD", "bad", "181", "0", "m");
    EXPECT(out,
           "-ERR invalid longitude,latitude pair 181.000000,0.000000\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_geohash(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    add_sicily(&d, &out);
    exec_cmd(&d, &out, 4, "GEOHASH", "Sicily", "Palermo", "Catania");
    EXPECT(out,
           "*2\r\n"
           "$11\r\nsqc8b49rny0\r\n"
           "$11\r\nsqdtr74hyu0\r\n");

    exec_cmd(&d, &out, 3, "GEOHASH", "Sicily", "missing");
    EXPECT(out, "*1\r\n$-1\r\n");
    exec_cmd(&d, &out, 2, "GEOHASH", "Sicily");
    EXPECT(out, "*0\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_geosearch_radius(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    add_sicily(&d, &out);

    exec_cmd(&d, &out, 9, "GEOSEARCH", "Sicily", "FROMLONLAT", "15", "37",
             "BYRADIUS", "100", "km", "ASC");
    EXPECT(out, "*1\r\n$7\r\nCatania\r\n");

    exec_cmd(&d, &out, 8, "GEOSEARCH", "Sicily", "FROMMEMBER", "Palermo",
             "BYRADIUS", "200", "km", "ASC");
    EXPECT(out, "*2\r\n$7\r\nPalermo\r\n$7\r\nCatania\r\n");

    exec_cmd(&d, &out, 9, "GEOSEARCHSTORE", "dest", "Sicily", "FROMMEMBER",
             "Palermo", "BYRADIUS", "200", "km", "ASC");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, &out, 4, "ZRANGE", "dest", "0", "-1");
    EXPECT(out, "*2\r\n$7\r\nPalermo\r\n$7\r\nCatania\r\n");
    exec_cmd(&d, &out, 9, "GEOSEARCHSTORE", "dest", "Sicily", "FROMLONLAT",
             "15", "37", "BYRADIUS", "100", "km", "ASC");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, &out, 4, "ZRANGE", "dest", "0", "-1");
    EXPECT(out, "*1\r\n$7\r\nCatania\r\n");

    exec_cmd(&d, &out, 7, "GEOSEARCH", "Sicily", "FROMMEMBER", "missing",
             "BYRADIUS", "200", "km");
    EXPECT(out, "-ERR could not decode requested zset member\r\n");
    exec_cmd(&d, &out, 8, "GEOSEARCHSTORE", "dest", "Sicily", "FROMMEMBER",
             "missing", "BYRADIUS", "200", "km");
    EXPECT(out, "-ERR could not decode requested zset member\r\n");

    exec_cmd(&d, &out, 9, "GEOSEARCHSTORE", "dest2", "Sicily", "FROMMEMBER",
             "Palermo", "BYRADIUS", "200", "km", "STOREDIST");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, &out, 5, "ZRANGE", "dest2", "0", "-1", "WITHSCORES");
    EXPECT(out,
           "*4\r\n$7\r\nPalermo\r\n$1\r\n0\r\n"
           "$7\r\nCatania\r\n$18\r\n166.27415156960041\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_georadius(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    add_sicily(&d, &out);

    exec_cmd(&d, &out, 7, "GEORADIUS", "Sicily", "15", "37", "100", "km", "ASC");
    EXPECT(out, "*1\r\n$7\r\nCatania\r\n");

    exec_cmd(&d, &out, 6, "GEORADIUSBYMEMBER_RO", "Sicily", "Palermo", "200",
             "km", "ASC");
    EXPECT(out, "*2\r\n$7\r\nPalermo\r\n$7\r\nCatania\r\n");

    exec_cmd(&d, &out, 6, "GEORADIUSBYMEMBER_RO", "Sicily", "missing", "200",
             "km", "ASC");
    EXPECT(out, "-ERR could not decode requested zset member\r\n");

    exec_cmd(&d, &out, 8, "GEORADIUS", "Sicily", "15", "37", "200", "km",
             "WITHDIST", "ASC");
    EXPECT(out,
           "*2\r\n"
           "*2\r\n$7\r\nCatania\r\n$7\r\n56.4413\r\n"
           "*2\r\n$7\r\nPalermo\r\n$8\r\n190.4424\r\n");

    exec_cmd(&d, &out, 8, "GEORADIUS", "Sicily", "15", "37", "200", "km",
             "STOREDIST", "store");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, &out, 5, "ZRANGE", "store", "0", "-1", "WITHSCORES");
    EXPECT(out,
           "*4\r\n$7\r\nCatania\r\n$18\r\n56.441257870158054\r\n"
           "$7\r\nPalermo\r\n$18\r\n190.44242984775798\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

int main(void)
{
    DD_RUN(test_geoadd_geopos_geodist);
    DD_RUN(test_geohash);
    DD_RUN(test_geosearch_radius);
    DD_RUN(test_georadius);
    return DD_TEST_SUMMARY();
}
