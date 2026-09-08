#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ESP32Servo.h>
#include <DNSServer.h>

// ESP32
#define TRIG_PIN 32
#define ECHO_PIN 33
#define BUZZER 25
#define SERVO_PIN 26

#define MOTOR_IN1 14
#define MOTOR_IN2 27
#define MOTOR_EN 13

// Servo
Servo steering;
int steerCenter = 77;
int carSpeed = 255;

// WebSocket
WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);
const byte DNS_PORT = 53;
DNSServer dnsServer;

// hardware/safety
unsigned long previousMicros = 0;
bool buzzerState = false;
unsigned long lastCommandTime = 0;

// controls layout
const char *htmlApp = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no, viewport-fit=cover">
  <title>RC Control</title>
  <style>
    * { box-sizing: border-box; }

    body { 
      margin: 0;
      padding: 0;
      background-color: #111;
      color: white;
      font-family: sans-serif;
      user-select: none;
      -webkit-user-select: none;
      overscroll-behavior: none;
      touch-action: none;
      display: flex;
      justify-content: center;
      align-items: center;
      width: 100vw;
      height: 100vh;
      height: 100dvh;
      overflow: hidden;
    }

    .d-pad {
      width: min(86vmin, 345px);
      height: min(86vmin, 345px);
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      grid-template-rows: repeat(3, 1fr);
      gap: 5px;
      transition: transform 0.2s;
    }

    .d-pad.rotated {
      transform: translateX(-16.67%) rotate(90deg);
    }

    .btn { 
      border: 0;
      background-color: #444;
      color: white;
      display: flex;
      justify-content: center;
      align-items: center;
      font-size: clamp(14px, 4vmin, 18px);
      font-weight: bold;
      box-shadow: 0 8px 15px rgba(0,0,0,0.5);
      transition: transform 0.05s, background-color 0.05s;
      touch-action: none;
    }

    .btn.active {
      background-color: #00ff88;
      color: black;
      transform: scale(0.90);
    }

    #btn-F { grid-column: 2; grid-row: 1; border-radius: 20px 20px 0 0; }
    #btn-L { grid-column: 1; grid-row: 2; border-radius: 20px 0 0 20px; }
    #btn-R { grid-column: 3; grid-row: 2; border-radius: 0 20px 20px 0; }
    #btn-B { grid-column: 2; grid-row: 2; }

    #rotate-btn {
      position: fixed;
      top: max(12px, env(safe-area-inset-top));
      right: max(12px, env(safe-area-inset-right));
      z-index: 2;
      border: 1px solid #666;
      border-radius: 12px;
      padding: 10px 14px;
      background: #333;
      color: white;
      font: inherit;
      font-weight: bold;
    }
  </style>
</head>
<body>
  <button id="rotate-btn">ROTATE</button>

  <div class="d-pad" id="d-pad">
    <button class="btn" id="btn-F" data-dir="F">FWD</button>
    <button class="btn" id="btn-L" data-dir="L">LEFT</button>
    <button class="btn" id="btn-R" data-dir="R">RIGHT</button>
    <button class="btn" id="btn-B" data-dir="B">REV</button>
  </div>

  <script>
    const ws = new WebSocket('ws://' + window.location.hostname + ':81/');
    const dPad = document.getElementById('d-pad');
    const rotateBtn = document.getElementById('rotate-btn');
    const buttons = [...document.querySelectorAll('[data-dir]')];
    const pointers = new Map();

    function command() {
      let isF = false;
      let isB = false;
      let isL = false;
      let isR = false;

      pointers.forEach(function(state) {
        if (state.drive === 'F') isF = true;
        if (state.drive === 'B') isB = true;
        if (state.steer === 'L') isL = true;
        if (state.steer === 'R') isR = true;
      });

      const t = (isF ? 1 : 0) - (isB ? 1 : 0);
      const s = (isR ? 1 : 0) - (isL ? 1 : 0);

      if (t === 1 && s === -1) return 'FL';
      if (t === 1 && s === 1) return 'FR';
      if (t === -1 && s === -1) return 'BL';
      if (t === -1 && s === 1) return 'BR';
      if (t === 1) return 'F';
      if (t === -1) return 'B';
      if (s === -1) return 'L';
      if (s === 1) return 'R';
      return 'S';
    }

    function sendState() {
      if (ws.readyState === WebSocket.OPEN) ws.send(command());
    }

    function showActive() {
      const active = new Set();

      pointers.forEach(function(state) {
        if (state.drive) active.add(state.drive);
        if (state.steer) active.add(state.steer);
      });

      buttons.forEach(function(button) {
        button.classList.toggle('active', active.has(button.dataset.dir));
      });
    }

    function directionAt(x, y) {
      const element = document.elementFromPoint(x, y);
      const button = element && element.closest('[data-dir]');
      return button ? button.dataset.dir : null;
    }

    function clearPending(state) {
      clearTimeout(state.pendingTimer);
      state.pendingDrive = null;
      state.pendingTimer = null;
      state.pendingX = null;
      state.pendingY = null;
    }

    function waitForReverse(pointerId, state, dir, x, y) {
      if (state.pendingDrive === dir &&
          Math.hypot(x - state.pendingX, y - state.pendingY) < 12) return;

      clearPending(state);
      state.pendingDrive = dir;
      state.pendingX = x;
      state.pendingY = y;
      state.pendingTimer = setTimeout(function() {
        if (pointers.get(pointerId) !== state || state.pendingDrive !== dir) return;
        state.drive = dir;
        state.steer = null;
        clearPending(state);
        showActive();
        sendState();
      }, 300);
    }

    function updatePointer(event) {
      if (!pointers.has(event.pointerId)) return;
      event.preventDefault();
      const dir = directionAt(event.clientX, event.clientY);
      const box = dPad.getBoundingClientRect();
      const state = pointers.get(event.pointerId);

      if (state.role === 'steer') {
        clearPending(state);
        if (dir === 'L' || dir === 'R') state.steer = dir;
        else if (event.clientX < box.left || event.clientX > box.right ||
                 event.clientY < box.top || event.clientY > box.bottom)
          state.steer = null;
      }
      else if (dir === 'L' || dir === 'R') {
        clearPending(state);
        state.steer = dir;
      }
      else if (dir === 'F' || dir === 'B') {
        if (state.drive && state.drive !== dir) {
          waitForReverse(event.pointerId, state, dir, event.clientX, event.clientY);
        }
        else {
          clearPending(state);
          state.drive = dir;
          state.steer = null;
        }
      }
      else if (event.clientX < box.left || event.clientX > box.right ||
               event.clientY < box.top || event.clientY > box.bottom) {
        clearPending(state);
        state.drive = null;
        state.steer = null;
      }
      else {
        clearPending(state);
      }

      showActive();
      sendState();
    }

    function releasePointer(event) {
      const state = pointers.get(event.pointerId);
      if (state) clearPending(state);
      pointers.delete(event.pointerId);
      showActive();
      sendState();
    }

    function releaseAll() {
      pointers.forEach(clearPending);
      pointers.clear();
      showActive();
      sendState();
    }

    buttons.forEach(function(button) {
      button.addEventListener('pointerdown', function(event) {
        event.preventDefault();
        const dir = button.dataset.dir;
        pointers.set(event.pointerId, {
          role: dir === 'L' || dir === 'R' ? 'steer' : 'drive',
          drive: dir === 'F' || dir === 'B' ? dir : null,
          steer: dir === 'L' || dir === 'R' ? dir : null,
          pendingDrive: null,
          pendingTimer: null,
          pendingX: null,
          pendingY: null
        });
        showActive();
        sendState();
      });
    });

    rotateBtn.addEventListener('click', function() {
      dPad.classList.toggle('rotated');
    });

    window.addEventListener('pointermove', updatePointer, { passive: false });
    window.addEventListener('pointerup', releasePointer);
    window.addEventListener('pointercancel', releasePointer);
    window.addEventListener('blur', releaseAll);
    document.addEventListener('visibilitychange', function() {
      if (document.hidden) releaseAll();
    });

    setInterval(sendState, 100);
  </script>
</body>
</html>
)rawliteral";

// hardware values
long getDistance()
{
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    long duration = pulseIn(ECHO_PIN, HIGH, 20000);
    if (duration == 0)
        return 999;
    return duration * 0.034 / 2;
}

void stopMotor()
{
    digitalWrite(MOTOR_IN1, LOW);
    digitalWrite(MOTOR_IN2, LOW);
    analogWrite(MOTOR_EN, 0);
}

void drive(String dir)
{
    if (dir.indexOf("L") != -1)
        steering.write(32);
    else if (dir.indexOf("R") != -1)
        steering.write(122);
    else
        steering.write(steerCenter);

    if (dir.indexOf("F") != -1)
    {
        digitalWrite(MOTOR_IN1, HIGH);
        digitalWrite(MOTOR_IN2, LOW);
        analogWrite(MOTOR_EN, carSpeed);
    }
    else if (dir.indexOf("B") != -1)
    {
        digitalWrite(MOTOR_IN1, LOW);
        digitalWrite(MOTOR_IN2, HIGH);
        analogWrite(MOTOR_EN, carSpeed);
    }
    else
    {
        stopMotor();
    }
}

// WebSocket Event Handler
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length)
{
    if (type == WStype_TEXT)
    {
        String command = (char *)payload;
        lastCommandTime = millis();
        drive(command);
    }
}

// main
void setup()
{
    Serial.begin(115200);

    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);
    pinMode(BUZZER, OUTPUT);
    pinMode(MOTOR_IN1, OUTPUT);
    pinMode(MOTOR_IN2, OUTPUT);
    pinMode(MOTOR_EN, OUTPUT);

    ESP32PWM::allocateTimer(0);
    steering.setPeriodHertz(50);
    steering.attach(SERVO_PIN, 500, 2400);
    steering.write(steerCenter);
    stopMotor();

    Serial.println("Starting Wi-Fi Access Point...");
    WiFi.softAP("rc_car");
    IPAddress IP = WiFi.softAPIP();

    dnsServer.start(DNS_PORT, "*", IP);

    server.on("/", []()
              { server.send(200, "text/html", htmlApp); });

    server.onNotFound([]()
                      { server.send(200, "text/html", htmlApp); });

    server.begin();

    webSocket.begin();
    webSocket.onEvent(webSocketEvent);

    Serial.println("System Ready!");
}

// loop
void loop()
{
    dnsServer.processNextRequest();
    webSocket.loop();
    server.handleClient();

    // dead man's switch
    if (millis() - lastCommandTime > 500)
    {
        stopMotor();
        steering.write(steerCenter);
    }

    // distance sensor
    long distance = getDistance();
    if (distance > 0 && distance < 4)
    {
        if (micros() - previousMicros >= 100)
        {
            previousMicros = micros();
            buzzerState = !buzzerState;
            digitalWrite(BUZZER, buzzerState);
        }
    }
    else
    {
        digitalWrite(BUZZER, LOW);
    }
}
