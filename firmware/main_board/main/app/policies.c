/*
 * The policy library. Every policy is a weight
 * file and a Microduck schema 2 manifest; DoitDuck's own fields sit in its
 * "doitduck" object (label, alpha, mouth_deg, mouth_until, hold_recovery,
 * verified). Two sources:
 *
 *   built-in  robots/<name>.json "policy.policies", weights in models/
 *   imported  data/policies/<name>.json and .bin, from policy.import
 *
 * An imported policy replaces a built-in one of the same name, and a walk or
 * get-up policy (slot "walk" or "stand") replaces the one in that slot.
 * Removing it brings the built-in one back. Imported policies run from the
 * web page only until policy.verify. The table C.p changes under d.lock, and
 * only for the moment of the swap: files are written outside it, so the bus
 * and policy loops never wait on the flash. A policy in use is never replaced
 * or removed.
 */
#include "policies.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "config.h"
#include "duck.h"
#include "event.h"
#include "storage.h"

#define DEFAULT_RAMP_S 2.0

/* Per entry of C.p: its manifest and whether it was imported (its weights are then ours to free). */
static cJSON *manifest[MAX_POLICIES];
static int imported[MAX_POLICIES];
/* One change to the library at a time. */
static os_mutex_t lib_lock = OS_MUTEX_INIT;

static const char *str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static double num(const cJSON *o, const char *key, double fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

/* A claim that is present and differs; an absent one is no evidence (policy-manifest.md). */
static int differs(const cJSON *o, const char *key, double want)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return v && !(cJSON_IsNumber(v) && v->valuedouble == want);
}

/* Checks a manifest against this robot and fills p from it; NULL or an error code. */
static const char *parse(const cJSON *m, struct policy_cfg *p)
{
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(m, "command");
    const cJSON *dd = cJSON_GetObjectItemCaseSensitive(m, "doitduck");
    const char *kind = str(m, "kind"), *slot = str(m, "slot");
    const char *enc = str(cmd, "encoding")[0] ? str(cmd, "encoding") : "constant";
    const double duration = num(m, "duration_s", 0);

    memset(p, 0, sizeof *p);
    if (!api_good_name(str(m, "name"), sizeof p->name)) return "policy_name";
    snprintf(p->name, sizeof p->name, "%s", str(m, "name"));
    if (differs(m, "model_api", 1)) return "policy_model_api";
    if (differs(m, "obs_len", OBS_DIM)) return "policy_obs_len";
    if (differs(m, "action_len", ACT_DIM)) return "policy_action_len";
    if (differs(m, "action_scale", 1)) return "policy_action_scale";
    int compatible = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(config_section("policy"), "compatible_models")) {
        if (cJSON_IsString(e) && !strcmp(e->valuestring, str(cJSON_GetObjectItemCaseSensitive(m, "robot"), "model"))) compatible = 1;
    }
    if (!compatible) return "policy_robot_model";

    p->end_phase = 1;
    if (!strcmp(kind, "perpetual") && !strcmp(enc, "constant") && (!strcmp(slot, "walk") || !strcmp(slot, "stand"))) {
        p->kind = !strcmp(slot, "walk") ? POLICY_GAIT : POLICY_RECOVERY;
    } else if (!strcmp(kind, "episodic") && !strcmp(enc, "constant")) {
        p->kind = POLICY_SKILL;
        p->command = CMD_CONSTANT;
        p->period_s = (float)duration;
    } else if (!strcmp(kind, "episodic") && !strcmp(enc, "phase")) {
        p->kind = POLICY_SKILL;
        p->command = CMD_PHASE;
        p->period_s = (float)num(cmd, "period_s", 0);
        p->end_phase = (float)num(cmd, "end_phase", duration > 0 && p->period_s > 0 ? duration / p->period_s : 1);
    } else if (!strcmp(kind, "scripted") && !strcmp(enc, "posture_flag")) {
        p->kind = POLICY_SKILL;
        p->command = CMD_POSTURE;
        p->ramp_s = (float)num(m, "ramp_s", DEFAULT_RAMP_S);
    } else {
        return "policy_kind";
    }
    if (p->kind == POLICY_SKILL && p->command != CMD_POSTURE && !(p->period_s > 0 && p->end_phase > 0)) return "policy_duration";

    /* Trained with no action filter: run with none, whatever the user's smoothing. */
    const cJSON *filter = cJSON_GetObjectItemCaseSensitive(m, "action_filter");
    p->alpha = (float)num(dd, "alpha", cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(filter, "enabled")) ? 0 : -1);
    p->mouth_deg = (float)num(dd, "mouth_deg", 0);
    p->mouth_until = (float)num(dd, "mouth_until", 0);
    p->hold_recovery = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(dd, "hold_recovery"));
    p->verified = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(dd, "verified"));
    /* Trained around another HOME pose than the robot's: degrees, in motor order. */
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(dd, "home_deg");
    memcpy(p->home, C.home, sizeof p->home);
    if (h) {
        if (!cJSON_IsArray(h) || cJSON_GetArraySize(h) != C.n) return "policy_home";
        for (int i = 0; i < C.n; i++) {
            const cJSON *v = cJSON_GetArrayItem(h, i);
            if (!cJSON_IsNumber(v)) return "policy_home";
            p->home[i] = (float)v->valuedouble;
        }
    }
    return NULL;
}

/* The entry a new policy takes the place of: the same name, or for walk and get-up the same slot; -1 for none. */
static int replaces(const struct policy_cfg *p)
{
    for (int i = 0; i < C.np; i++) {
        if (!C.p[i].name[0]) continue;
        if (!strcmp(C.p[i].name, p->name) || (p->kind != POLICY_SKILL && C.p[i].kind == p->kind)) return i;
    }
    return -1;
}

static int in_use(int k)
{
    return d.infer_policy == k || d.skill == k ||
           (C.p[k].kind != POLICY_SKILL && (d.mode == MODE_PREVIEW || d.mode == MODE_WALK));
}

static void find_base(void)
{
    C.gait = C.recovery = -1;
    for (int i = 0; i < C.np; i++) {
        if (C.p[i].name[0] && C.p[i].kind == POLICY_GAIT) C.gait = i;
        if (C.p[i].name[0] && C.p[i].kind == POLICY_RECOVERY) C.recovery = i;
    }
}

/* Puts p into entry k (-1: a free one), with d.lock held; the old manifest is freed, the old weights returned. */
static const void *put(int k, const struct policy_cfg *p, cJSON *m, int from_user)
{
    const void *old = NULL;
    if (k < 0) {
        for (k = 0; k < C.np && C.p[k].name[0]; k++) {}
        if (k == C.np) C.np++;
    } else if (imported[k]) {
        old = C.p[k].mlp;
    }
    cJSON_Delete(manifest[k]);
    C.p[k] = *p;
    manifest[k] = m;
    imported[k] = from_user;
    find_base();
    return old;
}

static int has_free(void)
{
    for (int i = 0; i < C.np; i++) {
        if (!C.p[i].name[0]) return 1;
    }
    return C.np < MAX_POLICIES;
}

/* The built-in policies whose name and slot no imported one has taken. */
static void load_builtin(void)
{
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(config_section("policy"), "policies")) {
        struct policy_cfg p;
        const char *err = parse(e, &p);
        if (err) {
            printf("policy %s: %s\n", str(e, "name"), err);
            continue;
        }
        os_lock(&d.lock);
        const int taken = replaces(&p) >= 0 || !has_free();
        os_unlock(&d.lock);
        if (taken) continue;
        char path[64];
        size_t size = 0;
        snprintf(path, sizeof path, "models/%s", str(e, "file"));
        p.mlp = strchr(str(e, "file"), '/') ? NULL : store_blob(path, &size);
        if (!p.mlp || size != policy_size()) {
            printf("policy %s: cannot load %s (%lu bytes expected); import it from the web page\n", p.name, path,
                   policy_size());
            continue;
        }
        p.verified = 1; /* chosen by whoever wrote the robot file */
        os_lock(&d.lock);
        put(-1, &p, cJSON_Duplicate(e, 1), 0);
        os_unlock(&d.lock);
    }
}

static void load_imported(void)
{
    char names[MAX_POLICIES * 2][32];
    const int n = store_list("policies", names, MAX_POLICIES * 2);
    for (int i = 0; i < n; i++) {
        char path[64];
        const size_t len = strlen(names[i]);
        if (len < 6 || strcmp(names[i] + len - 5, ".json")) continue;
        snprintf(path, sizeof path, "policies/%.31s", names[i]);
        char *text = store_get(path, NULL);
        cJSON *m = text ? cJSON_Parse(text) : NULL;
        free(text);
        struct policy_cfg p;
        const char *err = m ? parse(m, &p) : "policy_manifest";
        size_t size = 0;
        snprintf(path, sizeof path, "policies/%s.bin", str(m, "name"));
        p.mlp = err ? NULL : store_get(path, &size);
        if (!err && size != policy_size()) err = "policy_size";
        os_lock(&d.lock);
        if (!err && (replaces(&p) >= 0 || !has_free())) err = "policy_duplicate";
        if (!err) put(-1, &p, m, 1);
        os_unlock(&d.lock);
        if (err) {
            printf("policy %s: %s\n", names[i], err);
            free((void *)p.mlp);
            cJSON_Delete(m);
        }
    }
}

void policies_load(void)
{
    find_base();
    load_imported();
    load_builtin();
    printf("policies:");
    for (int i = 0; i < C.np; i++) {
        if (C.p[i].name[0]) printf(" %s%s", C.p[i].name, imported[i] ? "*" : "");
    }
    printf("%s\n", C.gait < 0 || C.recovery < 0 ? "; no walk or get-up policy: import them from the web page" : "");
}

static cJSON *label(int k)
{
    const cJSON *l = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(manifest[k], "doitduck"), "label");
    return l ? cJSON_Duplicate(l, 1) : cJSON_CreateString(C.p[k].name);
}

void policies_skills(cJSON *a)
{
    os_lock(&d.lock);
    for (int i = 0; i < C.np; i++) {
        if (!C.p[i].name[0] || C.p[i].kind != POLICY_SKILL) continue;
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "name", C.p[i].name);
        cJSON_AddItemToObject(s, "label", label(i));
        cJSON_AddBoolToObject(s, "verified", C.p[i].verified);
        cJSON_AddBoolToObject(s, "posture", C.p[i].command == CMD_POSTURE);
        cJSON_AddItemToArray(a, s);
    }
    os_unlock(&d.lock);
}

static int find(const char *name)
{
    for (int i = 0; i < C.np; i++) {
        if (C.p[i].name[0] && !strcmp(C.p[i].name, name)) return i;
    }
    return -1;
}

static void save(const char *name, const char *text)
{
    char path[64];
    snprintf(path, sizeof path, "policies/%s.json", name);
    store_put(path, text, strlen(text));
}

static void remove_files(const char *name)
{
    char path[64];
    snprintf(path, sizeof path, "policies/%s.json", name);
    store_remove(path);
    snprintf(path, sizeof path, "policies/%s.bin", name);
    store_remove(path);
}

static const char *policy_list(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)p;
    cJSON *list = cJSON_AddArrayToObject(r, "policies");
    os_lock(&d.lock);
    for (int i = 0; i < C.np; i++) {
        if (!C.p[i].name[0]) continue;
        const cJSON *m = manifest[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", C.p[i].name);
        cJSON_AddItemToObject(o, "label", label(i));
        cJSON_AddStringToObject(o, "kind", str(m, "kind"));
        cJSON_AddStringToObject(o, "slot", str(m, "slot"));
        cJSON_AddStringToObject(o, "encoding", str(cJSON_GetObjectItemCaseSensitive(m, "command"), "encoding"));
        cJSON_AddStringToObject(o, "description", str(m, "description"));
        cJSON_AddStringToObject(o, "source", imported[i] ? "imported" : "builtin");
        cJSON_AddBoolToObject(o, "verified", C.p[i].verified);
        cJSON_AddBoolToObject(o, "in_use", in_use(i));
        cJSON_AddItemToArray(list, o);
    }
    os_unlock(&d.lock);
    cJSON_AddItemToObject(r, "compatible_models",
                          cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(config_section("policy"), "compatible_models"), 1));
    return NULL;
}

/*
 * {name, manifest, weights (base64 of the float32 file), confirm}. Replacing
 * the walk or get-up policy needs confirm: true. The weights are checked by
 * size only; the web page and tools/import_policy.py convert and check the ONNX.
 */
/* Whether p may take the place of entry k (from replaces); NULL or an error code. With d.lock held. */
static const char *may_replace(int k, const struct policy_cfg *p, const cJSON *params)
{
    if (k >= 0 && in_use(k)) return "policy_in_use";
    if (k >= 0 && C.p[k].kind != p->kind) return "policy_name";
    if (k >= 0 && p->kind != POLICY_SKILL && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params, "confirm")))
        return "policy_replace_base";
    if (k < 0 && !has_free()) return "policy_full";
    return NULL;
}

/*
 * {name, manifest, weights (base64 of the float32 file), confirm}. Replacing
 * the walk or get-up policy needs confirm: true. The weights are checked by
 * size only; the web page and tools/import_policy.py convert and check the ONNX.
 */
static const char *policy_import(int src, const cJSON *params, cJSON *r)
{
    (void)src;
    const cJSON *weights = cJSON_GetObjectItemCaseSensitive(params, "weights");
    const cJSON *given = cJSON_GetObjectItemCaseSensitive(params, "manifest");
    if (!cJSON_IsObject(given) || !cJSON_IsString(weights)) return "policy_manifest";
    cJSON *m = cJSON_Duplicate(given, 1);
    if (str(params, "name")[0]) {
        cJSON_DeleteItemFromObject(m, "name");
        cJSON_AddStringToObject(m, "name", str(params, "name"));
    }
    /* Nothing imported is verified yet, whatever the file says. */
    cJSON *dd = cJSON_GetObjectItemCaseSensitive(m, "doitduck");
    if (!cJSON_IsObject(dd)) {
        cJSON_DeleteItemFromObject(m, "doitduck");
        dd = cJSON_AddObjectToObject(m, "doitduck");
    }
    cJSON_DeleteItemFromObject(dd, "verified");
    struct policy_cfg p;
    const char *err = parse(m, &p);
    uint8_t *w = err ? NULL : malloc(policy_size() + 3);
    if (!err && (!w || api_base64_decode(weights->valuestring, w, policy_size() + 3) != (int)policy_size())) err = "policy_size";
    if (err) {
        free(w);
        cJSON_Delete(m);
        return err;
    }
    p.mlp = w;

    os_lock(&lib_lock);
    os_lock(&d.lock);
    const int k = replaces(&p);
    err = may_replace(k, &p, params);
    char old_name[sizeof p.name] = "";
    if (k >= 0) snprintf(old_name, sizeof old_name, "%s", C.p[k].name);
    const int old_imported = k >= 0 && imported[k];
    os_unlock(&d.lock);

    char bin[64], *text = cJSON_Print(m);
    snprintf(bin, sizeof bin, "policies/%s.bin", p.name);
    if (!err && store_put(bin, w, policy_size()) != 0) err = "policy_storage";
    if (!err) save(p.name, text);
    free(text);
    const void *old = NULL;
    if (!err) {
        /* Checked again: the duck may have started using the old one meanwhile. */
        os_lock(&d.lock);
        err = replaces(&p) == k ? may_replace(k, &p, params) : "policy_in_use";
        if (!err) old = put(k, &p, m, 1);
        os_unlock(&d.lock);
        if (!err && old_imported && strcmp(old_name, p.name)) remove_files(old_name);
    }
    os_unlock(&lib_lock);
    if (err) {
        free(w);
        cJSON_Delete(m);
        return err;
    }
    free((void *)old);
    cJSON_AddStringToObject(r, "name", p.name);
    if (k >= 0) cJSON_AddStringToObject(r, "replaced", old_name);
    event_post(EV_POLICIES_CHANGED, p.name);
    return NULL;
}

static const char *policy_remove(int src, const cJSON *params, cJSON *r)
{
    (void)src;
    (void)r;
    const char *err = NULL;
    const void *old = NULL;
    os_lock(&lib_lock);
    os_lock(&d.lock);
    const int k = find(str(params, "name"));
    if (k < 0) err = "policy_not_found";
    else if (!imported[k]) err = "policy_builtin";
    else if (in_use(k)) err = "policy_in_use";
    if (!err) {
        old = C.p[k].mlp;
        cJSON_Delete(manifest[k]);
        manifest[k] = NULL;
        imported[k] = 0;
        memset(&C.p[k], 0, sizeof C.p[k]);
        find_base();
    }
    os_unlock(&d.lock);
    if (!err) {
        remove_files(str(params, "name"));
        free((void *)old);
        load_builtin();
    }
    os_unlock(&lib_lock);
    if (!err) event_post(EV_POLICIES_CHANGED, str(params, "name"));
    return err;
}

static const char *policy_verify(int src, const cJSON *params, cJSON *r)
{
    (void)src;
    (void)r;
    const char *err = NULL;
    char *text = NULL;
    os_lock(&lib_lock);
    os_lock(&d.lock);
    const int k = find(str(params, "name"));
    if (k < 0) {
        err = "policy_not_found";
    } else if (!C.p[k].verified) {
        C.p[k].verified = 1;
        cJSON *dd = cJSON_GetObjectItemCaseSensitive(manifest[k], "doitduck");
        if (!dd) dd = cJSON_AddObjectToObject(manifest[k], "doitduck");
        cJSON_DeleteItemFromObject(dd, "verified");
        cJSON_AddTrueToObject(dd, "verified");
        text = cJSON_Print(manifest[k]);
    }
    os_unlock(&d.lock);
    if (text) save(str(params, "name"), text);
    os_unlock(&lib_lock);
    free(text);
    if (!err) event_post(EV_POLICIES_CHANGED, str(params, "name"));
    return err;
}

const struct api_method POLICY_METHODS[] = {
    {"policy.list", policy_list, 0},
    {"policy.import", policy_import, 1},
    {"policy.remove", policy_remove, 1},
    {"policy.verify", policy_verify, 1},
};
const int NPOLICY_METHODS = sizeof POLICY_METHODS / sizeof POLICY_METHODS[0];
