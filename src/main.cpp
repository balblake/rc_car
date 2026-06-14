#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ESP32Servo.h>
#include <DNSServer.h>

// ==========================================
// PIN DEFINITIONS (ESP32)
// ==========================================
#define TRIG_PIN 32
#define ECHO_PIN 33
#define BUZZER 25
#define SERVO_PIN 26

#define MOTOR_IN1 14 
#define MOTOR_IN2 27 
#define MOTOR_EN 13  

// ==========================================
// GLOBAL OBJECTS & VARIABLES
// ==========================================
Servo steering;
int steerCenter = 77;
int carSpeed = 255;

WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);
const byte DNS_PORT = 53;
DNSServer dnsServer; 

unsigned long previousMicros = 0;
bool buzzerState = false;
unsigned long lastCommandTime = 0; 

// ==========================================
// THE HTML/JS PHONE APP (Plus-Sign Layout)
// ==========================================
const char* htmlApp = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
  <title>RC Control</title>
  <style>
    body { 
      margin: 0; padding: 0; background-color: #111; color: white; 
      font-family: sans-serif; user-select: none; overscroll-behavior: none; 
      display: flex; justify-content: center; align-items: center; height: 100vh;
    }
    
    .d-pad {
      display: grid;
      grid-template-columns: 115px 115px 115px; 
      grid-template-rows: 115px 115px 115px;    
      gap: 5px; 
    }
    
    .btn { 
      background-color: #444; 
      display: flex; justify-content: center; align-items: center; 
      font-size: 18px; font-weight: bold;
      box-shadow: 0 8px 15px rgba(0,0,0,0.5);
      transition: transform 0.05s, background-color 0.05s; 
    }
    
    .btn.active { background-color: #00ff88; color: black; transform: scale(0.90); }

    #btn-F { grid-column: 2; grid-row: 1; border-radius: 20px 20px 0 0; } 
    #btn-L { grid-column: 1; grid-row: 2; border-radius: 20px 0 0 20px; } 
    #btn-R { grid-column: 3; grid-row: 2; border-radius: 0 20px 20px 0; } 
    #btn-B { grid-column: 2; grid-row: 2; } 
  </style>
</head>
<body>

  <div class="d-pad">
    <div class="btn" id="btn-F">FWD</div>
    <div class="btn" id="btn-L">LEFT</div>
    <div class="btn" id="btn-R">RIGHT</div>
    <div class="btn" id="btn-B">REV</div>
  </div>

  <script>
    var ws = new WebSocket('ws://' + window.location.hostname + ':81/');
    
    var isF = false;
    var isB = false;
    var isL = false;
    var isR = false;

    function sendState() {
      if(ws.readyState !== WebSocket.OPEN) return;
      
      var t = (isF ? 1 : 0) + (isB ? -1 : 0); 
      var s = (isR ? 1 : 0) + (isL ? -1 : 0); 

      if(t == 1 && s == 0) ws.send("F");
      else if(t == -1 && s == 0) ws.send("B");
      else if(t == 0 && s == -1) ws.send("L");
      else if(t == 0 && s == 1) ws.send("R");
      else if(t == 1 && s == -1) ws.send("FL");
      else if(t == 1 && s == 1) ws.send("FR");
      else if(t == -1 && s == -1) ws.send("BL");
      else if(t == -1 && s == 1) ws.send("BR");
      else ws.send("S"); 
    }

    setInterval(sendState, 100);

    function setupBtn(id, btnName) {
      var el = document.getElementById(id);
      
      el.addEventListener('touchstart', function(e) { 
        e.preventDefault(); 
        el.classList.add('active'); 
        if(btnName === 'F') isF = true;
        if(btnName === 'B') isB = true;
        if(btnName === 'L') isL = true;
        if(btnName === 'R') isR = true;
        sendState(); 
      });
      
      el.addEventListener('touchend', function(e) { 
        e.preventDefault(); 
        el.classList.remove('active');
        if(btnName === 'F') isF = false;
        if(btnName === 'B') isB = false;
        if(btnName === 'L') isL = false;
        if(btnName === 'R') isR = false;
        sendState(); 
      });
    }

    setupBtn('btn-F', 'F');
    setupBtn('btn-B', 'B');
    setupBtn('btn-L', 'L');
    setupBtn('btn-R', 'R');
  </script>
</body>
</html>
)rawliteral";

// ==========================================
// HARDWARE FUNCTIONS
// ==========================================

long getDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 20000); 
  if (duration == 0) return 999; 
  return duration * 0.034 / 2;
}

void stopMotor() {
  digitalWrite(MOTOR_IN1, LOW);
  digitalWrite(MOTOR_IN2, LOW);
  analogWrite(MOTOR_EN, 0); 
}

void drive(String dir) {
  if (dir.indexOf("L") != -1) steering.write(32);
  else if (dir.indexOf("R") != -1) steering.write(122);
  else steering.write(steerCenter);

  if (dir.indexOf("F") != -1) {
    digitalWrite(MOTOR_IN1, HIGH);
    digitalWrite(MOTOR_IN2, LOW);
    analogWrite(MOTOR_EN, carSpeed);
  } else if (dir.indexOf("B") != -1) {
    digitalWrite(MOTOR_IN1, LOW);
    digitalWrite(MOTOR_IN2, HIGH);
    analogWrite(MOTOR_EN, carSpeed);
  } else {
    stopMotor();
  }
}

// ==========================================
// WEBSOCKET EVENT HANDLER
// ==========================================
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  if (type == WStype_TEXT) {
    String command = (char*)payload;
    lastCommandTime = millis(); 
    drive(command);
  }
}

// ==========================================
// MAIN SETUP
// ==========================================
void setup() {
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

  server.on("/", []() {
    server.send(200, "text/html", htmlApp);
  });
  
  server.onNotFound([]() {
    server.send(200, "text/html", htmlApp);
  });
  
  server.begin();

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  Serial.println("System Ready!");
}

// ==========================================
// MAIN LOOP
// ==========================================
void loop() {
  dnsServer.processNextRequest(); 
  webSocket.loop();               
  server.handleClient();          

  // Dead Man's Switch (500ms timeout)
  if (millis() - lastCommandTime > 500) {
    stopMotor();
    steering.write(steerCenter);
  }

  // Safety Distance Check (Warning Alarm ONLY)
  long distance = getDistance();
  if (distance > 0 && distance < 4) {
    if (micros() - previousMicros >= 100) {
      previousMicros = micros();
      buzzerState = !buzzerState;
      digitalWrite(BUZZER, buzzerState); 
    }
    // Note: I deleted the two motor override lines from here!
  } else {
    digitalWrite(BUZZER, LOW);
  }
}