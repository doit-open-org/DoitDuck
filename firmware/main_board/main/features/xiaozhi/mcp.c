/*
 * MCP tools for the LLM. Every tool calls the command interface as SRC_LLM, so
 * the arbiter gives it the lowest priority and the slowest speeds, and a walk
 * always ends by itself. Descriptions are Chinese, written for the model.
 */
#include "mcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "camera.h"
#include "config.h"
#include "duck.h"
#include "sound.h"
#include "xiaozhi.h"

#define PHOTO_MAX (512 * 1024)

/* Where take_photo sends the picture, given by the server in initialize. */
static char vision_url[256], vision_token[256];

static const char *call(const char *method, cJSON *params)
{
    const char *err = NULL;
    cJSON_Delete(api_call(SRC_LLM, method, params, &err));
    cJSON_Delete(params);
    return err;
}

static const char *set_mode(const char *mode)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "mode", mode);
    return call("robot.set_mode", p);
}

static const char *arg_str(const cJSON *args, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static double arg_num(const cJSON *args, const char *key, double fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

static const char *tool_status(const cJSON *args, char *text, size_t n)
{
    (void)args;
    os_lock(&d.lock);
    snprintf(text, n, "{\"mode\":\"%s\",\"standing\":%s,\"fallen\":%s,\"action\":\"%s\",\"battery_v\":%.2f}",
             MODE_NAMES[d.mode], d.enabled ? "true" : "false", d.phase != PHASE_WALK ? "true" : "false",
             d.skill >= 0 ? C.p[d.skill].name : "", d.battery_mv / 1000.0);
    os_unlock(&d.lock);
    return NULL;
}

/* The network as JSON: {connected, ssid, ip, url, ap, ap_ssid, ap_url, mac}; url is the web console on the
 * Wi-Fi ("" when not on one), ap_url the one on the hotspot ("" when it is closed). */
static const char *tool_network(const cJSON *args, char *text, size_t n)
{
    (void)args;
    const char *err = NULL;
    cJSON *s = api_call(SRC_LLM, "wifi.status", NULL, &err);
    if (err) return err;
    const char *ip = arg_str(s, "ip");
    const int connected = ip[0] && strcmp(ip, "0.0.0.0");
    const int ap = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(s, "ap"));
    char url[48] = "", mac[18];
    if (connected) snprintf(url, sizeof url, "http://%s", ip);
    os_device_id(mac);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "connected", connected);
    cJSON_AddStringToObject(o, "ssid", connected ? arg_str(s, "ssid") : "");
    cJSON_AddStringToObject(o, "ip", connected ? ip : "");
    cJSON_AddStringToObject(o, "url", url);
    cJSON_AddBoolToObject(o, "ap", ap);
    cJSON_AddStringToObject(o, "ap_ssid", ap ? arg_str(s, "ap_ssid") : "");
    cJSON_AddStringToObject(o, "ap_url", ap ? "http://192.168.4.1" : "");
    cJSON_AddStringToObject(o, "mac", mac);
    cJSON_PrintPreallocated(o, text, (int)n, 0);
    cJSON_Delete(o);
    cJSON_Delete(s);
    return NULL;
}

/*
 * As xiaozhi-esp32's self.get_device_status, with what the duck has: the speaker's volume, the battery (2S,
 * low_battery_v empty, 8.4 V full; when the IMU board gives it), the network, the chip's temperature. No screen.
 */
static const char *tool_device(const cJSON *args, char *text, size_t n)
{
    (void)args;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(cJSON_AddObjectToObject(o, "audio_speaker"), "volume", sound_volume());
    os_lock(&d.lock);
    const int mv = d.battery_mv;
    os_unlock(&d.lock);
    if (mv > 0) {
        const int level = (mv - C.low_battery_mv) * 100 / (8400 - C.low_battery_mv);
        cJSON *b = cJSON_AddObjectToObject(o, "battery");
        cJSON_AddNumberToObject(b, "level", level < 0 ? 0 : level > 100 ? 100 : level);
        cJSON_AddNumberToObject(b, "voltage", mv / 1000.0);
    }
    const char *err = NULL;
    cJSON *s = api_call(SRC_LLM, "wifi.status", NULL, &err);
    if (!err) {
        const int rssi = (int)arg_num(s, "rssi", 0);
        cJSON *w = cJSON_AddObjectToObject(o, "network");
        cJSON_AddStringToObject(w, "type", "wifi");
        cJSON_AddStringToObject(w, "ssid", arg_str(s, "ssid"));
        if (rssi) cJSON_AddStringToObject(w, "signal", rssi >= -60 ? "strong" : rssi >= -70 ? "medium" : "weak");
    }
    cJSON_Delete(s);
    /* The SoC's temperature, from the power manager's "cur_temp:" line. */
    char line[256];
    FILE *f = fopen("/proc/umap/pm", "r");
    while (f && fgets(line, sizeof line, f)) {
        const char *t = strstr(line, "cur_temp:");
        if (t) cJSON_AddNumberToObject(cJSON_AddObjectToObject(o, "chip"), "temperature", atoi(t + 9));
        if (t) break;
    }
    if (f) fclose(f);
    cJSON_PrintPreallocated(o, text, (int)n, 0);
    cJSON_Delete(o);
    return NULL;
}

static const char *tool_volume(const cJSON *args, char *text, size_t n)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, "volume");
    if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 100) return "volume 是 0 到 100 的整数";
    sound_set_volume(v->valueint);
    snprintf(text, n, "音量已调到 %d", sound_volume());
    return NULL;
}

static const char *tool_stand(const cJSON *args, char *text, size_t n)
{
    (void)args;
    snprintf(text, n, "已站好");
    /* Stands under the walk policy, ready to move, not in the fixed pose of hold. */
    os_lock(&d.lock);
    const int walking = d.mode == MODE_WALK;
    os_unlock(&d.lock);
    return walking ? call("robot.stop", NULL) : set_mode("walk");
}

static const char *tool_rest(const cJSON *args, char *text, size_t n)
{
    (void)args;
    snprintf(text, n, "已趴下放松");
    return call("robot.estop", NULL);
}

static const char *move(double vx, double vy, double wz, double seconds)
{
    os_lock(&d.lock);
    const int walking = d.mode == MODE_WALK;
    os_unlock(&d.lock);
    const char *err = walking ? NULL : set_mode("walk");
    if (err) return err;
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "vx", vx);
    cJSON_AddNumberToObject(p, "vy", vy);
    cJSON_AddNumberToObject(p, "wz", wz);
    cJSON_AddNumberToObject(p, "duration", seconds < 1 ? 1 : seconds > 5 ? 5 : seconds);
    return call("robot.move", p);
}

static const char *tool_walk(const cJSON *args, char *text, size_t n)
{
    const char *dir = arg_str(args, "direction");
    const double s = arg_num(args, "seconds", 2);
    snprintf(text, n, "开始移动 %.0f 秒", s);
    if (!strcmp(dir, "forward")) return move(0.2, 0, 0, s);
    if (!strcmp(dir, "backward")) return move(-0.2, 0, 0, s);
    if (!strcmp(dir, "left")) return move(0, 0.2, 0, s);
    if (!strcmp(dir, "right")) return move(0, -0.2, 0, s);
    return "direction 只能是 forward、backward、left、right";
}

static const char *tool_turn(const cJSON *args, char *text, size_t n)
{
    const char *dir = arg_str(args, "direction");
    const double s = arg_num(args, "seconds", 2);
    snprintf(text, n, "开始转向 %.0f 秒", s);
    if (!strcmp(dir, "left")) return move(0, 0, 0.8, s);
    if (!strcmp(dir, "right")) return move(0, 0, -0.8, s);
    return "direction 只能是 left、right";
}

static const char *tool_stop(const cJSON *args, char *text, size_t n)
{
    (void)args;
    snprintf(text, n, "已停下");
    return call("robot.stop", NULL);
}

static const char *tool_action(const cJSON *args, char *text, size_t n)
{
    os_lock(&d.lock);
    const int walking = d.mode == MODE_WALK;
    os_unlock(&d.lock);
    const char *err = walking ? NULL : set_mode("walk");
    if (err) return err;
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "name", arg_str(args, "name"));
    snprintf(text, n, "开始执行动作");
    return call("robot.skill", p);
}

static const char *tool_look(const cJSON *args, char *text, size_t n)
{
    const cJSON *preset = cJSON_GetObjectItemCaseSensitive(config_section("head_presets"), arg_str(args, "direction"));
    if (cJSON_GetArraySize(preset) != 4) return "direction 只能是 up、down、left、right、front";
    cJSON *p = cJSON_CreateObject();
    cJSON_AddItemToObject(p, "head", cJSON_Duplicate(preset, 1));
    snprintf(text, n, "已转头");
    return call("robot.head", p);
}

static const char *tool_mouth(const cJSON *args, char *text, size_t n)
{
    const int open = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "open"));
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "deg", open ? C.mouth_max : 0);
    snprintf(text, n, open ? "已张嘴" : "已闭嘴");
    return call("robot.mouth", p);
}

/* A quack for a mood: "normal" is quack, the others quack_<mood>. */
static const char *tool_quack(const cJSON *args, char *text, size_t n)
{
    const char *mood = arg_str(args, "mood");
    char name[32];
    snprintf(name, sizeof name, !mood[0] || !strcmp(mood, "normal") ? "quack" : "quack_%s", mood);
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "name", name);
    snprintf(text, n, "嘎！");
    return call("robot.sound", p);
}

/* The photo goes to the server's vision model with the question; its answer is the tool's result. */
static const char *tool_photo(const cJSON *args, char *text, size_t n)
{
    if (!vision_url[0]) return "服务器没有提供图像识别接口";
    uint8_t *jpeg = malloc(PHOTO_MAX);
    const int len = jpeg ? camera_jpeg(jpeg, PHOTO_MAX) : -1;
    const int status = len > 0 ? xiaozhi_explain(vision_url, vision_token, arg_str(args, "question"), jpeg,
                                                 (size_t)len, text, n)
                               : 0;
    free(jpeg);
    if (len <= 0) return "摄像头没有打开，拍不到照片";
    if (status != 200) return "照片上传失败";
    return NULL;
}

#define SECONDS "\"seconds\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":5,\"description\":\"持续秒数\"}"

static const struct {
    const char *name, *description, *schema;
    const char *(*fn)(const cJSON *args, char *text, size_t n);
} TOOLS[] = {
    {"self.get_device_status",
     "查询设备的实时信息：扬声器音量（0-100）、电池电量、网络（Wi-Fi 名称和信号强弱）、芯片温度。"
     "用户问音量多大、电量还剩多少、Wi-Fi 信号好不好、设备热不热时调用；调音量前先用它查当前音量。",
     "{\"type\":\"object\",\"properties\":{}}", tool_device},
    {"self.audio_speaker.set_volume",
     "设置扬声器音量，0 到 100，100 最大。用户说大声点、小声点时，先调用 self.get_device_status 查到当前音量，"
     "再在它的基础上加减 10 到 20。",
     "{\"type\":\"object\",\"properties\":{\"volume\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}},"
     "\"required\":[\"volume\"]}", tool_volume},
    {"self.duck.get_status", "查询机器鸭当前状态：模式、是否上力站立、是否摔倒、正在做的动作、电池电压。",
     "{\"type\":\"object\",\"properties\":{}}", tool_status},
    {"self.duck.get_network", "查询机器鸭的网络信息：连的 Wi-Fi、IP 地址、网页控制台地址、热点是否打开、MAC 地址。"
                           "用户问网络、Wi-Fi、IP、控制台、网页、怎么用手机或电脑控制时调用。"
                           "返回 connected 是否连上 Wi-Fi，ssid Wi-Fi 名，ip，url 同一 Wi-Fi 下的控制台地址，"
                           "ap 热点是否打开，ap_ssid 热点名，ap_url 连上热点后的控制台地址，mac。"
                           "没连上 Wi-Fi 时告诉用户说「开始配网」。",
     "{\"type\":\"object\",\"properties\":{}}", tool_network},
    {"self.duck.stand", "让机器鸭上力站起来，进入行走模式原地站着。走路、做动作前不需要先调用，相关工具会自动进入行走模式。",
     "{\"type\":\"object\",\"properties\":{}}", tool_stand},
    {"self.duck.rest", "让机器鸭趴下休息：所有舵机卸力。用户让它休息、睡觉、放松时调用。",
     "{\"type\":\"object\",\"properties\":{}}", tool_rest},
    {"self.duck.walk", "让机器鸭朝一个方向走几秒，到时间自动停下。forward 前进，backward 后退，left 向左平移，right 向右平移。",
     "{\"type\":\"object\",\"properties\":{\"direction\":{\"type\":\"string\",\"enum\":[\"forward\",\"backward\",\"left\",\"right\"]}," SECONDS
     "},\"required\":[\"direction\"]}", tool_walk},
    {"self.duck.turn", "让机器鸭原地向左或向右转几秒，到时间自动停下。",
     "{\"type\":\"object\",\"properties\":{\"direction\":{\"type\":\"string\",\"enum\":[\"left\",\"right\"]}," SECONDS
     "},\"required\":[\"direction\"]}", tool_turn},
    {"self.duck.stop", "立即停止移动，原地站着。", "{\"type\":\"object\",\"properties\":{}}", tool_stop},
    /* The description and the name enum are filled from the robot's skills in tools/list. */
    {"self.duck.do_action", "让机器鸭做一个动作，做完自动回到站立行走。",
     "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"enum\":[]}},\"required\":[\"name\"]}",
     tool_action},
    {"self.duck.look", "转动头部看向某个方向：up 抬头，down 低头，left 看左边，right 看右边，front 回正。",
     "{\"type\":\"object\",\"properties\":{\"direction\":{\"type\":\"string\",\"enum\":[\"up\",\"down\",\"left\",\"right\",\"front\"]}},"
     "\"required\":[\"direction\"]}", tool_look},
    {"self.camera.take_photo",
     "你有一个摄像头，长在机器鸭头上。用户让你看看什么、问你看到了什么、前面有什么时，用它拍一张照片并解读。"
     "question 写你想从照片里知道的问题。返回照片内容的描述。",
     "{\"type\":\"object\",\"properties\":{\"question\":{\"type\":\"string\"}},\"required\":[\"question\"]}",
     tool_photo},
    {"self.duck.mouth", "张开或闭上嘴巴。",
     "{\"type\":\"object\",\"properties\":{\"open\":{\"type\":\"boolean\"}},\"required\":[\"open\"]}", tool_mouth},
    {"self.duck.quack", "让机器鸭叫一声，嘴巴会跟着张合。可以配合对话的情绪：normal 普通，double 嘎嘎，happy 开心，"
                        "question 疑问，angry 生气，scared 害怕，sad 难过，sleepy 犯困。",
     "{\"type\":\"object\",\"properties\":{\"mood\":{\"type\":\"string\",\"enum\":[\"normal\",\"double\",\"happy\","
     "\"question\",\"angry\",\"scared\",\"sad\",\"sleepy\"]}}}", tool_quack},
};
#define NTOOLS (int)(sizeof TOOLS / sizeof TOOLS[0])

/* The do_action entry: the verified skills in the policy library, described by their manifests. */
static void describe_skills(cJSON *t)
{
    char desc[1024];
    int n = snprintf(desc, sizeof desc, "让机器鸭做一个动作，做完自动回到站立行走；坐下类的动作再调用一次就站起来。可选：");
    cJSON *names = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(t, "inputSchema"),
                                                                          "properties"), "name"), "enum");
    const char *err = NULL;
    cJSON *list = api_call(SRC_LLM, "policy.list", NULL, &err);
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(list, "policies")) {
        const char *name = arg_str(e, "name"), *kind = arg_str(e, "kind");
        if ((strcmp(kind, "episodic") && strcmp(kind, "scripted")) || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "verified")))
            continue;
        const cJSON *label = cJSON_GetObjectItemCaseSensitive(e, "label");
        const char *what = arg_str(e, "description")[0] ? arg_str(e, "description")
                         : cJSON_IsString(label) ? label->valuestring : arg_str(label, "zh")[0] ? arg_str(label, "zh") : name;
        cJSON_AddItemToArray(names, cJSON_CreateString(name));
        if (n < (int)sizeof desc) n += snprintf(desc + n, sizeof desc - n, " %s（%s）", name, what);
    }
    cJSON_Delete(list);
    cJSON_ReplaceItemInObject(t, "description", cJSON_CreateString(desc));
}

static cJSON *response(const cJSON *id)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    cJSON_AddItemToObject(r, "id", cJSON_Duplicate(id, 1));
    return r;
}

static cJSON *error(const cJSON *id, int code, const char *message)
{
    cJSON *r = response(id);
    cJSON *e = cJSON_AddObjectToObject(r, "error");
    cJSON_AddNumberToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message);
    return r;
}

static cJSON *call_tool(const cJSON *id, const cJSON *params)
{
    const char *name = arg_str(params, "name");
    for (int i = 0; i < NTOOLS; i++) {
        if (strcmp(name, TOOLS[i].name)) continue;
        char text[2048] = ""; /* a photo's description can be long */
        const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
        const char *err = TOOLS[i].fn(args, text, sizeof text);
        char *a = cJSON_PrintUnformatted(args);
        printf("mcp: %s %s -> %s\n", name, a ? a : "", err ? api_message(err) : text);
        cJSON_free(a);
        cJSON *r = response(id);
        cJSON *result = cJSON_AddObjectToObject(r, "result");
        cJSON *content = cJSON_AddArrayToObject(result, "content");
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "type", "text");
        cJSON_AddStringToObject(item, "text", err ? api_message(err) : text);
        cJSON_AddItemToArray(content, item);
        cJSON_AddBoolToObject(result, "isError", err != NULL);
        return r;
    }
    char msg[96];
    snprintf(msg, sizeof msg, "Unknown tool: %s", name);
    return error(id, -32601, msg);
}

cJSON *mcp_handle(const cJSON *req)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(req, "method");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
    if (!cJSON_IsString(method) || !id) return NULL; /* a notification, or a reply to us */
    if (!strcmp(method->valuestring, "initialize")) {
        const cJSON *vision = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(params, "capabilities"), "vision");
        snprintf(vision_url, sizeof vision_url, "%s", arg_str(vision, "url"));
        snprintf(vision_token, sizeof vision_token, "%s", arg_str(vision, "token"));
        printf("mcp: vision %s\n", vision_url[0] ? vision_url : "(none)");
        cJSON *r = response(id);
        cJSON *result = cJSON_AddObjectToObject(r, "result");
        cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");
        cJSON_AddObjectToObject(cJSON_AddObjectToObject(result, "capabilities"), "tools");
        cJSON *info = cJSON_AddObjectToObject(result, "serverInfo");
        cJSON_AddStringToObject(info, "name", "doitduck");
        cJSON_AddStringToObject(info, "version", DOITDUCK_VERSION);
        return r;
    }
    if (!strcmp(method->valuestring, "tools/list")) {
        cJSON *r = response(id);
        cJSON *result = cJSON_AddObjectToObject(r, "result");
        cJSON *tools = cJSON_AddArrayToObject(result, "tools");
        for (int i = 0; i < NTOOLS; i++) {
            cJSON *t = cJSON_CreateObject();
            cJSON_AddStringToObject(t, "name", TOOLS[i].name);
            cJSON_AddStringToObject(t, "description", TOOLS[i].description);
            cJSON_AddItemToObject(t, "inputSchema", cJSON_Parse(TOOLS[i].schema));
            if (TOOLS[i].fn == tool_action) describe_skills(t);
            cJSON_AddItemToArray(tools, t);
        }
        cJSON_AddStringToObject(result, "nextCursor", "");
        return r;
    }
    if (!strcmp(method->valuestring, "tools/call")) return call_tool(id, params);
    return error(id, -32601, "Method not found");
}
