/**
 * ============================================================================
 * Proyecto : Estacion de Control HMI y Telemetria IoT
 * Archivo  : main.cpp (ESP32-S3 Firmware)
 * Descrip. : Gateway IoT, Servidor Web Asincrono y Panel Tactil LVGL 8.3
 * Author   : DanyGhostt
 * ============================================================================
 */

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

#include "triceratops_gif.h"
#include "web_ui.h"

// FreeRTOS Kernel
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// ============================================================================
// 1. DEFINICION DE PINES DE HARDWARE / HARDWARE PIN DEFINITIONS
// ============================================================================
// Enlace Serie UART hacia STM32 / UART Link to STM32
#define RXD1 18 // STM32 TX (PB10) -> ESP32 RX
#define TXD1 17 // STM32 RX (PC5)  -> ESP32 TX

// Bus SPI Principal Compartido / Shared Hardware SPI Bus
#define SPI_SCLK_PIN    6
#define SPI_MOSI_PIN    7
#define SPI_MISO_PIN    1   // T_DO del panel táctil / Touch MISO

// Pantalla TFT ILI9341 (320x240) / ILI9341 TFT Display
#define TFT_CS_PIN     10
#define TFT_DC_PIN      9
#define TFT_RST_PIN    14

// Panel Táctil Resistivo XPT2046 / XPT2046 Resistive Touch Controller
#define TOUCH_CS_PIN    2   // T_CS
#define TOUCH_IRQ_PIN   3   // T_IRQ

// Calibración del panel táctil (lectura directa) / Touchscreen ADC Calibration
#define TS_RAW_MIN_X   370
#define TS_RAW_MAX_X  3720
#define TS_RAW_MIN_Y   410
#define TS_RAW_MAX_Y  3730
#define TOUCH_PRESSURE_THRESHOLD 200

// ============================================================================
// 2. PROTOCOLO CON LA STM32 (6 BYTES) / STM32 UART BINARY PROTOCOL
// ============================================================================
#define CMD_MOTOR_DC       0x01
#define CMD_ULTRASONIC     0x02
#define CMD_SERVO          0x03
#define CMD_TEMP           0x04
#define CMD_EMERGENCY      0xEE
#define CMD_WEB_MOTOR_SPEED 0x02

enum ControlKind : uint8_t {
    CONTROL_MOTOR_ACTION,
    CONTROL_MOTOR_SPEED,
    CONTROL_SERVO
};

// Trama binaria estructurada de 6 bytes / 6-Byte Packed Binary Interaction Frame
typedef struct __attribute__((packed)) {
    uint8_t startMarker;   // Inicio de trama / Start delimiter: 0xAA
    uint8_t commandCode;   // Identificador de accion / Command ID
    uint8_t payloadLength; // Longitud del dato / Payload length: 0x01
    uint8_t actionData;    // Parametro o telemetria / Parameter or telemetry data
    uint8_t checksum;      // Suma de validacion / Checksum: (CMD + LEN + DATA) & 0xFF
    uint8_t endMarker;     // Fin de trama / End delimiter: 0x55
} RemoteInteractionFrame_t;

typedef struct {
    uint8_t cmd;
    uint8_t data;
    uint8_t kind;
} QueuedCommand_t;

// ============================================================================
// 3. OBJETOS DEL SISTEMA Y RED / SYSTEM & NETWORK OBJECTS
// ================================================================================
HardwareSerial STM32_Serial(1);

Adafruit_ILI9341 tft = Adafruit_ILI9341(&SPI, TFT_DC_PIN, TFT_CS_PIN, TFT_RST_PIN);
XPT2046_Touchscreen touch(TOUCH_CS_PIN);

QueueHandle_t xCommandDispatchQueue;
SemaphoreHandle_t xGuiMutex;

volatile uint8_t g_telemetry_distance = 0;
volatile int8_t  g_telemetry_temp = 24;
volatile bool    g_temperature_available = false;
volatile bool    g_distance_available = false;
volatile bool    g_temperature_received = false;
volatile bool    g_stm_connected = false;
volatile uint8_t g_motor_speed = 50;
volatile uint8_t g_motor_action = 0xFF;
volatile uint8_t g_servo_angle = 90;
volatile bool    g_syncing_controls = false;
uint32_t g_last_touch_log_ms = 0;

// Servidor Web y WebSocket
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

const char *ssid_wifi = "Totalplay-2.4G-a728";
const char *pass_wifi = "FamP3rezSus4n4001.";

// ==========================================
// 4. BUFFERS Y OBJETOS LVGL
// ==========================================
#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240
#define DISPLAY_ROTATION 1
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf_1[SCREEN_WIDTH * 10]; // Buffer en SRAM interna

// Pantallas
static lv_obj_t *scr_splash;
static lv_obj_t *scr_dashboard;
static lv_obj_t *scr_qr;
static lv_obj_t *qr_wifi;
static lv_obj_t *lbl_wifi_status;
static lv_obj_t *lbl_wifi_url;

// Widgets dinámicos de telemetría y control
static lv_obj_t *led_stm_status;
static lv_obj_t *bar_ultrasonic;
static lv_obj_t *lbl_temp_val;
static lv_obj_t *lbl_distance_val;
static lv_obj_t *arc_servo;
static lv_obj_t *slider_motor;
static lv_obj_t *btn_motor_on;
static lv_obj_t *btn_motor_off;
static lv_obj_t *btn_motor_stop;
static lv_obj_t *lbl_servo_value;
static lv_obj_t *splash_title_mask;
static lv_obj_t *splash_triceratops;
static lv_coord_t splash_title_x;
static lv_coord_t splash_title_y;
static lv_coord_t splash_title_width;
static lv_coord_t splash_title_height;

#define SPLASH_TRICERATOPS_WIDTH 215
#define SPLASH_TRICERATOPS_HEIGHT 110

static const lv_img_dsc_t splash_triceratops_source = {
    .header = {
        .cf = LV_IMG_CF_RAW,
        .always_zero = 0,
        .reserved = 0,
        .w = 215,
        .h = 110,
    },
    .data_size = TRICERATOPS_GIF_DATA_SIZE,
    .data = triceratops_gif_data,
};

// Declaración previa
void Send_Command_To_STM32(uint8_t cmd, uint8_t data);
void initWebServer();
void broadcastControlState();
void sendCurrentState(AsyncWebSocketClient *client);
void applyLocalControlState(uint8_t kind);

// ==========================================
// 5. MANEJADOR WEBSOCKET Y SERVIDOR ASÍNCRONO
// ==========================================
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, 
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        Serial.printf("[WS] Cliente conectado #%u desde %s\n", client->id(), client->remoteIP().toString().c_str());
        sendCurrentState(client);
    } else if (type == WS_EVT_DISCONNECT) {
        Serial.printf("[WS] Cliente desconectado #%u\n", client->id());
    } else if (type == WS_EVT_DATA) {
        AwsFrameInfo *info = (AwsFrameInfo*)arg;
        if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
            data[len] = 0;
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, (char*)data);
            if (!error) {
                uint8_t cmd = doc["cmd"] | 0;
                uint8_t val = doc["data"] | 0;
                QueuedCommand_t q_cmd;
                if (cmd == CMD_WEB_MOTOR_SPEED) {
                    q_cmd = {CMD_MOTOR_DC, val, CONTROL_MOTOR_SPEED};
                } else if (cmd == CMD_MOTOR_DC) {
                    q_cmd = {CMD_MOTOR_DC, val, CONTROL_MOTOR_ACTION};
                } else if (cmd == CMD_SERVO) {
                    q_cmd = {CMD_SERVO, val, CONTROL_SERVO};
                } else {
                    return;
                }

                BaseType_t queued = xQueueSend(xCommandDispatchQueue, &q_cmd, 0);
                Serial.printf("[WS QUEUE] Cmd: 0x%02X | Val: %u | Queued: %s\n",
                              q_cmd.cmd, q_cmd.data, queued == pdTRUE ? "OK" : "FULL");
            }
        }
    }
}

void updateWiFiScreen(bool connected) {
    if (xSemaphoreTake(xGuiMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    lv_label_set_text(lbl_wifi_status, connected ? "WiFi Connected" : "WiFi Disconnected");
    lv_obj_set_style_text_color(lbl_wifi_status,
                                lv_color_hex(connected ? 0x00FF00 : 0xFF5555), 0);

    if (connected) {
        String url = "http://" + WiFi.localIP().toString();
        if (qr_wifi == NULL) {
            qr_wifi = lv_qrcode_create(scr_qr, 90, lv_color_black(), lv_color_white());
            lv_obj_align(qr_wifi, LV_ALIGN_TOP_MID, 0, 48);
        }
        lv_qrcode_update(qr_wifi, url.c_str(), url.length());
        lv_label_set_text_fmt(lbl_wifi_url, "URL: %s", url.c_str());
    } else {
        if (qr_wifi != NULL) {
            lv_obj_del(qr_wifi);
            qr_wifi = NULL;
        }
        lv_label_set_text(lbl_wifi_url, "IP unavailable");
    }

    xSemaphoreGive(xGuiMutex);
}

void vWiFiTask(void *pvParameters) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    Serial.printf("[WIFI] Conectando a: %s\n", ssid_wifi);
    WiFi.begin(ssid_wifi, pass_wifi);

    bool wasConnected = false;
    bool webServerStarted = false;
    while (true) {
        bool connected = WiFi.status() == WL_CONNECTED;
        if (connected != wasConnected) {
            if (connected) {
                Serial.printf("[WIFI] Conectado a %s\n", ssid_wifi);
                Serial.printf("[WIFI] Acceso Web: http://%s\n", WiFi.localIP().toString().c_str());
                if (!webServerStarted) {
                    initWebServer();
                    webServerStarted = true;
                }
            } else {
                Serial.println("[WIFI] WiFi Disconnected");
            }
            updateWiFiScreen(connected);
            wasConnected = connected;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void initWebServer() {
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send_P(200, "text/html", INDEX_HTML);
    });

    server.begin();
    Serial.println("[HTTP] Servidor listo.");
}

void broadcastTelemetry(float temp, float dist) {
    if (ws.count() > 0) {
        JsonDocument doc;
        if (g_temperature_received) {
            doc["temp"] = temp;
        }
        if (g_distance_available) {
            doc["dist"] = dist;
        }
        String output;
        serializeJson(doc, output);
        ws.textAll(output);
    }
}

// ==========================================
// CONTROLADORES DE PANTALLA Y ENTRADA LVGL
// ==========================================
void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    digitalWrite(TOUCH_CS_PIN, HIGH);
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.writePixels((uint16_t *)&color_p->full, w * h);
    tft.endWrite();

    lv_disp_flush_ready(disp_drv);
}

void my_touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data) {
    digitalWrite(TFT_CS_PIN, HIGH);

    if (touch.touched()) {
        TS_Point p = touch.getPoint();

        if (p.z > 200 && p.z < 3900 && p.x > 100 && p.y > 100) {
            int16_t touchX = map(p.x, TS_RAW_MIN_X, TS_RAW_MAX_X, 0, SCREEN_WIDTH);
            int16_t touchY = map(p.y, TS_RAW_MIN_Y, TS_RAW_MAX_Y, 0, SCREEN_HEIGHT);

            data->state = LV_INDEV_STATE_PR;
            data->point.x = constrain(touchX, 0, SCREEN_WIDTH - 1);
            data->point.y = constrain(touchY, 0, SCREEN_HEIGHT - 1);
            if (millis() - g_last_touch_log_ms >= 250) {
                Serial.printf("[TOUCH] raw=(%d,%d) mapped=(%d,%d) z=%d\n",
                              p.x, p.y, data->point.x, data->point.y, p.z);
                g_last_touch_log_ms = millis();
            }
            return;
        }
    }
    data->state = LV_INDEV_STATE_REL;
}

// ==========================================
// EVENTOS DE LA INTERFAZ FÍSICA LVGL
// ==========================================
static void btn_motor_event_cb(lv_event_t *e) {
    if (g_syncing_controls) return;
    static uint32_t last_button_ms = 0;
    uint32_t now_ms = millis();
    if (now_ms - last_button_ms < 250) return;
    last_button_ms = now_ms;

    uint8_t action = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    QueuedCommand_t cmd = {CMD_MOTOR_DC, action, CONTROL_MOTOR_ACTION};
    BaseType_t queued = xQueueSend(xCommandDispatchQueue, &cmd, 0);
    Serial.printf("[TOUCH GUI] Motor action=%u queued=%s\n", action, queued == pdTRUE ? "yes" : "no");
}

static void slider_speed_event_cb(lv_event_t *e) {
    if (g_syncing_controls || g_motor_action != 1) return;
    lv_obj_t *slider = lv_event_get_target(e);
    uint8_t speed = (uint8_t)lv_slider_get_value(slider);
    QueuedCommand_t cmd = {CMD_MOTOR_DC, speed, CONTROL_MOTOR_SPEED};
    BaseType_t queued = xQueueSend(xCommandDispatchQueue, &cmd, 0);
    Serial.printf("[TOUCH GUI] Motor speed=%u queued=%s\n", speed, queued == pdTRUE ? "yes" : "no");
}

static void arc_servo_event_cb(lv_event_t *e) {
    if (g_syncing_controls) return;
    lv_obj_t *arc = lv_event_get_target(e);
    int16_t val = lv_arc_get_value(arc);
    uint8_t angle = (uint8_t)map(val, 0, 100, 0, 180);
    QueuedCommand_t cmd = {CMD_SERVO, angle, CONTROL_SERVO};
    BaseType_t queued = xQueueSend(xCommandDispatchQueue, &cmd, 0);
    Serial.printf("[TOUCH GUI] Servo angle=%u queued=%s\n", angle, queued == pdTRUE ? "yes" : "no");
}

static void nav_to_qr_cb(lv_event_t *e) {
    lv_scr_load_anim(scr_qr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}

static void nav_to_dash_cb(lv_event_t *e) {
    lv_scr_load_anim(scr_dashboard, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
}

// ==========================================
// CONSTRUCCIÓN DE PANTALLAS
// ==========================================
static lv_obj_t *create_splash_shape(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                     lv_coord_t width, lv_coord_t height,
                                     uint32_t color, lv_coord_t radius) {
    lv_obj_t *shape = lv_obj_create(parent);
    lv_obj_set_size(shape, width, height);
    lv_obj_set_pos(shape, x, y);
    lv_obj_set_style_bg_color(shape, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(shape, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(shape, 0, 0);
    lv_obj_set_style_radius(shape, radius, 0);
    lv_obj_set_style_pad_all(shape, 0, 0);
    lv_obj_clear_flag(shape, LV_OBJ_FLAG_SCROLLABLE);
    return shape;
}

static void create_splash_memory_icon(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                      lv_coord_t width, lv_coord_t height) {
    const uint32_t color = 0xFFFFFF;
    const lv_coord_t body_width = width * 7 / 10;
    const lv_coord_t body_height = height * 6 / 10;
    const lv_coord_t body_x = x + (width - body_width) / 2;
    const lv_coord_t body_y = y + (height - body_height) / 2;
    const lv_coord_t pin_width = LV_MAX(2, width / 14);
    const lv_coord_t pin_height = LV_MAX(2, height / 15);
    const lv_coord_t border_width = LV_MAX(1, width / 32);

    lv_obj_t *body = lv_obj_create(parent);
    lv_obj_set_size(body, body_width, body_height);
    lv_obj_set_pos(body, body_x, body_y);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(body, border_width, 0);
    lv_obj_set_style_radius(body, LV_MAX(1, width / 14), 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    for (uint8_t pin = 1; pin <= 3; pin++) {
        lv_coord_t pin_x = body_x + pin * body_width / 4 - pin_width / 2;
        create_splash_shape(parent, pin_x, body_y - pin_height, pin_width, pin_height, color, 1);
        create_splash_shape(parent, pin_x, body_y + body_height, pin_width, pin_height, color, 1);
    }

    create_splash_shape(parent, body_x - pin_width, body_y + body_height / 3, pin_width, pin_height, color, 1);
    create_splash_shape(parent, body_x - pin_width, body_y + body_height * 2 / 3, pin_width, pin_height, color, 1);
    create_splash_shape(parent, body_x + body_width, body_y + body_height / 3, pin_width, pin_height, color, 1);
    create_splash_shape(parent, body_x + body_width, body_y + body_height * 2 / 3, pin_width, pin_height, color, 1);

    const lv_coord_t cell_width = LV_MAX(2, (body_width - 4 * border_width) / 3);
    const lv_coord_t cell_height = LV_MAX(2, (body_height - 4 * border_width) / 3);
    for (uint8_t row = 0; row < 2; row++) {
        for (uint8_t column = 0; column < 2; column++) {
            create_splash_shape(parent,
                                body_x + border_width * 2 + column * (cell_width + border_width),
                                body_y + border_width * 2 + row * (cell_height + border_width),
                                cell_width, cell_height, color, 1);
        }
    }
}

static void update_splash_reveal(lv_coord_t revealed_width, lv_coord_t dinosaur_x) {
    if (revealed_width >= splash_title_width) {
        lv_obj_add_flag(splash_title_mask, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(splash_title_mask, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(splash_title_mask, splash_title_x + revealed_width, splash_title_y);
        lv_obj_set_size(splash_title_mask, splash_title_width - revealed_width, splash_title_height);
    }
    lv_obj_set_x(splash_triceratops, dinosaur_x);
}

void Build_Splash_Screen() {
    scr_splash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_splash, lv_color_hex(0x111827), 0);

    lv_obj_t *card = lv_obj_create(scr_splash);
    lv_obj_set_size(card, 320, 240);
    lv_obj_set_pos(card, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x170949), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0xFDFCFC), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_title = lv_label_create(card);
    lv_label_set_text(lbl_title, "TOUCH GATEWAY ESP32 S3");
    lv_obj_set_width(lbl_title, 255);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0xFAFAFA), 0);
    lv_obj_set_style_text_align(lbl_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_title, LV_ALIGN_CENTER, 13, -13);
    lv_obj_update_layout(lbl_title);

    splash_title_x = lv_obj_get_x(lbl_title);
    splash_title_y = lv_obj_get_y(lbl_title);
    splash_title_width = lv_obj_get_width(lbl_title);
    splash_title_height = lv_obj_get_height(lbl_title);

    splash_title_mask = lv_obj_create(card);
    lv_obj_set_pos(splash_title_mask, splash_title_x, splash_title_y);
    lv_obj_set_size(splash_title_mask, splash_title_width, splash_title_height);
    lv_obj_set_style_bg_color(splash_title_mask, lv_color_hex(0x170949), 0);
    lv_obj_set_style_bg_opa(splash_title_mask, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(splash_title_mask, 0, 0);
    lv_obj_set_style_radius(splash_title_mask, 0, 0);
    lv_obj_set_style_pad_all(splash_title_mask, 0, 0);
    lv_obj_clear_flag(splash_title_mask, LV_OBJ_FLAG_SCROLLABLE);

    splash_triceratops = lv_gif_create(card);
    lv_obj_set_pos(splash_triceratops, splash_title_x - SPLASH_TRICERATOPS_WIDTH, 105);
    lv_gif_set_src(splash_triceratops, &splash_triceratops_source);

    create_splash_memory_icon(scr_splash, -53, -45, 125, 100);
    create_splash_memory_icon(scr_splash, 245, 169, 125, 100);
    create_splash_memory_icon(scr_splash, 5, 85, 35, 45);
}

void Build_Dashboard_Screen() {
    scr_dashboard = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_dashboard, lv_color_hex(0x7418AA), 0);
    lv_obj_set_style_bg_grad_color(scr_dashboard, lv_color_hex(0x153684), 0);
    lv_obj_set_style_bg_grad_dir(scr_dashboard, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_color(scr_dashboard, lv_color_hex(0x080808), 0);
    lv_obj_set_style_border_width(scr_dashboard, 2, 0);
    lv_obj_set_style_radius(scr_dashboard, 7, 0);

    // 1. Header & Conectividad STM32
    led_stm_status = lv_led_create(scr_dashboard);
    lv_obj_set_size(led_stm_status, 15, 15);
    lv_obj_set_pos(led_stm_status, 10, 10);
    lv_led_set_color(led_stm_status, lv_color_hex(0x12CE60));
    lv_obj_set_style_bg_color(led_stm_status, lv_color_hex(0xE6D5D5), 0);
    lv_obj_set_style_shadow_width(led_stm_status, 12, 0);
    lv_obj_set_style_shadow_color(led_stm_status, lv_color_hex(0xE6D5D5), 0);
    lv_obj_set_style_shadow_spread(led_stm_status, 4, 0);
    lv_led_off(led_stm_status);

    lv_obj_t *lbl_stm = lv_label_create(scr_dashboard);
    lv_label_set_text(lbl_stm, "STM Conectivity");
    lv_obj_set_style_text_color(lbl_stm, lv_color_hex(0xF7F7F7), 0);
    lv_obj_set_pos(lbl_stm, 40, 10);

    // Botón QR para ir a Pantalla 3
    lv_obj_t *btn_qr = lv_btn_create(scr_dashboard);
    lv_obj_set_size(btn_qr, 40, 25);
    lv_obj_set_pos(btn_qr, 270, 5);
    lv_obj_set_style_bg_color(btn_qr, lv_color_hex(0x57269C), 0);
    lv_obj_add_event_cb(btn_qr, nav_to_qr_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_btn_qr = lv_label_create(btn_qr);
    lv_label_set_text(lbl_btn_qr, "QR");
    lv_obj_set_style_text_color(lbl_btn_qr, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl_btn_qr);

    // Línea divisoria
    lv_obj_t *div = lv_label_create(scr_dashboard);
    lv_label_set_text(div, "----------------------------------------------------");
    lv_obj_set_style_text_color(div, lv_color_hex(0xF7F7F7), 0);
    lv_obj_set_pos(div, 0, 23);

    // 2. Control Servo (Arc interactivo)
    arc_servo = lv_arc_create(scr_dashboard);
    lv_obj_set_size(arc_servo, 70, 60);
    lv_obj_set_pos(arc_servo, 25, 48);
    lv_arc_set_bg_angles(arc_servo, 135, 45);
    lv_arc_set_mode(arc_servo, LV_ARC_MODE_SYMMETRICAL);
    lv_arc_set_value(arc_servo, map(g_servo_angle, 0, 180, 0, 100));
    lv_obj_set_style_bg_color(arc_servo, lv_color_hex(0x000000), LV_PART_KNOB);
    lv_obj_set_style_radius(arc_servo, 9999, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc_servo, 5, LV_PART_KNOB);
    lv_obj_add_event_cb(arc_servo, arc_servo_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *lbl_servo = lv_label_create(scr_dashboard);
    lv_label_set_text(lbl_servo, "Servo motor");
    lv_obj_set_style_text_color(lbl_servo, lv_color_hex(0x000000), 0);
    lv_obj_set_pos(lbl_servo, 10, 110);

    lbl_servo_value = lv_label_create(scr_dashboard);
    lv_label_set_text(lbl_servo_value, "Servo: 90 deg");
    lv_obj_set_style_text_color(lbl_servo_value, lv_color_hex(0x000000), 0);
    lv_obj_set_pos(lbl_servo_value, 10, 128);

    // 3. Control Motor DC (ON, OFF, STOP y Slider)
    btn_motor_on = lv_btn_create(scr_dashboard);
    lv_obj_set_size(btn_motor_on, 40, 20);
    lv_obj_set_pos(btn_motor_on, 160, 55);
    lv_obj_set_style_bg_color(btn_motor_on, lv_color_hex(0x00D146), 0);
    lv_obj_add_event_cb(btn_motor_on, btn_motor_event_cb, LV_EVENT_CLICKED, (void*)0x01);
    lv_obj_t *l_on = lv_label_create(btn_motor_on); lv_label_set_text(l_on, "ON");
    lv_obj_set_style_text_color(l_on, lv_color_hex(0x000000), 0); lv_obj_center(l_on);

    btn_motor_off = lv_btn_create(scr_dashboard);
    lv_obj_set_size(btn_motor_off, 40, 20);
    lv_obj_set_pos(btn_motor_off, 218, 55);
    lv_obj_set_style_bg_color(btn_motor_off, lv_color_hex(0xD10000), 0);
    lv_obj_add_event_cb(btn_motor_off, btn_motor_event_cb, LV_EVENT_CLICKED, (void*)0x00);
    lv_obj_t *l_off = lv_label_create(btn_motor_off); lv_label_set_text(l_off, "OFF");
    lv_obj_set_style_text_color(l_off, lv_color_hex(0x000000), 0); lv_obj_center(l_off);

    btn_motor_stop = lv_btn_create(scr_dashboard);
    lv_obj_set_size(btn_motor_stop, 40, 20);
    lv_obj_set_pos(btn_motor_stop, 270, 55);
    lv_obj_set_style_bg_color(btn_motor_stop, lv_color_hex(0xF08B0F), 0);
    lv_obj_add_event_cb(btn_motor_stop, btn_motor_event_cb, LV_EVENT_CLICKED, (void*)0x05);
    lv_obj_t *l_stop = lv_label_create(btn_motor_stop); lv_label_set_text(l_stop, "STOP");
    lv_obj_set_style_text_color(l_stop, lv_color_hex(0x000000), 0); lv_obj_center(l_stop);

    slider_motor = lv_slider_create(scr_dashboard);
    lv_obj_set_size(slider_motor, 135, 15);
    lv_obj_set_pos(slider_motor, 170, 93);
    lv_slider_set_range(slider_motor, 0, 100);
    lv_slider_set_value(slider_motor, 50, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider_motor, lv_color_hex(0xF5F4F4), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider_motor, 69, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider_motor, lv_color_hex(0x131012), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider_motor, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider_motor, 9999, LV_PART_MAIN | LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_motor, lv_color_hex(0xFAFAFA), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider_motor, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_clear_flag(slider_motor, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(slider_motor, slider_speed_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *lbl_motor = lv_label_create(scr_dashboard);
    lv_label_set_text(lbl_motor, "Motor");
    lv_obj_set_size(lbl_motor, 90, 20);
    lv_obj_set_style_text_align(lbl_motor, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lbl_motor, lv_color_hex(0x000000), 0);
    lv_obj_set_pos(lbl_motor, 193, 115);

    // 4. Indicador de Temperatura
    lv_obj_t *lbl_temp_title = lv_label_create(scr_dashboard);
    lv_label_set_text(lbl_temp_title, "Temperature");
    lv_obj_set_style_text_color(lbl_temp_title, lv_color_hex(0x000000), 0);
    lv_obj_set_pos(lbl_temp_title, 120, 160);

    lbl_temp_val = lv_label_create(scr_dashboard);
    lv_label_set_text(lbl_temp_val, "-- °C");
    lv_obj_set_size(lbl_temp_val, 105, 35);
    lv_obj_set_style_text_align(lbl_temp_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lbl_temp_val, lv_color_hex(0x000000), 0);
    lv_obj_set_pos(lbl_temp_val, 113, 135);

    // 5. Sensor Ultrasónico
    lv_obj_t *lbl_ultra = lv_label_create(scr_dashboard);
    lv_label_set_text(lbl_ultra, "Ultrasonic Sensor");
    lv_obj_set_style_text_color(lbl_ultra, lv_color_hex(0x000000), 0);
    lv_obj_set_pos(lbl_ultra, 15, 185);

    lbl_distance_val = lv_label_create(scr_dashboard);
    lv_label_set_text(lbl_distance_val, "-- cm");
    lv_obj_set_style_text_color(lbl_distance_val, lv_color_hex(0x000000), 0);
    lv_obj_set_pos(lbl_distance_val, 235, 185);

    bar_ultrasonic = lv_bar_create(scr_dashboard);
    lv_obj_set_size(bar_ultrasonic, 285, 20);
    lv_obj_set_pos(bar_ultrasonic, 15, 205);
    lv_bar_set_range(bar_ultrasonic, 0, 100);
    lv_bar_set_value(bar_ultrasonic, 50, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_ultrasonic, lv_color_hex(0xD006DB), 0);
    lv_obj_set_style_bg_opa(bar_ultrasonic, 51, 0);
    lv_obj_set_style_bg_color(bar_ultrasonic, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar_ultrasonic, LV_OPA_COVER, LV_PART_INDICATOR);
}

void Build_QR_Screen() {
    scr_qr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_qr, lv_color_hex(0x153684), 0);

    lv_obj_t *lbl_qr_title = lv_label_create(scr_qr);
    lv_label_set_text(lbl_qr_title, "Scan for Remote app!");
    lv_obj_set_style_text_color(lbl_qr_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(lbl_qr_title, LV_ALIGN_TOP_MID, 0, 15);

    lbl_wifi_status = lv_label_create(scr_qr);
    lv_label_set_text(lbl_wifi_status, "WiFi Disconnected");
    lv_obj_set_style_text_color(lbl_wifi_status, lv_color_hex(0xFF5555), 0);
    lv_obj_align(lbl_wifi_status, LV_ALIGN_TOP_MID, 0, 143);

    lbl_wifi_url = lv_label_create(scr_qr);
    lv_label_set_text(lbl_wifi_url, "IP unavailable");
    lv_obj_set_style_text_color(lbl_wifi_url, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(lbl_wifi_url, LV_ALIGN_TOP_MID, 0, 163);

    // Botón para volver al Dashboard
    lv_obj_t *btn_back = lv_btn_create(scr_qr);
    lv_obj_set_size(btn_back, 90, 30);
    lv_obj_align(btn_back, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x57269C), 0);
    lv_obj_add_event_cb(btn_back, nav_to_dash_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_center(lbl_back);
}

// ==========================================
// COMUNICACIÓN SERIAL CON LA STM32
// ==========================================
void Send_Command_To_STM32(uint8_t cmd, uint8_t data) {
    RemoteInteractionFrame_t frame;
    frame.startMarker   = 0xAA;
    frame.commandCode   = cmd;
    frame.payloadLength = 0x01;
    frame.actionData    = data;
    frame.checksum      = (uint8_t)(cmd + 0x01 + data);
    frame.endMarker     = 0x55;

    STM32_Serial.write((uint8_t*)&frame, sizeof(RemoteInteractionFrame_t));
    STM32_Serial.flush();
}

void Process_Incoming_STM32_Data() {
    while (STM32_Serial.available() >= (int)sizeof(RemoteInteractionFrame_t)) {
        if (STM32_Serial.peek() != 0xAA) {
            STM32_Serial.read();
            continue;
        }

        RemoteInteractionFrame_t rx_frame;
        STM32_Serial.readBytes((uint8_t*)&rx_frame, sizeof(RemoteInteractionFrame_t));
        uint8_t expected_cs = (uint8_t)(rx_frame.commandCode + rx_frame.payloadLength + rx_frame.actionData);

        if (rx_frame.startMarker == 0xAA && rx_frame.endMarker == 0x55 && rx_frame.checksum == expected_cs) {
            g_stm_connected = true;
            if (rx_frame.commandCode == CMD_ULTRASONIC) {
                g_telemetry_distance = rx_frame.actionData;
                g_distance_available = true;
            } else if (rx_frame.commandCode == CMD_TEMP) {
                g_telemetry_temp = (int8_t)rx_frame.actionData;
                g_temperature_available = true;
                g_temperature_received = true;
            }
        }
    }
}

// ============================================================================
// TAREA 1 (CORE 1): MOTOR GRAFICO LVGL & DIGITALIZADOR TACTIL
// TASK 1 (CORE 1): LVGL 8.3 GRAPHICS ENGINE & TOUCH DIGITIZER (66 FPS)
// ============================================================================
void vGuiTask(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    uint32_t last_lv_tick_ms = millis();

    if (xSemaphoreTake(xGuiMutex, portMAX_DELAY) == pdTRUE) {
        lv_scr_load(scr_splash);
        xSemaphoreGive(xGuiMutex);
    }

    const uint32_t splash_duration_ms = 2000;
    const uint32_t splash_start_ms = millis();
    const lv_coord_t dinosaur_start_x = splash_title_x - SPLASH_TRICERATOPS_WIDTH;
    const lv_coord_t dinosaur_end_x = LV_MAX(51, splash_title_x + splash_title_width - SPLASH_TRICERATOPS_WIDTH);
    const lv_coord_t dinosaur_travel = dinosaur_end_x - dinosaur_start_x;
    
    while (millis() - splash_start_ms < splash_duration_ms) {
        const uint32_t elapsed_ms = millis() - splash_start_ms;
        const lv_coord_t dinosaur_x = dinosaur_start_x + (dinosaur_travel * elapsed_ms / splash_duration_ms);
        lv_coord_t revealed_width = dinosaur_x + SPLASH_TRICERATOPS_WIDTH - splash_title_x;
        if (revealed_width < 0) revealed_width = 0;
        if (revealed_width > splash_title_width) revealed_width = splash_title_width;

        if (xSemaphoreTake(xGuiMutex, pdMS_TO_TICKS(15)) == pdTRUE) {
            update_splash_reveal(revealed_width, dinosaur_x);
            uint32_t now_ms = millis();
            lv_tick_inc(now_ms - last_lv_tick_ms);
            last_lv_tick_ms = now_ms;
            lv_timer_handler();
            xSemaphoreGive(xGuiMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }

    xLastWakeTime = xTaskGetTickCount();
    if (xSemaphoreTake(xGuiMutex, portMAX_DELAY) == pdTRUE) {
        lv_scr_load_anim(scr_dashboard, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0, false);
        xSemaphoreGive(xGuiMutex);
    }

    while (1) {
        if (xSemaphoreTake(xGuiMutex, pdMS_TO_TICKS(15)) == pdTRUE) {
            if (g_stm_connected) {
                lv_led_on(led_stm_status);
            } else {
                lv_led_off(led_stm_status);
            }

            if (g_temperature_available) {
                char temp_str[16];
                snprintf(temp_str, sizeof(temp_str), "%d °C", (int)g_telemetry_temp);
                lv_label_set_text(lbl_temp_val, temp_str);
                g_temperature_available = false;
            }
            if (g_distance_available) {
                lv_bar_set_value(bar_ultrasonic, g_telemetry_distance, LV_ANIM_OFF);
                lv_label_set_text_fmt(lbl_distance_val, "%u cm", (unsigned)g_telemetry_distance);
            } else {
                lv_bar_set_value(bar_ultrasonic, 0, LV_ANIM_OFF);
                lv_label_set_text(lbl_distance_val, "-- cm");
            }

            uint32_t now_ms = millis();
            lv_tick_inc(now_ms - last_lv_tick_ms);
            last_lv_tick_ms = now_ms;
            lv_timer_handler();
            xSemaphoreGive(xGuiMutex);
        }

        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(15));
    }
}

// ============================================================================
// TAREA 2 (CORE 0): CONTROL DETERMINISTA Y TELEMETRIA UART STM32
// TASK 2 (CORE 0): DETERMINISTIC UART DISPATCH & SENSOR TELEMETRY
// ============================================================================
void vControlTask(void *pvParameters) {
    QueuedCommand_t cmd;
    TickType_t xLastWakeTime = xTaskGetTickCount();
    uint32_t polling_ticks = 0;
    uint32_t telemetry_ticks = 0;

    while (1) {
        Process_Incoming_STM32_Data();

        // Enviar comandos disparados (vía Touch local o vía WebSocket remoto)
        if (xQueueReceive(xCommandDispatchQueue, &cmd, 0) == pdTRUE) {
            if (cmd.kind == CONTROL_MOTOR_SPEED && g_motor_action != 1) {
                Serial.printf("[MOTOR] Speed ignored while motor is inactive: %u\n", cmd.data);
            } else {
                if (cmd.kind == CONTROL_MOTOR_SPEED) {
                    g_motor_speed = cmd.data;
                } else if (cmd.kind == CONTROL_MOTOR_ACTION) {
                    g_motor_action = cmd.data;
                } else if (cmd.kind == CONTROL_SERVO) {
                    g_servo_angle = cmd.data;
                }
                Serial.printf("[UART TX -> STM32] Cmd: 0x%02X | Data: %u\n", cmd.cmd, cmd.data);
                Send_Command_To_STM32(cmd.cmd, cmd.data);
                applyLocalControlState(cmd.kind);
                broadcastControlState();
            }
        }

        // Sondeo periódico a STM32 cada 200 ms
        polling_ticks++;
        if (polling_ticks >= 10) {
            polling_ticks = 0;
            static bool poll_temp = false;
            if (poll_temp) {
                Send_Command_To_STM32(CMD_TEMP, 0x00);
            } else {
                Send_Command_To_STM32(CMD_ULTRASONIC, 0x00);
            }
            poll_temp = !poll_temp;
        }

        // Transmitir telemetría al celular conectado cada 500 ms
        telemetry_ticks++;
        if (telemetry_ticks >= 25) {
            telemetry_ticks = 0;
            broadcastTelemetry(g_telemetry_temp, g_telemetry_distance);
        }

        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(20));
    }
}

// ==========================================
// SETUP
// ==========================================
void setup() {
    Serial.begin(115200);
    delay(500);

    // 1. Control de CS para el bus SPI
    pinMode(TFT_CS_PIN, OUTPUT);
    pinMode(TOUCH_CS_PIN, OUTPUT);
    digitalWrite(TFT_CS_PIN, HIGH);
    digitalWrite(TOUCH_CS_PIN, HIGH);

    // 2. Pulso de Reset por hardware para el ILI9341
    pinMode(TFT_RST_PIN, OUTPUT);
    digitalWrite(TFT_RST_PIN, HIGH);
    delay(10);
    digitalWrite(TFT_RST_PIN, LOW);
    delay(20);
    digitalWrite(TFT_RST_PIN, HIGH);
    delay(150);

    // 3. Inicialización UART con STM32
    STM32_Serial.begin(115200, SERIAL_8N1, RXD1, TXD1);

    // 4. Inicializar Bus SPI nativo
    SPI.begin(SPI_SCLK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN, TFT_CS_PIN);

    // 5. Iniciar Pantalla a 16MHz en rotación 3
    tft.begin(16000000);
    tft.setRotation(3);

    tft.fillScreen(ILI9341_BLUE);
    delay(300);
    tft.fillScreen(ILI9341_BLACK);

    // 6. Iniciar Controlador Táctil
    touch.begin();
    touch.setRotation(0);

    // 7. Inicialización de LVGL
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf_1, NULL, SCREEN_WIDTH * 10);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCREEN_WIDTH;
    disp_drv.ver_res = SCREEN_HEIGHT;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    // 8. Construir interfaces gráficas sin esperar la conexión Wi-Fi
    Build_Splash_Screen();
    Build_Dashboard_Screen();
    Build_QR_Screen();

    // 9. Primitivas FreeRTOS y tareas
    xGuiMutex = xSemaphoreCreateMutex();
    xCommandDispatchQueue = xQueueCreate(15, sizeof(QueuedCommand_t));

    xTaskCreatePinnedToCore(vGuiTask, "GuiTask", 8192, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(vControlTask, "ControlTask", 4096, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(vWiFiTask, "WiFiTask", 4096, NULL, 2, NULL, 0);

    Serial.println("[HMI] Sistema HMI y Gateway Web inicializados correctamente.");
}

void loop() {
    // Mantiene la memoria limpia liberando clientes WebSocket desconectados
    ws.cleanupClients();
    vTaskDelay(pdMS_TO_TICKS(100));
}

void sendCurrentState(AsyncWebSocketClient *client) {
    JsonDocument doc;
    doc["motorSpeed"] = g_motor_speed;
    doc["motorAction"] = g_motor_action;
    doc["servo"] = g_servo_angle;
    if (g_distance_available) {
        doc["dist"] = g_telemetry_distance;
    }
    if (g_temperature_received) {
        doc["temp"] = g_telemetry_temp;
    }
    String output;
    serializeJson(doc, output);
    client->text(output);
}

void broadcastControlState() {
    if (ws.count() == 0) {
        return;
    }
    JsonDocument doc;
    doc["motorSpeed"] = g_motor_speed;
    doc["motorAction"] = g_motor_action;
    doc["servo"] = g_servo_angle;
    String output;
    serializeJson(doc, output);
    ws.textAll(output);
}

void applyLocalControlState(uint8_t kind) {
    if (xSemaphoreTake(xGuiMutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    g_syncing_controls = true;
    if (kind == CONTROL_MOTOR_SPEED) {
        lv_slider_set_value(slider_motor, g_motor_action == 1 ? g_motor_speed : 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(btn_motor_on,
                                  lv_color_hex(g_motor_action == 1 ? 0x00FF70 : 0x00D146), 0);
        lv_obj_set_style_bg_color(btn_motor_off,
                                  lv_color_hex(g_motor_action == 0 ? 0xFF5555 : 0xD10000), 0);
        lv_obj_set_style_bg_color(btn_motor_stop,
                                  lv_color_hex(g_motor_action == 5 ? 0xFFC04D : 0xF08B0F), 0);
    } else if (kind == CONTROL_MOTOR_ACTION) {
        if (g_motor_action == 1) {
            lv_slider_set_value(slider_motor, g_motor_speed, LV_ANIM_OFF);
            lv_obj_add_flag(slider_motor, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_slider_set_value(slider_motor, 0, LV_ANIM_OFF);
            lv_obj_clear_flag(slider_motor, LV_OBJ_FLAG_CLICKABLE);
        }
        lv_obj_set_style_bg_color(btn_motor_on,
                                  lv_color_hex(g_motor_action == 1 ? 0x00FF70 : 0x00D146), 0);
        lv_obj_set_style_bg_color(btn_motor_off,
                                  lv_color_hex(g_motor_action == 0 ? 0xFF5555 : 0xD10000), 0);
        lv_obj_set_style_bg_color(btn_motor_stop,
                                  lv_color_hex(g_motor_action == 5 ? 0xFFC04D : 0xF08B0F), 0);
    } else if (kind == CONTROL_SERVO) {
        lv_arc_set_value(arc_servo, map(g_servo_angle, 0, 180, 0, 100));
        lv_label_set_text_fmt(lbl_servo_value, "Servo: %u deg", g_servo_angle);
    }
    g_syncing_controls = false;
    xSemaphoreGive(xGuiMutex);
}