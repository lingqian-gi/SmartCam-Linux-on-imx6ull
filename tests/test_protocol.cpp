/**
 * @file    test_protocol.cpp
 * @brief   TCP 私有控制协议单元测试
 *
 * 测试内容:
 *   1. CRC-16/MODBUS 校验值（已知答案测试）
 *   2. 响应帧序列化/反序列化
 *   3. 帧边界解析（魔数检测、负载长度校验）
 *
 * 编译:
 *   cd tests && g++ -std=c++17 -I.. -o test_protocol test_protocol.cpp \
 *       ../src/network/control.cpp && ./test_protocol
 */

#include <cstdio>
#include <cstring>
#include <cassert>
#include <arpa/inet.h>

#include "include/network/control.h"

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name) \
    printf("  [TEST] %s ... ", name)

#define PASS() \
    do { printf("PASS\n"); testsPassed++; } while(0)

#define FAIL(msg) \
    do { printf("FAIL: %s\n", msg); testsFailed++; } while(0)

#define ASSERT_EQ(expected, actual, msg) \
    do { \
        if ((expected) != (actual)) { \
            printf("FAIL: %s (expected=%d, actual=%d)\n", msg, \
                   static_cast<int>(expected), static_cast<int>(actual)); \
            testsFailed++; \
            return; \
        } \
    } while(0)

// ============================================================
// Test 1: CRC-16/MODBUS 已知答案测试
// ============================================================
static void test_crc16_known_values() {
    TEST("CRC16 known values");

    // 空数据 CRC16
    uint16_t crcEmpty = crc16Modbus(nullptr, 0);
    ASSERT_EQ(0xFFFF, crcEmpty, "CRC16 of empty buffer should be 0xFFFF");

    // 单字节 0x00
    uint8_t singleZero[] = {0x00};
    uint16_t crcSingle = crc16Modbus(singleZero, 1);
    ASSERT_EQ(0x40BF, crcSingle, "CRC16 of [0x00] should be 0x40BF");

    // 字符串 "123456789"
    const uint8_t* knownStr = reinterpret_cast<const uint8_t*>("123456789");
    uint16_t crcKnown = crc16Modbus(knownStr, 9);
    // MODBUS CRC16 for "123456789" is 0x4B37
    ASSERT_EQ(0x4B37, crcKnown, "CRC16 of '123456789' should be 0x4B37");

    PASS();
}

// ============================================================
// Test 2: Response frame packing
// ============================================================
static void test_pack_response_no_payload() {
    TEST("packResponse (no payload)");

    uint8_t buf[256];
    int len = packResponse(CMD_CAPTURE, STATUS_OK, nullptr, 0, buf);

    // 最小响应帧: magic[2] + version[1] + cmd[1] + status[1] + plen[2] + crc[2] = 9
    ASSERT_EQ(9, len, "Response frame len should be 9");

    // 验证 magic
    ASSERT_EQ(kProtoMagic0, buf[0], "magic[0] should be 0xEB");
    ASSERT_EQ(kProtoMagic1, buf[1], "magic[1] should be 0x90");

    // 验证 version
    ASSERT_EQ(kProtoVersion, buf[2], "version should be 0x01");

    // 验证 cmd (CMD_CAPTURE | 0x80)
    ASSERT_EQ(static_cast<uint8_t>(CMD_CAPTURE | kResponseFlag), buf[3],
              "cmd should be CMD_CAPTURE | 0x80");

    // 验证 status
    ASSERT_EQ(STATUS_OK, buf[4], "status should be STATUS_OK");

    // 验证 payload_len = 0 (network byte order)
    ASSERT_EQ(0, buf[5], "payload_len low byte should be 0");
    ASSERT_EQ(0, buf[6], "payload_len high byte should be 0");

    PASS();
}

static void test_pack_response_with_payload() {
    TEST("packResponse (with payload)");

    const uint8_t payload[] = {'H', 'e', 'l', 'l', 'o'};
    uint8_t buf[256];
    int len = packResponse(CMD_GET_STATUS, STATUS_OK, payload, 5, buf);

    // 响应帧: header(7) + payload(5) + crc(2) = 14
    ASSERT_EQ(14, len, "Response frame len should be 14");

    // 验证 cmd
    ASSERT_EQ(static_cast<uint8_t>(CMD_GET_STATUS | kResponseFlag), buf[3],
              "cmd should be CMD_GET_STATUS | 0x80");

    // 验证 payload_len = 5 (network byte order)
    uint16_t plen = static_cast<uint16_t>(buf[5] | (buf[6] << 8));
    ASSERT_EQ(5, ntohs(plen), "payload_len should be 5");

    // 验证 payload 内容
    for (int i = 0; i < 5; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "payload[%d] mismatch", i);
        ASSERT_EQ(payload[i], buf[7 + i], msg);
    }

    // 验证 CRC 可被重新校验
    uint16_t net_crc = static_cast<uint16_t>(buf[len - 2] | (buf[len - 1] << 8));
    uint16_t crc = ntohs(net_crc);
    uint16_t calc_crc = crc16Modbus(buf, len - 2);
    ASSERT_EQ(calc_crc, crc, "embedded CRC should be valid");

    PASS();
}

static void test_pack_response_error_status() {
    TEST("packResponse (error status)");

    uint8_t buf[256];
    int len = packResponse(CMD_SET_RESOLUTION, STATUS_BAD_PARAM, nullptr, 0, buf);

    ASSERT_EQ(9, len, "Error response len should be 9");
    ASSERT_EQ(static_cast<uint8_t>(CMD_SET_RESOLUTION | kResponseFlag), buf[3],
              "cmd should be CMD_SET_RESOLUTION | 0x80");
    ASSERT_EQ(STATUS_BAD_PARAM, buf[4], "status should be STATUS_BAD_PARAM");

    PASS();
}

// ============================================================
// Test 3: StatusPayload struct layout
// ============================================================
static void test_status_payload_layout() {
    TEST("StatusPayload struct layout");

    StatusPayload sp;
    memset(&sp, 0, sizeof(sp));

    sp.streaming    = 1;
    sp.recording    = 0;
    sp.client_count = 3;
    sp.width        = htons(1280);
    sp.height       = htons(720);
    sp.format       = 1;
    sp.fps          = 25;

    ASSERT_EQ(1, sp.streaming, "streaming should be 1");
    ASSERT_EQ(0, sp.recording, "recording should be 0");
    ASSERT_EQ(3, sp.client_count, "client_count should be 3");
    ASSERT_EQ(1280, static_cast<int>(ntohs(sp.width)), "width should be 1280");
    ASSERT_EQ(720,  static_cast<int>(ntohs(sp.height)), "height should be 720");
    ASSERT_EQ(1, sp.format, "format should be 1 (MJPEG)");
    ASSERT_EQ(25, sp.fps, "fps should be 25");

    // 序列化到响应负载
    uint8_t payload[sizeof(StatusPayload)];
    payload[0] = sp.streaming;
    payload[1] = sp.recording;
    payload[2] = sp.client_count;
    payload[3] = sp.reserved;
    memcpy(payload + 4, &sp.width, 2);
    memcpy(payload + 6, &sp.height, 2);
    payload[8] = sp.format;
    payload[9] = sp.fps;

    uint8_t buf[256];
    int len = packResponse(CMD_GET_STATUS, STATUS_OK, payload,
                           sizeof(StatusPayload), buf);
    ASSERT_EQ(static_cast<int>(7 + 2 + sizeof(StatusPayload)), len,
              "Status response len should be 7+2+10 = 19");

    PASS();
}

// ============================================================
// Test 4: Command constants
// ============================================================
static void test_command_constants() {
    TEST("Command constants");

    ASSERT_EQ(0x01, CMD_CAPTURE, "CMD_CAPTURE should be 0x01");
    ASSERT_EQ(0x02, CMD_START_RECORD, "CMD_START_RECORD should be 0x02");
    ASSERT_EQ(0x03, CMD_STOP_RECORD, "CMD_STOP_RECORD should be 0x03");
    ASSERT_EQ(0x10, CMD_SET_RESOLUTION, "CMD_SET_RESOLUTION should be 0x10");
    ASSERT_EQ(0x11, CMD_SET_FORMAT, "CMD_SET_FORMAT should be 0x11");
    ASSERT_EQ(0x20, CMD_GET_STATUS, "CMD_GET_STATUS should be 0x20");
    ASSERT_EQ(0xFF, CMD_HEARTBEAT, "CMD_HEARTBEAT should be 0xFF");

    ASSERT_EQ(0x81, responseCmd(CMD_CAPTURE), "responseCmd(CAPTURE) should be 0x81");
    ASSERT_EQ(0x90, responseCmd(CMD_SET_RESOLUTION),
              "responseCmd(SET_RESOLUTION) should be 0x90");
    ASSERT_EQ(0xA0, responseCmd(CMD_GET_STATUS),
              "responseCmd(GET_STATUS) should be 0xA0");
    ASSERT_EQ(0xFF, responseCmd(CMD_HEARTBEAT),
              "responseCmd(HEARTBEAT) should be 0xFF");

    PASS();
}

// ============================================================
// Test 5: Status code constants
// ============================================================
static void test_status_constants() {
    TEST("Status code constants");

    ASSERT_EQ(0x00, STATUS_OK,           "STATUS_OK should be 0x00");
    ASSERT_EQ(0x01, STATUS_UNKNOWN_CMD,   "STATUS_UNKNOWN_CMD should be 0x01");
    ASSERT_EQ(0x02, STATUS_BAD_PARAM,     "STATUS_BAD_PARAM should be 0x02");
    ASSERT_EQ(0x03, STATUS_CRC_ERROR,     "STATUS_CRC_ERROR should be 0x03");
    ASSERT_EQ(0x04, STATUS_INTERNAL_ERR,  "STATUS_INTERNAL_ERR should be 0x04");
    ASSERT_EQ(0x05, STATUS_BUSY,          "STATUS_BUSY should be 0x05");
    ASSERT_EQ(0x06, STATUS_NOT_SUPPORTED, "STATUS_NOT_SUPPORTED should be 0x06");

    PASS();
}

// ============================================================
// Test 6: calcFrameCRC consistency
// ============================================================
static void test_calc_frame_crc() {
    TEST("calcFrameCRC consistency");

    // 构建一个简单的帧
    ProtoHeader hdr;
    hdr.magic[0] = kProtoMagic0;
    hdr.magic[1] = kProtoMagic1;
    hdr.version  = kProtoVersion;
    hdr.cmd      = CMD_HEARTBEAT;
    hdr.payload_len = htons(0);

    uint16_t crc1 = calcFrameCRC(hdr, nullptr);
    uint16_t crc2 = calcFrameCRC(hdr, nullptr);

    ASSERT_EQ(crc1, crc2, "calcFrameCRC should be deterministic");

    // 改变 cmd，CRC 应变
    hdr.cmd = CMD_CAPTURE;
    uint16_t crc3 = calcFrameCRC(hdr, nullptr);
    if (crc1 == crc3) {
        printf("FAIL: CRC should change when cmd changes "
               "(crc1=0x%04X, crc3=0x%04X)\n", crc1, crc3);
        testsFailed++;
        return;
    }

    PASS();
}

// ============================================================
// main
// ============================================================
int main() {
    printf("\n========================================\n");
    printf("  TCP Control Protocol Unit Tests\n");
    printf("========================================\n\n");

    test_crc16_known_values();
    test_pack_response_no_payload();
    test_pack_response_with_payload();
    test_pack_response_error_status();
    test_status_payload_layout();
    test_command_constants();
    test_status_constants();
    test_calc_frame_crc();

    printf("\n========================================\n");
    printf("  Results: %d passed, %d failed\n",
           testsPassed, testsFailed);
    printf("========================================\n\n");

    return (testsFailed == 0) ? 0 : 1;
}
