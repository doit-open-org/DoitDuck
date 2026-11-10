/*
 * DoitDuck IMU board: ESP32-C2 + LSM6DSV16X, device 200 on the servo bus.
 * Protocol and registers: README.md in this directory.
 *
 *   bus_task     highest priority: parses bus frames, answers from regs[]
 *   sample_task  reads the LSM6DSV16X FIFO every 2 ms, updates regs[], runs calibration
 *   app_main     NVS saves, UART0 console (keys 1/2/3 = calibration commands)
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define BUS_UART UART_NUM_1
#define BUS_TX_PIN 1
#define BUS_RX_PIN 2
#define BUS_TX_EN_PIN 0 /* SN74LVC2G125 1OE#：发送时拉低 */
#define IMU_MISO_PIN 4
#define IMU_MOSI_PIN 5
#define IMU_SCK_PIN 6
#define IMU_CS_PIN 7

#define BUS_ID 200
#define FW_VERSION 1
#define ERR_RANGE 0x08

/* 按传感器原始 XYZ 顺序上报，安装轴映射由主控完成。 */

/* LSM6DSV16X registers and FIFO tags. */
#define LSM_FUNC_CFG_ACCESS 0x01
#define LSM_FIFO_CTRL3 0x09
#define LSM_FIFO_CTRL4 0x0A
#define LSM_FIFO_STATUS1 0x1B
#define LSM_WHO_AM_I 0x0F
#define LSM_CTRL1 0x10
#define LSM_CTRL2 0x11
#define LSM_CTRL3 0x12
#define LSM_CTRL6 0x15
#define LSM_CTRL8 0x17
#define LSM_FIFO_DATA 0x78
#define EMB_FUNC_EN_A 0x04
#define EMB_FUNC_FIFO_EN_A 0x44
#define EMB_SFLP_ODR 0x5E
#define EMB_FUNC_INIT_A 0x66
#define TAG_GYRO 0x01
#define TAG_ACC 0x02
#define TAG_SFLP_GBIAS 0x16
#define TAG_SFLP_GRAVITY 0x17

#define AVG_N 5           /* 10 ms of 480 Hz samples */
#define CAL_SAMPLES 960   /* 2 s */
#define CAL_BOOT 4        /* the bias measurement at power-up, not reported in 0x61 */

static const char *TAG = "imu";

/* Register image read by the host; table in README.md. */
static uint8_t regs[0x66] = {[0x00] = 0xD0, [0x01] = 0x0D, [0x02] = FW_VERSION, [0x05] = BUS_ID};
static portMUX_TYPE regs_lock = portMUX_INITIALIZER_UNLOCKED;

/* Calibration kept in NVS. */
struct cal {
    float bias[3];  /* deg/s，传感器坐标，倾角校准前 */
    float mount[2]; /* roll, pitch in deg: what the IMU reads with the body level */
    uint8_t bias_valid, mount_valid;
};
static struct cal cal;
static volatile int cal_cmd = CAL_BOOT, mount_written, save_pending;

static QueueHandle_t uart_queue;
static spi_device_handle_t lsm;

static void put16(uint8_t *p, float v)
{
    const int x = (int)lroundf(fminf(fmaxf(v, -32768), 32767));
    p[0] = (uint8_t)x;
    p[1] = (uint8_t)(x >> 8);
}

/* ---- Bus ---- */

static void bus_reply(uint8_t err, const uint8_t *data, int len)
{
    uint8_t buf[6 + 32] = {0xFF, 0xFF, BUS_ID, (uint8_t)(len + 2), err};
    uint8_t sum = (uint8_t)(BUS_ID + len + 2 + err);
    for (int i = 0; i < len; i++) {
        buf[5 + i] = data[i];
        sum += data[i];
    }
    buf[5 + len] = (uint8_t)~sum;
    uart_write_bytes(BUS_UART, buf, len + 6);
}

/* frame: ID LEN INST PARAM... CHK, already checksummed and addressed to us. */
static void bus_frame(const uint8_t *f)
{
    const int n = f[1] - 2;
    const uint8_t *p = f + 3;
    uint8_t data[32];
    switch (f[2]) {
    case 0x01: /* PING */
        bus_reply(0, NULL, 0);
        break;
    case 0x02: /* READ addr len */
        if (n != 2 || p[1] == 0 || p[1] > 32 || p[0] + p[1] > (int)sizeof regs) {
            bus_reply(ERR_RANGE, NULL, 0);
            break;
        }
        portENTER_CRITICAL(&regs_lock);
        memcpy(data, regs + p[0], p[1]);
        portEXIT_CRITICAL(&regs_lock);
        bus_reply(0, data, p[1]);
        break;
    case 0x03: /* WRITE addr data... : calibration command, or mount roll/pitch */
        if (n == 2 && p[0] == 0x60 && p[1] >= 1 && p[1] <= 3) {
            cal_cmd = p[1];
        } else if (n == 5 && p[0] == 0x62) {
            portENTER_CRITICAL(&regs_lock);
            memcpy(regs + 0x62, p + 1, 4);
            portEXIT_CRITICAL(&regs_lock);
            mount_written = 1;
        } else {
            bus_reply(ERR_RANGE, NULL, 0);
            break;
        }
        bus_reply(0, NULL, 0);
        break;
    }
}

/* 接收主控请求和舵机应答，RS485 模式丢弃自身发送回波。 */
static void bus_task(void *arg)
{
    static uint8_t f[2 + 256];
    int state = 0, pos = 0;
    for (;;) {
        uart_event_t e;
        if (!xQueueReceive(uart_queue, &e, portMAX_DELAY)) continue;
        if (e.type == UART_FIFO_OVF || e.type == UART_BUFFER_FULL) {
            uart_flush_input(BUS_UART);
            xQueueReset(uart_queue);
            state = 0;
            continue;
        }
        if (e.type != UART_DATA) continue;
        uint8_t buf[128];
        int len;
        while ((len = uart_read_bytes(BUS_UART, buf, sizeof buf, 0)) > 0) {
            for (int i = 0; i < len; i++) {
                const uint8_t b = buf[i];
                switch (state) {
                case 0: state = b == 0xFF; break;
                case 1: state = b == 0xFF ? 2 : 0; break;
                case 2: /* a third 0xFF is still header */
                    if (b != 0xFF) {
                        f[0] = b;
                        state = 3;
                    }
                    break;
                case 3:
                    f[1] = b;
                    pos = 0;
                    state = b >= 2 ? 4 : 0;
                    break;
                case 4:
                    f[2 + pos++] = b;
                    if (pos < f[1]) break;
                    state = 0;
                    uint8_t sum = 0;
                    for (int k = 0; k <= f[1]; k++) sum += f[k];
                    /* Broadcast (0xFE), sync read and sync write are for the servos only. */
                    if ((uint8_t)~sum == f[1 + f[1]] && f[0] == BUS_ID) bus_frame(f);
                    break;
                }
            }
        }
    }
}

/* ---- LSM6DSV16X ---- */

static esp_err_t lsm_read(uint8_t reg, uint8_t *buf, int len)
{
    spi_transaction_t t = {.addr = reg | 0x80, .length = len * 8, .rx_buffer = buf};
    return spi_device_polling_transmit(lsm, &t);
}

static esp_err_t lsm_write(uint8_t reg, uint8_t value)
{
    spi_transaction_t t = {.addr = reg, .length = 8, .flags = SPI_TRANS_USE_TXDATA,
                           .tx_data = {value}};
    return spi_device_polling_transmit(lsm, &t);
}

/* Accel and gyro at 480 Hz into the FIFO, plus SFLP gyro bias and gravity at 120 Hz. */
static esp_err_t lsm_init(void)
{
    uint8_t who = 0, odr = 0;
    if (lsm_read(LSM_WHO_AM_I, &who, 1) != ESP_OK || who != 0x70) return ESP_FAIL;
    if (lsm_write(LSM_CTRL3, 0x01) != ESP_OK) return ESP_FAIL; /* software reset */
    vTaskDelay(pdMS_TO_TICKS(20));
    if (lsm_write(LSM_CTRL3, 0x44) || /* BDU, auto increment */
        lsm_write(LSM_CTRL8, 0x02) || /* +-8 g */
        lsm_write(LSM_CTRL6, 0x04) || /* +-2000 dps */
        lsm_write(LSM_FIFO_CTRL3, 0x88) || /* batch gyro and accel at 480 Hz */
        lsm_write(LSM_FUNC_CFG_ACCESS, 0x80) || /* embedded function registers */
        lsm_read(EMB_SFLP_ODR, &odr, 1) ||
        lsm_write(EMB_SFLP_ODR, (uint8_t)((odr & ~0x38) | 3 << 3)) || /* SFLP 120 Hz */
        lsm_write(EMB_FUNC_FIFO_EN_A, 0x30) || /* SFLP gravity and gyro bias into the FIFO */
        lsm_write(EMB_FUNC_EN_A, 0x02) || /* SFLP on */
        lsm_write(EMB_FUNC_INIT_A, 0x02) ||
        lsm_write(LSM_FUNC_CFG_ACCESS, 0x00) ||
        lsm_write(LSM_FIFO_CTRL4, 0x06) || /* FIFO continuous */
        lsm_write(LSM_CTRL1, 0x08) || /* accel 480 Hz, high performance */
        lsm_write(LSM_CTRL2, 0x08)) /* gyro 480 Hz, high performance */
        return ESP_FAIL;
    ESP_LOGI(TAG, "LSM6DSV16X on SPI");
    return ESP_OK;
}

/* 解码 FIFO 中的 XYZ，只做单位换算，不交换轴或翻转单轴方向。 */
static void decode_xyz(const uint8_t *w, float scale, float v[3])
{
    for (int k = 0; k < 3; k++) {
        v[k] = scale * (int16_t)(w[1 + 2 * k] | w[2 + 2 * k] << 8);
    }
}

/* The mount correction: rotate about x by roll, then about y by pitch. */
static void level(float v[3], const float mount[2])
{
    const float r = mount[0] * (float)M_PI / 180, p = mount[1] * (float)M_PI / 180;
    const float x = v[0], y = cosf(r) * v[1] - sinf(r) * v[2], z = sinf(r) * v[1] + cosf(r) * v[2];
    v[0] = cosf(p) * x + sinf(p) * z;
    v[1] = y;
    v[2] = -sinf(p) * x + cosf(p) * z;
}

static void sample_task(void *arg)
{
    float acc_h[AVG_N][3] = {0}, gyro_h[AVG_N][3] = {0}, grav[3] = {0};
    float sum[3] = {0}, lo[3] = {0}, hi[3] = {0};
    int ah = 0, gh = 0, nacc = 0, ngyro = 0, errors = 0, ready = 0, cal_n = 0, running = 0;
    uint16_t count = 0;
    TickType_t last_init = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2));
        if (!ready && xTaskGetTickCount() - last_init >= pdMS_TO_TICKS(1000)) {
            last_init = xTaskGetTickCount();
            ready = lsm_init() == ESP_OK;
            errors = nacc = ngyro = 0;
            if (!ready) ESP_LOGW(TAG, "LSM6DSV16X not found, retrying");
        }

        if (cal_cmd) {
            running = cal_cmd;
            cal_cmd = 0;
            cal_n = 0;
            for (int k = 0; k < 3; k++) {
                sum[k] = 0;
                lo[k] = INFINITY;
                hi[k] = -INFINITY;
            }
            if (running == 3) {
                memset(&cal, 0, sizeof cal);
                save_pending = 1;
                running = 0;
            }
            if (running != CAL_BOOT) {
                portENTER_CRITICAL(&regs_lock);
                regs[0x61] = running ? 1 : 2;
                portEXIT_CRITICAL(&regs_lock);
            }
        }
        if (mount_written) {
            mount_written = 0;
            portENTER_CRITICAL(&regs_lock);
            for (int k = 0; k < 2; k++) cal.mount[k] = (int16_t)(regs[0x62 + 2 * k] | regs[0x63 + 2 * k] << 8) / 100.0f;
            portEXIT_CRITICAL(&regs_lock);
            cal.mount_valid = 1;
            save_pending = 1;
        }

        uint8_t st[2], w[7];
        int n = 0;
        if (ready) {
            if (lsm_read(LSM_FIFO_STATUS1, st, 2) == ESP_OK) n = st[0] | (st[1] & 1) << 8;
            else errors++;
        }
        for (; n > 0; n--) {
            if (lsm_read(LSM_FIFO_DATA, w, 7) != ESP_OK) {
                errors++;
                break;
            }
            errors = 0;
            float v[3];
            switch (w[0] >> 3) {
            case TAG_GYRO:
                decode_xyz(w, 0.070f, v); /* 70 mdps/LSB at +-2000 dps */
                if (running == 1 || (running == CAL_BOOT && count >= 240)) { /* skip the first 0.5 s after power-up */
                    for (int k = 0; k < 3; k++) {
                        sum[k] += v[k];
                        lo[k] = fminf(lo[k], v[k]);
                        hi[k] = fmaxf(hi[k], v[k]);
                    }
                    cal_n++;
                }
                for (int k = 0; k < 3; k++) v[k] -= cal.bias[k];
                level(v, cal.mount);
                memcpy(gyro_h[gh], v, sizeof v);
                gh = (gh + 1) % AVG_N;
                ngyro++;
                count++;
                break;
            case TAG_ACC:
                decode_xyz(w, -0.244f, v); /* 0.244 mg/LSB at +-8 g */
                if (running == 2) {
                    for (int k = 0; k < 3; k++) {
                        sum[k] += v[k];
                        lo[k] = fminf(lo[k], v[k]);
                        hi[k] = fmaxf(hi[k], v[k]);
                    }
                    cal_n++;
                }
                level(v, cal.mount);
                memcpy(acc_h[ah], v, sizeof v);
                ah = (ah + 1) % AVG_N;
                nacc++;
                break;
            case TAG_SFLP_GBIAS:
                /* SFLP only estimates the bias at rest and leaves it in the raw gyro; zero until it has one. */
                decode_xyz(w, 0.004375f, v);
                if (v[0] != 0 || v[1] != 0 || v[2] != 0) {
                    memcpy(cal.bias, v, sizeof v);
                    cal.bias_valid = 1;
                }
                break;
            case TAG_SFLP_GRAVITY:
                decode_xyz(w, -1.0f, v); /* 1/16384 g per LSB; sign as accel, check on the real board */
                level(v, cal.mount);
                memcpy(grav, v, sizeof v);
                break;
            }
        }
        if (errors >= 10) {
            ready = 0;
            ESP_LOGW(TAG, "LSM6DSV16X SPI errors, reinitializing");
        }

        /* Calibration result after 2 s: fails when the robot moved by more than 1 deg/s or 1 deg. */
        if (running && cal_n >= CAL_SAMPLES) {
            int ok = 1;
            float mean[3];
            for (int k = 0; k < 3; k++) {
                mean[k] = sum[k] / cal_n;
                if (hi[k] - lo[k] > (running == 2 ? 1000 * sinf(1 * (float)M_PI / 180) : 1.0f)) ok = 0;
            }
            if (ok && running == 2) {
                /* Roll levels y, then pitch levels x, as level() applies them. */
                const float roll = atan2f(-mean[1], -mean[2]);
                const float z = sinf(roll) * mean[1] + cosf(roll) * mean[2];
                const float pitch = atan2f(mean[0], -z);
                ok = fabsf(roll) < 15 * (float)M_PI / 180 && fabsf(pitch) < 15 * (float)M_PI / 180;
                if (ok) {
                    cal.mount[0] = roll * 180 / (float)M_PI;
                    cal.mount[1] = pitch * 180 / (float)M_PI;
                    cal.mount_valid = 1;
                }
            } else if (ok) {
                memcpy(cal.bias, mean, sizeof mean);
                cal.bias_valid = 1;
            }
            if (ok) save_pending = 1;
            if (running != CAL_BOOT) {
                portENTER_CRITICAL(&regs_lock);
                regs[0x61] = ok ? 2 : 3;
                portEXIT_CRITICAL(&regs_lock);
            }
            ESP_LOGI(TAG, "calibration %d %s", running, ok ? "done" : "failed: not still");
            running = 0;
        }

        /* Snapshot for the host: 0x40-0x5B, plus mount roll/pitch at 0x62. */
        uint8_t out[0x5C - 0x40] = {0}, mount[4];
        float a[3] = {0}, g[3] = {0};
        const int na = nacc < AVG_N ? nacc : AVG_N, ng = ngyro < AVG_N ? ngyro : AVG_N;
        for (int i = 0; i < AVG_N; i++) {
            for (int k = 0; k < 3; k++) {
                a[k] += acc_h[i][k];
                g[k] += gyro_h[i][k];
            }
        }
        for (int k = 0; k < 3; k++) {
            put16(out + 2 * k, na ? a[k] / na : 0);                  /* mg */
            put16(out + 0x06 + 2 * k, ng ? g[k] / ng * 10 : 0);      /* 0.1 deg/s */
            put16(out + 0x10 + 2 * k, grav[k]);                      /* 1/16384 g */
            put16(out + 0x16 + 2 * k, cal.bias[k] * 100);            /* 0.01 deg/s */
        }
        out[0x0C] = (uint8_t)count;
        out[0x0D] = (uint8_t)(count >> 8);
        out[0x0E] = (uint8_t)((ready && na && ng) | cal.bias_valid << 1 | cal.mount_valid << 2 | !ready << 3);
        put16(mount, cal.mount[0] * 100);
        put16(mount + 2, cal.mount[1] * 100);
        portENTER_CRITICAL(&regs_lock);
        memcpy(regs + 0x40, out, sizeof out);
        if (!mount_written) memcpy(regs + 0x62, mount, sizeof mount);
        portEXIT_CRITICAL(&regs_lock);
    }
}

/* ---- Start-up, NVS, console ---- */

void app_main(void)
{
    if (nvs_flash_init() != ESP_OK) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open("imu", NVS_READWRITE, &nvs));
    size_t size = sizeof cal;
    if (nvs_get_blob(nvs, "cal", &cal, &size) != ESP_OK || size != sizeof cal) memset(&cal, 0, sizeof cal);
    struct cal saved = cal;
    ESP_LOGI(TAG, "bias %.2f %.2f %.2f deg/s, mount %.2f %.2f deg", cal.bias[0], cal.bias[1], cal.bias[2],
             cal.mount[0], cal.mount[1]);

    const spi_bus_config_t spi = {.mosi_io_num = IMU_MOSI_PIN, .miso_io_num = IMU_MISO_PIN,
                                  .sclk_io_num = IMU_SCK_PIN, .quadwp_io_num = -1, .quadhd_io_num = -1};
    const spi_device_interface_config_t dev = {.address_bits = 8, .mode = 3, .clock_speed_hz = 10000000,
                                              .spics_io_num = IMU_CS_PIN, .queue_size = 1};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &spi, SPI_DMA_DISABLED));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &lsm));

    /* RS485 半双工模式下 RTS 发送时为高、空闲为低（DE 高有效），与 1OE# 相反，所以反相：发送时 GPIO0 低。 */
    const uart_config_t uart = {.baud_rate = 1000000, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                                .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                                .source_clk = UART_SCLK_DEFAULT};
    ESP_ERROR_CHECK(uart_driver_install(BUS_UART, 1024, 0, 16, &uart_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(BUS_UART, &uart));
    ESP_ERROR_CHECK(uart_set_pin(BUS_UART, BUS_TX_PIN, BUS_RX_PIN, BUS_TX_EN_PIN, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_line_inverse(BUS_UART, UART_SIGNAL_RTS_INV));
    ESP_ERROR_CHECK(uart_set_rts(BUS_UART, 1));
    ESP_ERROR_CHECK(uart_set_mode(BUS_UART, UART_MODE_RS485_HALF_DUPLEX));
    /* 总线空闲 2 个字节时间后唤醒 bus_task。 */
    ESP_ERROR_CHECK(uart_set_rx_timeout(BUS_UART, 2));

    xTaskCreate(bus_task, "bus", 4096, NULL, configMAX_PRIORITIES - 1, NULL);
    xTaskCreate(sample_task, "sample", 4096, NULL, configMAX_PRIORITIES - 2, NULL);

    /* UART0 使用原理图的 U0RXD/U0TXD 调试接口，波特率与启动日志相同（26 MHz 晶振为 74880）。 */
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_set_baudrate(UART_NUM_0, CONFIG_ESP_CONSOLE_UART_BAUDRATE));
    for (int tick = 1;; tick++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        char c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, 0) == 1 && c >= '1' && c <= '3') cal_cmd = c - '0';
        if (tick % 10) continue;

        /* 本板没有电池分压采样，寄存器 0x5C 保持为零。 */
        portENTER_CRITICAL(&regs_lock);
        uint8_t snap[0x10];
        memcpy(snap, regs + 0x40, sizeof snap);
        portEXIT_CRITICAL(&regs_lock);

        /* Explicit calibrations at once; the SFLP bias once a minute when it has moved. */
        const int moved = fabsf(cal.bias[0] - saved.bias[0]) + fabsf(cal.bias[1] - saved.bias[1]) +
                          fabsf(cal.bias[2] - saved.bias[2]) > 0.05f;
        if (save_pending || (tick % 600 == 0 && moved)) {
            save_pending = 0;
            saved = cal;
            nvs_set_blob(nvs, "cal", &saved, sizeof saved);
            nvs_commit(nvs);
            ESP_LOGI(TAG, "saved bias %.2f %.2f %.2f deg/s, mount %.2f %.2f deg", saved.bias[0], saved.bias[1],
                     saved.bias[2], saved.mount[0], saved.mount[1]);
        }

        int16_t s[8];
        for (int i = 0; i < 8; i++) s[i] = (int16_t)(snap[2 * i] | snap[2 * i + 1] << 8);
        ESP_LOGI(TAG, "acc %d %d %d mg  gyro %.1f %.1f %.1f deg/s  bias %.2f %.2f %.2f  status 0x%02x  battery %d mV",
                 s[0], s[1], s[2], s[3] / 10.0f, s[4] / 10.0f, s[5] / 10.0f, cal.bias[0], cal.bias[1], cal.bias[2],
                 snap[0x0E], 0);
    }
}
