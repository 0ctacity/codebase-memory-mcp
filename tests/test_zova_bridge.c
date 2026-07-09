#include "test_framework.h"

#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h"
#include "../src/foundation/platform.h"
#include "store/store.h"
#include "zova/cbm_zova.h"
#include "zova/cbm_zova_bridge.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef CBM_WITH_ZOVA
#define CBM_WITH_ZOVA 0
#endif

enum { TZB_PATH_MAX = 512 };

typedef struct {
    char db_path[TZB_PATH_MAX];
    char zova_path[TZB_PATH_MAX];
} bridge_fixture_t;

static int bridge_fixture_create(bridge_fixture_t *fx) {
    memset(fx, 0, sizeof(*fx));
    snprintf(fx->db_path, sizeof(fx->db_path), "%s/cbm_zova_bridge_%d_%p.db", cbm_tmpdir(),
             (int)getpid(), (void *)fx);
    ASSERT_EQ(cbm_zova_sidecar_path(fx->db_path, fx->zova_path, sizeof(fx->zova_path)), 0);
    cbm_unlink(fx->db_path);
    cbm_unlink(fx->zova_path);
    cbm_store_t *store = cbm_store_open_path(fx->db_path);
    ASSERT_NOT_NULL(store);
    ASSERT_EQ(cbm_store_upsert_project(store, "proj", "/tmp/proj"), CBM_STORE_OK);
    cbm_store_close(store);
    cbm_setenv("CBM_ZOVA_MODE", "container", 1);
    ASSERT_EQ(cbm_zova_after_sqlite_dump(fx->db_path), 0);
    cbm_unsetenv("CBM_ZOVA_MODE");
    return 0;
}

static void bridge_fixture_cleanup(bridge_fixture_t *fx) {
    cbm_unlink(fx->db_path);
    cbm_unlink(fx->zova_path);
    char tmp[TZB_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp.zova", fx->zova_path);
    cbm_unlink(tmp);
}

#if !CBM_WITH_ZOVA

TEST(zova_bridge_disabled) {
    ASSERT_EQ(cbm_zova_bridge_build_enabled(), 0);
    PASS();
}

#else

TEST(zova_bridge_installs_extension) {
    bridge_fixture_t fx;
    ASSERT_EQ(bridge_fixture_create(&fx), 0);
    ASSERT_EQ(cbm_zova_bridge_build_enabled(), 1);
    ASSERT_EQ(cbm_zova_bridge_install_extension(fx.zova_path), 0);
    bridge_fixture_cleanup(&fx);
    PASS();
}

TEST(zova_bridge_cosine_matches_cbm_semantics) {
    bridge_fixture_t fx;
    ASSERT_EQ(bridge_fixture_create(&fx), 0);
    ASSERT_EQ(cbm_zova_bridge_install_extension(fx.zova_path), 0);
    const int8_t a[] = {1, 0};
    const int8_t b[] = {1, 0};
    const int8_t c[] = {0, 1};
    const int8_t z[] = {0, 0};
    double score = -1.0;
    ASSERT_EQ(cbm_zova_bridge_cosine_smoke(fx.zova_path, a, b, 2, &score), 0);
    ASSERT_FLOAT_EQ(score, 1.0, 0.000001);
    ASSERT_EQ(cbm_zova_bridge_cosine_smoke(fx.zova_path, a, c, 2, &score), 0);
    ASSERT_FLOAT_EQ(score, 0.0, 0.000001);
    ASSERT_EQ(cbm_zova_bridge_cosine_smoke(fx.zova_path, a, z, 2, &score), 0);
    ASSERT_FLOAT_EQ(score, 0.0, 0.000001);
    ASSERT_EQ(cbm_zova_bridge_cosine_smoke(fx.zova_path, a, b, 0, &score), 0);
    ASSERT_FLOAT_EQ(score, 0.0, 0.000001);
    bridge_fixture_cleanup(&fx);
    PASS();
}

TEST(zova_bridge_camel_split_matches_cbm_examples) {
    bridge_fixture_t fx;
    ASSERT_EQ(bridge_fixture_create(&fx), 0);
    ASSERT_EQ(cbm_zova_bridge_install_extension(fx.zova_path), 0);
    char out[256];
    ASSERT_EQ(cbm_zova_bridge_camel_smoke(fx.zova_path, "updateCloudClient", out, sizeof(out)),
              0);
    ASSERT_STR_EQ(out, "updateCloudClient update Cloud Client");
    ASSERT_EQ(cbm_zova_bridge_camel_smoke(fx.zova_path, "XMLParser", out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "XMLParser XML Parser");
    ASSERT_EQ(cbm_zova_bridge_camel_smoke(fx.zova_path, "", out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "");
    bridge_fixture_cleanup(&fx);
    PASS();
}

TEST(zova_bridge_regex_matches_cbm_examples) {
    bridge_fixture_t fx;
    ASSERT_EQ(bridge_fixture_create(&fx), 0);
    ASSERT_EQ(cbm_zova_bridge_install_extension(fx.zova_path), 0);
    int matched = -1;
    ASSERT_EQ(cbm_zova_bridge_regex_smoke(fx.zova_path, "^Foo", "FooBar", 0, &matched), 0);
    ASSERT_EQ(matched, 1);
    ASSERT_EQ(cbm_zova_bridge_regex_smoke(fx.zova_path, "^foo", "FooBar", 0, &matched), 0);
    ASSERT_EQ(matched, 0);
    ASSERT_EQ(cbm_zova_bridge_regex_smoke(fx.zova_path, "^foo", "FooBar", 1, &matched), 0);
    ASSERT_EQ(matched, 1);
    char err[128];
    ASSERT_EQ(cbm_zova_bridge_invalid_regex_smoke(fx.zova_path, err, sizeof(err)), 0);
    ASSERT_NOT_NULL(strstr(err, "invalid regex"));
    bridge_fixture_cleanup(&fx);
    PASS();
}

#endif

SUITE(zova_bridge) {
#if !CBM_WITH_ZOVA
    RUN_TEST(zova_bridge_disabled);
#else
    RUN_TEST(zova_bridge_installs_extension);
    RUN_TEST(zova_bridge_cosine_matches_cbm_semantics);
    RUN_TEST(zova_bridge_camel_split_matches_cbm_examples);
    RUN_TEST(zova_bridge_regex_matches_cbm_examples);
#endif
}
