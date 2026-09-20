#include "motor_control.h"

#include "config.h"
#include "robot_state.h"

TaskHandle_t g_motorTaskHandle = nullptr;

namespace {

constexpr int CH_FL = 0, CH_FR = 1, CH_RL = 2, CH_RR = 3;
constexpr int DUTY_MAX = (1 << MOTOR_PWM_RES_BITS) - 1;

int32_t clampi(int32_t v, int32_t lo, int32_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// Signed duty (-DUTY_MAX..DUTY_MAX): sign picks direction via IN1/IN2,
// magnitude is the PWM duty on the channel's EN pin.
void setMotor(int channel, int in1, int in2, int32_t signedDuty) {
  if (signedDuty > 0) {
    digitalWrite(in1, HIGH);
    digitalWrite(in2, LOW);
  } else if (signedDuty < 0) {
    digitalWrite(in1, LOW);
    digitalWrite(in2, HIGH);
  } else {
    digitalWrite(in1, LOW);
    digitalWrite(in2, LOW);
  }
  ledcWrite(channel, abs(signedDuty));
}

void allStop() {
  digitalWrite(PIN_FL_IN1, LOW); digitalWrite(PIN_FL_IN2, LOW);
  digitalWrite(PIN_FR_IN1, LOW); digitalWrite(PIN_FR_IN2, LOW);
  digitalWrite(PIN_RL_IN1, LOW); digitalWrite(PIN_RL_IN2, LOW);
  digitalWrite(PIN_RR_IN1, LOW); digitalWrite(PIN_RR_IN2, LOW);
  ledcWrite(CH_FL, 0); ledcWrite(CH_FR, 0);
  ledcWrite(CH_RL, 0); ledcWrite(CH_RR, 0);
}

}  // namespace

void motorControlTask(void*) {
  Serial.printf("motor started on core %d\n", xPortGetCoreID());
  logStack("motor ");

  pinMode(PIN_FL_IN1, OUTPUT); pinMode(PIN_FL_IN2, OUTPUT);
  pinMode(PIN_FR_IN1, OUTPUT); pinMode(PIN_FR_IN2, OUTPUT);
  pinMode(PIN_RL_IN1, OUTPUT); pinMode(PIN_RL_IN2, OUTPUT);
  pinMode(PIN_RR_IN1, OUTPUT); pinMode(PIN_RR_IN2, OUTPUT);

  ledcSetup(CH_FL, MOTOR_PWM_FREQ_HZ, MOTOR_PWM_RES_BITS);
  ledcSetup(CH_FR, MOTOR_PWM_FREQ_HZ, MOTOR_PWM_RES_BITS);
  ledcSetup(CH_RL, MOTOR_PWM_FREQ_HZ, MOTOR_PWM_RES_BITS);
  ledcSetup(CH_RR, MOTOR_PWM_FREQ_HZ, MOTOR_PWM_RES_BITS);
  ledcAttachPin(PIN_FL_EN, CH_FL);
  ledcAttachPin(PIN_FR_EN, CH_FR);
  ledcAttachPin(PIN_RL_EN, CH_RL);
  ledcAttachPin(PIN_RR_EN, CH_RR);

  allStop();

  uint32_t cycles = 0;

  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));

    RobotState snap = readState();

    if (millis() - snap.inputAtMs > MOTOR_STALE_MS) {
      allStop();
    } else {
      // Skid steering (ECU-SPEC-001 §6): throttle sets magnitude, steer
      // differentiates left/right; front and rear on a side share duty.
      int32_t left  = clampi(snap.throttle + snap.steer, -AXIS_MAX, AXIS_MAX);
      int32_t right = clampi(snap.throttle - snap.steer, -AXIS_MAX, AXIS_MAX);

      float ratio = snap.tcuLinkUp ? snap.degradationRatio : MOTOR_LIMP_RATIO;
      ratio = constrain(ratio, 0.0f, 1.0f);

      int32_t leftDuty  = (int32_t)((int64_t)left  * ratio * DUTY_MAX / AXIS_MAX);
      int32_t rightDuty = (int32_t)((int64_t)right * ratio * DUTY_MAX / AXIS_MAX);

      setMotor(CH_FL, PIN_FL_IN1, PIN_FL_IN2, leftDuty);
      setMotor(CH_RL, PIN_RL_IN1, PIN_RL_IN2, leftDuty);
      setMotor(CH_FR, PIN_FR_IN1, PIN_FR_IN2, rightDuty);
      setMotor(CH_RR, PIN_RR_IN1, PIN_RR_IN2, rightDuty);
    }

    if ((cycles % 300) == 299) logStack("motor ");
    cycles++;
  }
}
