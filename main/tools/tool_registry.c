#include "tool_registry.h"
#include "mimi_config.h"
#include "tools/tool_web_search.h"
#include "tools/tool_get_time.h"
#include "tools/tool_files.h"
#include "tools/tool_cron.h"
#include "tools/tool_gpio.h"

#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tools";

#define MAX_TOOLS 16

static mimi_tool_t s_tools[MAX_TOOLS];
static int s_tool_count = 0;
static char *s_tools_json = NULL;  /* cached JSON array string */

 
// ====================== WS2812 驱动配置 ======================
// ====================== WS2812 驱动配置（修复版） ======================
#include "led_strip.h"
#include "esp_timer.h"  // 引入定时器组件

#define WS2812_GPIO 48        // 板载 WS2812 连接的 GPIO 引脚
#define BREATH_INTERVAL_MS 10 // 呼吸刷新间隔（越小越丝滑，建议5-10ms）

static led_strip_handle_t s_led_strip = NULL;  // LED 驱动句柄
static bool s_breath_enabled = false;         // 呼吸灯总开关
static uint8_t s_r = 0, s_g = 0, s_b = 0;     // 当前目标颜色
static uint8_t s_brightness = 0;              // 当前亮度（0-255）
static int8_t s_breath_dir = 1;               // 呼吸方向：1=渐亮，-1=渐暗
static esp_timer_handle_t s_breath_timer = NULL; // 呼吸定时器句柄

// 定时器回调函数：非阻塞式呼吸循环（核心修复）
static void breath_timer_callback(void *arg)
{
    if (!s_breath_enabled || !s_led_strip) return;

    // 更新亮度
    s_brightness += s_breath_dir;
    if (s_brightness >= 255) {
        s_brightness = 255;
        s_breath_dir = -1; // 到最大亮度，开始渐暗
    } else if (s_brightness <= 0) {
        s_brightness = 0;
        s_breath_dir = 1;  // 到最小亮度，开始渐亮
    }

    // 计算当前RGB值（按亮度缩放）
    uint8_t nr = (s_r * s_brightness) / 255;
    uint8_t ng = (s_g * s_brightness) / 255;
    uint8_t nb = (s_b * s_brightness) / 255;

    // 更新LED
    led_strip_set_pixel(s_led_strip, 0, nr, ng, nb);
    led_strip_refresh(s_led_strip);
}

// 初始化WS2812+呼吸定时器（只执行一次）
static esp_err_t ws2812_init(void)
{
    if (s_led_strip == NULL) {
        // 1. 初始化WS2812驱动
        led_strip_config_t strip_cfg = {
            .strip_gpio_num = WS2812_GPIO,
            .max_leds = 1,
            .led_model = LED_MODEL_WS2812,
            .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        };
        led_strip_rmt_config_t rmt_cfg = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 10 * 1000 * 1000,
            .flags.with_dma = false,
        };
        ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_led_strip));
        led_strip_clear(s_led_strip);

        // 2. 初始化呼吸定时器（只创建一次）
        esp_timer_create_args_t timer_args = {
            .callback = breath_timer_callback,
            .name = "breath_timer",
            .dispatch_method = ESP_TIMER_TASK, // 在任务中执行，避免中断阻塞
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_breath_timer));
        // 启动定时器（一直运行，由s_breath_enabled控制是否生效）
        ESP_ERROR_CHECK(esp_timer_start_periodic(s_breath_timer, BREATH_INTERVAL_MS * 1000));
    }
    return ESP_OK;
}

// 修复版ws2812_set：只设置颜色，不做阻塞循环
static esp_err_t ws2812_set(uint8_t r, uint8_t g, uint8_t b)
{
    ws2812_init(); // 确保初始化完成

    // 更新目标颜色
    s_r = r;
    s_g = g;
    s_b = b;

    if (!s_breath_enabled) {
        // 非呼吸模式：直接常亮
        led_strip_set_pixel(s_led_strip, 0, r, g, b);
        led_strip_refresh(s_led_strip);
    }
    // 呼吸模式：由定时器自动更新，无需手动操作
    return ESP_OK;
}

// 修复版呼吸灯开关工具
static esp_err_t tool_car_light_breath_execute(const char *in, char *out, size_t len)
{
    s_breath_enabled = true;
    // 开启呼吸后，重置亮度和方向，让呼吸从0开始
    s_brightness = 0;
    s_breath_dir = 1;
    snprintf(out, len, "[车灯] 呼吸灯已开启，当前颜色 R:%d G:%d B:%d", s_r, s_g, s_b);
    return ESP_OK;
}

// 修复版关灯工具（同步关闭呼吸）
static esp_err_t tool_off_execute(const char *in, char *out, size_t len)
{
    s_breath_enabled = false; // 关闭呼吸
    ws2812_set(0, 0, 0);     // 熄灭车灯
    snprintf(out, len, "[车灯] 已关闭");
    return ESP_OK;
}

// 颜色设置工具保持不变，完全兼容原有逻辑
static esp_err_t tool_car_light_color_execute(const char *in, char *out, size_t len) {
    int r = 0, g = 0, b = 0;
    cJSON *root = cJSON_Parse(in);
    if (root != NULL) {
        cJSON *node_r = cJSON_GetObjectItem(root, "r");
        cJSON *node_g = cJSON_GetObjectItem(root, "g");
        cJSON *node_b = cJSON_GetObjectItem(root, "b");
        if (cJSON_IsNumber(node_r)) r = node_r->valueint;
        if (cJSON_IsNumber(node_g)) g = node_g->valueint;
        if (cJSON_IsNumber(node_b)) b = node_b->valueint;
        cJSON_Delete(root);
    }
    r = (r < 0) ? 0 : (r > 255) ? 255 : r;
    g = (g < 0) ? 0 : (g > 255) ? 255 : g;
    b = (b < 0) ? 0 : (b > 255) ? 255 : b;

    ws2812_set(r, g, b);
    snprintf(out, len, "车灯已打开 → R:%d G:%d B:%d", r, g, b);
    return ESP_OK;
}
 

static void register_tool(const mimi_tool_t *tool)
{
    if (s_tool_count >= MAX_TOOLS) {
        ESP_LOGE(TAG, "Tool registry full");
        return;
    }
    s_tools[s_tool_count++] = *tool;
    ESP_LOGI(TAG, "Registered tool: %s", tool->name);
}

static void build_tools_json(void)
{
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < s_tool_count; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", s_tools[i].name);
        cJSON_AddStringToObject(tool, "description", s_tools[i].description);

        cJSON *schema = cJSON_Parse(s_tools[i].input_schema_json);
        if (schema) {
            cJSON_AddItemToObject(tool, "input_schema", schema);
        }

        cJSON_AddItemToArray(arr, tool);
    }

    free(s_tools_json);
    s_tools_json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    ESP_LOGI(TAG, "Tools JSON built (%d tools)", s_tool_count);
}

esp_err_t tool_registry_init(void)
{
    s_tool_count = 0;

    /* Register web_search */
    tool_web_search_init();

    mimi_tool_t ws = {
        .name = "web_search",
        .description = "Search the web for current information via Tavily (preferred) or Brave when configured.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"The search query\"}},"
            "\"required\":[\"query\"]}",
        .execute = tool_web_search_execute,
    };
    register_tool(&ws);

    /* Register get_current_time */
    mimi_tool_t gt = {
        .name = "get_current_time",
        .description = "Get the current date and time. Also sets the system clock. Call this when you need to know what time or date it is.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_get_time_execute,
    };
    register_tool(&gt);

    /* Register read_file */
    mimi_tool_t rf = {
        .name = "read_file",
        .description = "Read a file from SPIFFS storage. Path must start with " MIMI_SPIFFS_BASE "/.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " MIMI_SPIFFS_BASE "/\"}},"
            "\"required\":[\"path\"]}",
        .execute = tool_read_file_execute,
    };
    register_tool(&rf);

    /* Register write_file */
    mimi_tool_t wf = {
        .name = "write_file",
        .description = "Write or overwrite a file on SPIFFS storage. Path must start with " MIMI_SPIFFS_BASE "/.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " MIMI_SPIFFS_BASE "/\"},"
            "\"content\":{\"type\":\"string\",\"description\":\"File content to write\"}},"
            "\"required\":[\"path\",\"content\"]}",
        .execute = tool_write_file_execute,
    };
    register_tool(&wf);

    /* Register edit_file */
    mimi_tool_t ef = {
        .name = "edit_file",
        .description = "Find and replace text in a file on SPIFFS. Replaces first occurrence of old_string with new_string.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " MIMI_SPIFFS_BASE "/\"},"
            "\"old_string\":{\"type\":\"string\",\"description\":\"Text to find\"},"
            "\"new_string\":{\"type\":\"string\",\"description\":\"Replacement text\"}},"
            "\"required\":[\"path\",\"old_string\",\"new_string\"]}",
        .execute = tool_edit_file_execute,
    };
    register_tool(&ef);

    /* Register list_dir */
    mimi_tool_t ld = {
        .name = "list_dir",
        .description = "List files on SPIFFS storage, optionally filtered by path prefix.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"prefix\":{\"type\":\"string\",\"description\":\"Optional path prefix filter, e.g. " MIMI_SPIFFS_BASE "/memory/\"}},"
            "\"required\":[]}",
        .execute = tool_list_dir_execute,
    };
    register_tool(&ld);

    /* Register cron_add */
    mimi_tool_t ca = {
        .name = "cron_add",
        .description = "Schedule a recurring or one-shot task. The message will trigger an agent turn when the job fires.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Short name for the job\"},"
            "\"schedule_type\":{\"type\":\"string\",\"description\":\"'every' for recurring interval or 'at' for one-shot at a unix timestamp\"},"
            "\"interval_s\":{\"type\":\"integer\",\"description\":\"Interval in seconds (required for 'every')\"},"
            "\"at_epoch\":{\"type\":\"integer\",\"description\":\"Unix timestamp to fire at (required for 'at')\"},"
            "\"message\":{\"type\":\"string\",\"description\":\"Message to inject when the job fires, triggering an agent turn\"},"
            "\"channel\":{\"type\":\"string\",\"description\":\"Optional reply channel (e.g. 'telegram'). If omitted, current turn channel is used when available\"},"
            "\"chat_id\":{\"type\":\"string\",\"description\":\"Optional reply chat_id. Required when channel='telegram'. If omitted during a Telegram turn, current chat_id is used\"}"
            "},"
            "\"required\":[\"name\",\"schedule_type\",\"message\"]}",
        .execute = tool_cron_add_execute,
    };
    register_tool(&ca);

    /* Register cron_list */
    mimi_tool_t cl = {
        .name = "cron_list",
        .description = "List all scheduled cron jobs with their status, schedule, and IDs.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_cron_list_execute,
    };
    register_tool(&cl);

    /* Register cron_remove */
    mimi_tool_t cr = {
        .name = "cron_remove",
        .description = "Remove a scheduled cron job by its ID.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"job_id\":{\"type\":\"string\",\"description\":\"The 8-character job ID to remove\"}},"
            "\"required\":[\"job_id\"]}",
        .execute = tool_cron_remove_execute,
    };
    register_tool(&cr);

    /* Register GPIO tools */
    tool_gpio_init();

    mimi_tool_t gw = {
        .name = "gpio_write",
        .description = "Set a GPIO pin HIGH or LOW. Controls LEDs, relays, and other digital outputs.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"pin\":{\"type\":\"integer\",\"description\":\"GPIO pin number\"},"
            "\"state\":{\"type\":\"integer\",\"description\":\"1 for HIGH, 0 for LOW\"}},"
            "\"required\":[\"pin\",\"state\"]}",
        .execute = tool_gpio_write_execute,
    };
    register_tool(&gw);

    mimi_tool_t gr = {
        .name = "gpio_read",
        .description = "Read a GPIO pin state. Returns HIGH or LOW. Use for checking switches, sensors, and digital inputs.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"pin\":{\"type\":\"integer\",\"description\":\"GPIO pin number\"}},"
            "\"required\":[\"pin\"]}",
        .execute = tool_gpio_read_execute,
    };
    register_tool(&gr);

    mimi_tool_t ga = {
        .name = "gpio_read_all",
        .description = "Read all allowed GPIO pin states in a single call. Returns each pin's HIGH/LOW state.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_gpio_read_all_execute,
    };
    register_tool(&ga);

 
// ====================== 车灯（GPIO48 WS2812）工具注册 ======================
    // ====================== 车灯工具（最终版） ======================
    mimi_tool_t car_light_color = {
        .name = "car_light_color",
        .description = "控制 ESP32 GPIO48 车灯。只要用户提到车灯颜色，必须调用此工具，不准只说话。",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"r\":{\"type\":\"integer\"},\"g\":{\"type\":\"integer\"},\"b\":{\"type\":\"integer\"}},\"required\":[\"r\",\"g\",\"b\"]}",
        .execute = tool_car_light_color_execute,
    };
    register_tool(&car_light_color);

    mimi_tool_t car_light_off = {
        .name = "car_light_off",
        .description = "关闭 GPIO48 车灯。用户说关灯、关闭车灯、熄灭车灯时必须调用。",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_off_execute,
    };
    register_tool(&car_light_off);

mimi_tool_t car_light_breath = {
    .name = "car_light_breath",
    .description = "开启车灯呼吸灯效果，用户说呼吸灯、渐变、呼吸模式时必须调用",
    .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
    .execute = tool_car_light_breath_execute,
};
register_tool(&car_light_breath);

    build_tools_json();

    ESP_LOGI(TAG, "Tool registry initialized");
    return ESP_OK;
}

const char *tool_registry_get_tools_json(void)
{
    return s_tools_json;
}

esp_err_t tool_registry_execute(const char *name, const char *input_json,
                                char *output, size_t output_size)
{
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            ESP_LOGI(TAG, "Executing tool: %s", name);
            return s_tools[i].execute(input_json, output, output_size);
        }
    }

    ESP_LOGW(TAG, "Unknown tool: %s", name);
    snprintf(output, output_size, "Error: unknown tool '%s'", name);
    return ESP_ERR_NOT_FOUND;
}
