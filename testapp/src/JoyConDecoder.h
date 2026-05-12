#pragma once
#include <vector>
#include <utility>
#include <cstdint>
#include <Windows.h>
#include <ViGEm/Client.h>

enum class JoyConSide { Left, Right };
enum class JoyConOrientation { Upright, Sideways };
enum class GyroSource { Both, Left, Right };
enum class GyroMode { DS4Raw, DS4SwitchEmu, DsuUdp };
enum class MotionProfile { Raw, SwitchEmu };

struct StickData {
    int16_t x;
    int16_t y;
    BYTE rx;
    BYTE ry;
};

struct MotionData {
    SHORT gyroX, gyroY, gyroZ;
    SHORT accelX, accelY, accelZ;
};

DS4_REPORT_EX GenerateDS4Report(const std::vector<uint8_t>& buffer, JoyConSide side, JoyConOrientation orientation, MotionProfile profile = MotionProfile::Raw);
DS4_REPORT_EX GenerateDualJoyConDS4Report(const std::vector<uint8_t>& leftBuffer, const std::vector<uint8_t>& rightBuffer, GyroSource gyroSource, MotionProfile profile = MotionProfile::Raw);
DS4_REPORT_EX GenerateProControllerReport(const std::vector<uint8_t>& buffer, MotionProfile profile = MotionProfile::Raw);
DS4_REPORT_EX GenerateNSOGCReport(const std::vector<uint8_t>& buffer);

uint32_t ExtractButtonState(const std::vector<uint8_t>& buffer);
std::pair<int16_t, int16_t> GetRawOpticalMouse(const std::vector<uint8_t>& buffer);
StickData DecodeJoystick(const std::vector<uint8_t>& buffer, JoyConSide side, JoyConOrientation orientation);
MotionData DecodeMotion(const std::vector<uint8_t>& buffer);
MotionData DecodeMotionRaw(const std::vector<uint8_t>& buffer);
MotionData TransformMotion(MotionData raw, MotionProfile profile, JoyConSide side = JoyConSide::Right);
